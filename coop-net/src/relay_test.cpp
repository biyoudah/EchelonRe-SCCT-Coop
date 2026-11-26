// Deux paires de sockets UDP locaux passent par scct-lan-relay. Pas une partie.
// Le canal de donnees recopie 10 et 12 octets. Le depot lit l'adresse renvoyee par le relais.

#include "relay_proto.h"

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

#ifndef SIO_UDP_CONNRESET
#define SIO_UDP_CONNRESET _WSAIOW(IOC_VENDOR, 12)
#endif

namespace {

constexpr unsigned short kControlPort = 27100;
constexpr unsigned short kDataPort = 27101;

struct Client {
    SOCKET sock = INVALID_SOCKET;
    sockaddr_in bound{};
    unsigned short port = 0;
};

struct Side {
    Client control;
    Client data;
    uint32_t id = 0;
    sockaddr_in peer{};
    bool peer_known = false;
};

int g_failures = 0;

void Fail(const char* text) {
    std::printf("echec=%s\n", text);
    std::fflush(stdout);
    ++g_failures;
}

void SilenceUdpReset(SOCKET sock) {
    DWORD bytes = 0;
    BOOL off = FALSE;
    WSAIoctl(sock, SIO_UDP_CONNRESET, &off, sizeof(off), nullptr, 0, &bytes, nullptr, nullptr);
}

bool OpenClient(Client* client) {
    client->sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (client->sock == INVALID_SOCKET) {
        return false;
    }
    SilenceUdpReset(client->sock);
    sockaddr_in local{};
    local.sin_family = AF_INET;
    local.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(client->sock, reinterpret_cast<sockaddr*>(&local), sizeof(local)) != 0) {
        return false;
    }
    int len = sizeof(client->bound);
    if (getsockname(client->sock, reinterpret_cast<sockaddr*>(&client->bound), &len) != 0) {
        return false;
    }
    client->port = ntohs(client->bound.sin_port);
    u_long nb = 1;
    ioctlsocket(client->sock, FIONBIO, &nb);
    return true;
}

sockaddr_in Dest(unsigned short port) {
    sockaddr_in dest{};
    dest.sin_family = AF_INET;
    dest.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    dest.sin_port = htons(port);
    return dest;
}

bool SendTo(const Client& client, unsigned short port, const void* data, int len) {
    const sockaddr_in dest = Dest(port);
    for (int attempt = 0; attempt < 20; ++attempt) {
        const int sent = sendto(client.sock, static_cast<const char*>(data), len, 0,
                                 reinterpret_cast<const sockaddr*>(&dest), sizeof(dest));
        if (sent == len) {
            return true;
        }
        if (WSAGetLastError() != WSAEWOULDBLOCK) {
            return false;
        }
        Sleep(5);
    }
    return false;
}

int RecvFrom(const Client& client, uint8_t* buf, int cap, int timeout_ms) {
    const DWORD start = GetTickCount();
    for (;;) {
        sockaddr_in src{};
        int src_len = sizeof(src);
        const int got = recvfrom(client.sock, reinterpret_cast<char*>(buf), cap, 0, reinterpret_cast<sockaddr*>(&src),
                                 &src_len);
        if (got >= 0) {
            return got;
        }
        const int err = WSAGetLastError();
        if (err != WSAEWOULDBLOCK && err != WSAECONNRESET) {
            return -1;
        }
        if (static_cast<int>(GetTickCount() - start) >= timeout_ms) {
            return -1;
        }
        fd_set read_set;
        FD_ZERO(&read_set);
        FD_SET(client.sock, &read_set);
        timeval wait{};
        wait.tv_sec = 0;
        wait.tv_usec = 20000;
        select(0, &read_set, nullptr, nullptr, &wait);
    }
}

void Drain(const Client& client) {
    uint8_t buf[2048];
    while (RecvFrom(client, buf, sizeof(buf), 0) >= 0) {
    }
}

void Heartbeat(const Side& side) {
    uint8_t packet[16];
    const int n = scct::relay::BuildHeartbeat(packet);
    SendTo(side.control, kControlPort, packet, n);
}

bool SameAddress(const uint8_t ip[4], uint16_t port_host, const sockaddr_in& expect) {
    return port_host == ntohs(expect.sin_port) && std::memcmp(ip, &expect.sin_addr, 4) == 0;
}

bool CapturePeer(const uint8_t* buf, int got, const sockaddr_in& expect, sockaddr_in* out) {
    uint32_t id = 0;
    uint8_t has = 0;
    uint8_t ip[4]{};
    uint16_t port = 0;
    uint16_t game_port = 0;
    bool ok = false;
    if (scct::relay::ParseRegisterOk(buf, got, &id, &has, ip, &port) && has == 1 && SameAddress(ip, port, expect)) {
        ok = true;
    } else if (scct::relay::ParseAddress(buf, got, ip, &port, &game_port) &&
               game_port == scct::relay::kPortDiscovery && SameAddress(ip, port, expect)) {
        ok = true;
    }
    if (!ok) {
        return false;
    }
    *out = {};
    out->sin_family = AF_INET;
    std::memcpy(&out->sin_addr, ip, 4);
    out->sin_port = htons(port);
    return true;
}

bool ReadId(const uint8_t* buf, int got, uint32_t* id) {
    uint8_t has = 0;
    uint8_t ip[4]{};
    uint16_t port = 0;
    return scct::relay::ParseRegisterOk(buf, got, id, &has, ip, &port);
}

bool RegisterBoth(Side& first, Side& second) {
    uint8_t packet[16];
    const int n = scct::relay::BuildRegister(packet, scct::relay::kPortDiscovery);
    if (!SendTo(first.control, kControlPort, packet, n) || !SendTo(second.control, kControlPort, packet, n)) {
        Fail("register envoi");
        return false;
    }
    const DWORD id_start = GetTickCount();
    while (GetTickCount() - id_start < 2000 && (first.id == 0 || second.id == 0)) {
        uint8_t buf[128];
        const int got_a = RecvFrom(first.control, buf, sizeof(buf), 40);
        uint32_t id = 0;
        if (got_a > 0 && ReadId(buf, got_a, &id)) {
            first.id = id;
        }
        const int got_b = RecvFrom(second.control, buf, sizeof(buf), 40);
        if (got_b > 0 && ReadId(buf, got_b, &id)) {
            second.id = id;
        }
    }
    if (first.id == 0 || second.id == 0) {
        Fail("identifiant");
        return false;
    }
    uint8_t attach_first[16];
    uint8_t attach_second[16];
    const int n_first = scct::relay::BuildAttach(attach_first, first.id);
    const int n_second = scct::relay::BuildAttach(attach_second, second.id);
    if (!SendTo(first.data, kControlPort, attach_first, n_first) ||
        !SendTo(second.data, kControlPort, attach_second, n_second)) {
        Fail("attach envoi");
        return false;
    }
    const DWORD start = GetTickCount();
    while (GetTickCount() - start < 2000 && !(first.peer_known && second.peer_known)) {
        uint8_t buf[128];
        const int got_a = RecvFrom(first.control, buf, sizeof(buf), 40);
        if (got_a > 0 && CapturePeer(buf, got_a, second.data.bound, &first.peer)) {
            first.peer_known = true;
        }
        const int got_b = RecvFrom(second.control, buf, sizeof(buf), 40);
        if (got_b > 0 && CapturePeer(buf, got_b, first.data.bound, &second.peer)) {
            second.peer_known = true;
        }
        if (!first.peer_known) {
            Heartbeat(first);
            SendTo(first.data, kControlPort, attach_first, n_first);
        }
        if (!second.peer_known) {
            Heartbeat(second);
            SendTo(second.data, kControlPort, attach_second, n_second);
        }
        Sleep(100);
    }
    std::printf("echange=%d port_donnees_a=%u port_donnees_b=%u\n", first.peer_known && second.peer_known ? 1 : 0,
                first.data.port, second.data.port);
    std::fflush(stdout);
    if (!first.peer_known || !second.peer_known) {
        Fail("echange d'adresse");
        return false;
    }
    Heartbeat(first);
    Heartbeat(second);
    return true;
}

bool ExpectSame(Side& sender, Side& receiver, const uint8_t* data, int len, const char* name) {
    Drain(receiver.data);
    if (!SendTo(sender.data, kDataPort, data, len)) {
        Fail(name);
        return false;
    }
    uint8_t buf[4096];
    const DWORD start = GetTickCount();
    while (GetTickCount() - start < 1000) {
        const int got = RecvFrom(receiver.data, buf, sizeof(buf), 200);
        if (got < 0) {
            continue;
        }
        if (got == len && std::memcmp(buf, data, static_cast<size_t>(len)) == 0) {
            return true;
        }
        std::printf("echec=%s len_envoye=%d len_recu=%d\n", name, len, got);
        std::fflush(stdout);
        ++g_failures;
        return false;
    }
    Fail(name);
    return false;
}

bool SameBytes(Side& sender, Side& receiver) {
    uint8_t ten[10];
    uint8_t twelve[12];
    std::memset(ten, 0xAB, sizeof(ten));
    std::memset(twelve, 0x5A, sizeof(twelve));
    uint8_t shaped_beat[16];
    uint8_t shaped_reg[16];
    const int beat_n = scct::relay::BuildHeartbeat(shaped_beat);
    const int reg_n = scct::relay::BuildRegister(shaped_reg, scct::relay::kPortDiscovery);
    const bool ten_ok = ExpectSame(sender, receiver, ten, static_cast<int>(sizeof(ten)), "payload 10");
    const bool twelve_ok = ExpectSame(sender, receiver, twelve, static_cast<int>(sizeof(twelve)), "payload 12");
    const bool beat_ok = ExpectSame(sender, receiver, shaped_beat, beat_n, "payload forme heartbeat");
    const bool reg_ok = ExpectSame(sender, receiver, shaped_reg, reg_n, "payload forme register");
    std::printf("octet_10=%d octet_12=%d forme_10=%d forme_12=%d identiques=%d\n", ten_ok ? 1 : 0, twelve_ok ? 1 : 0,
                beat_ok ? 1 : 0, reg_ok ? 1 : 0, ten_ok && twelve_ok && beat_ok && reg_ok ? 1 : 0);
    std::fflush(stdout);
    return ten_ok && twelve_ok && beat_ok && reg_ok;
}

using DepositFn = int (*)(const void*, int, const sockaddr*, int, void*, int, sockaddr*, int*);

bool DepositBlocking(const Side& side, const sockaddr_in& expect, const std::string& dll_path) {
    const HMODULE dll = LoadLibraryA(dll_path.c_str());
    if (dll == nullptr) {
        Fail("chargement dll");
        return false;
    }
    const auto deposit = reinterpret_cast<DepositFn>(GetProcAddress(dll, "ScctCoopNet_Deposit"));
    if (deposit == nullptr) {
        FreeLibrary(dll);
        Fail("symbole depot");
        return false;
    }
    uint8_t payload[12];
    for (int i = 0; i < 12; ++i) {
        payload[i] = static_cast<uint8_t>(0x30 + i);
    }
    uint8_t original[12];
    std::memcpy(original, payload, sizeof(original));
    uint8_t buf[12];
    std::memset(buf, 0xCC, sizeof(buf));
    sockaddr_in from{};
    from.sin_family = AF_INET;
    from.sin_port = htons(9);
    from.sin_addr.s_addr = htonl(0x00000001);
    int fromlen = static_cast<int>(sizeof(from));
    const int n = deposit(payload, static_cast<int>(sizeof(payload)), reinterpret_cast<const sockaddr*>(&side.peer),
                          static_cast<int>(sizeof(side.peer)), buf, static_cast<int>(sizeof(buf)),
                          reinterpret_cast<sockaddr*>(&from), &fromlen);
    const bool bytes = n == static_cast<int>(sizeof(payload)) && std::memcmp(buf, payload, sizeof(payload)) == 0 &&
                       std::memcmp(payload, original, sizeof(payload)) == 0;
    const bool address = fromlen == static_cast<int>(sizeof(sockaddr_in)) && from.sin_family == AF_INET &&
                         from.sin_port == expect.sin_port && from.sin_addr.s_addr == expect.sin_addr.s_addr;
    const unsigned char* ip = reinterpret_cast<const unsigned char*>(&from.sin_addr);
    std::printf("recvfrom_bloquant=%d depot_octets=%d depot_port=%u port_pair=%u depot_ip=%u.%u.%u.%u\n",
                bytes && address ? 1 : 0, bytes ? 1 : 0, ntohs(from.sin_port), ntohs(expect.sin_port), ip[0], ip[1],
                ip[2], ip[3]);
    std::fflush(stdout);
    FreeLibrary(dll);
    if (!bytes || !address) {
        Fail("depot");
        return false;
    }
    return true;
}

bool Rate(Side& sender, Side& receiver) {
    Sleep(1200);
    Heartbeat(sender);
    Heartbeat(receiver);
    Sleep(50);
    Drain(sender.control);
    Drain(sender.data);
    Drain(receiver.control);
    Drain(receiver.data);
    for (int i = 0; i < 7; ++i) {
        const uint8_t byte = static_cast<uint8_t>('A' + i);
        if (!SendTo(sender.data, kDataPort, &byte, 1)) {
            Fail("debit envoi");
            return false;
        }
    }
    bool seen[7] = {};
    int served = 0;
    const DWORD start = GetTickCount();
    uint8_t buf[128];
    while (GetTickCount() - start < 800) {
        const int got = RecvFrom(receiver.data, buf, sizeof(buf), 50);
        if (got == 1 && buf[0] >= 'A' && buf[0] < 'A' + 7) {
            const int index = buf[0] - 'A';
            if (!seen[index]) {
                seen[index] = true;
                ++served;
            }
        }
    }
    int refused = 0;
    const DWORD refuse_start = GetTickCount();
    while (GetTickCount() - refuse_start < 500) {
        const int got = RecvFrom(sender.control, buf, sizeof(buf), 50);
        uint16_t reason = 0;
        if (got > 0 && scct::relay::ParseRefused(buf, got, &reason) && reason == scct::relay::kRefuseRate) {
            ++refused;
        }
    }
    const bool letters = seen[0] && seen[1] && seen[2] && seen[3] && !seen[4] && !seen[5] && !seen[6];
    std::printf("debit_servis=%d debit_refus=%d\n", served, refused);
    std::fflush(stdout);
    if (!letters || served != 4 || refused < 3) {
        Fail("debit");
        return false;
    }
    return true;
}

bool Expire(Side& sender, Side& receiver) {
    Sleep(6000);
    Drain(sender.control);
    Drain(sender.data);
    Drain(receiver.control);
    Drain(receiver.data);
    const uint8_t mark = 0xEE;
    if (!SendTo(sender.data, kDataPort, &mark, 1)) {
        Fail("expire envoi");
        return false;
    }
    bool delivered = false;
    int reason = -1;
    const DWORD start = GetTickCount();
    uint8_t buf[128];
    while (GetTickCount() - start < 500) {
        const int got_b = RecvFrom(receiver.data, buf, sizeof(buf), 40);
        if (got_b == 1 && buf[0] == 0xEE) {
            delivered = true;
        }
        const int got_a = RecvFrom(sender.control, buf, sizeof(buf), 40);
        uint16_t why = 0;
        if (got_a > 0 && scct::relay::ParseRefused(buf, got_a, &why)) {
            reason = why;
        }
    }
    std::printf("expire_non_servi=%d raison_expire=%d\n", delivered ? 0 : 1, reason);
    std::fflush(stdout);
    if (delivered || reason != scct::relay::kRefuseUnknown) {
        Fail("expiration");
        return false;
    }
    return true;
}

void PrintRelayLog(const std::string& path) {
    HANDLE file = CreateFileA(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                              OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        std::printf("relais_journal=absent\n");
        return;
    }
    char chunk[4096];
    DWORD got = 0;
    ReadFile(file, chunk, sizeof(chunk) - 1, &got, nullptr);
    CloseHandle(file);
    chunk[got] = '\0';
    std::printf("relais_journal<<\n%srelais_journal>>\n", chunk);
    std::fflush(stdout);
}

void CloseSide(Side& side) {
    if (side.control.sock != INVALID_SOCKET) {
        closesocket(side.control.sock);
    }
    if (side.data.sock != INVALID_SOCKET) {
        closesocket(side.data.sock);
    }
}

bool SawRelayPeer(const uint8_t* buf, int got) {
    uint32_t id = 0;
    uint8_t has = 0;
    uint8_t ip[4]{};
    uint16_t port = 0;
    uint16_t game_port = 0;
    if (scct::relay::ParseRegisterOk(buf, got, &id, &has, ip, &port) && has == 1 && port != 0) {
        return true;
    }
    return scct::relay::ParseAddress(buf, got, ip, &port, &game_port) && game_port == scct::relay::kPortDiscovery &&
           port != 0;
}

bool OverlappedSame(const std::string& relay_path, const std::string& dir, const std::string& dll_path) {
    constexpr unsigned short kListen = 27110;
    constexpr unsigned short kRelayData = 27111;
    const std::string log_path = dir + "\\relay-overlap-out.txt";
    char command[1200];
    std::snprintf(command, sizeof(command), "\"%s\" --listen 127.0.0.1:%u --expire 30 --rate 20", relay_path.c_str(),
                  kListen);
    std::printf("commande_croise=%s\n", command);
    std::fflush(stdout);

    SECURITY_ATTRIBUTES inherit{};
    inherit.nLength = sizeof(inherit);
    inherit.bInheritHandle = TRUE;
    HANDLE log = CreateFileA(log_path.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &inherit, CREATE_ALWAYS,
                             FILE_ATTRIBUTE_NORMAL, nullptr);
    STARTUPINFOA startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESHOWWINDOW;
    startup.wShowWindow = SW_HIDE;
    if (log != INVALID_HANDLE_VALUE) {
        startup.dwFlags |= STARTF_USESTDHANDLES;
        startup.hStdOutput = log;
        startup.hStdError = log;
    }
    PROCESS_INFORMATION process{};
    if (!CreateProcessA(relay_path.c_str(), command, nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, dir.c_str(),
                        &startup, &process)) {
        if (log != INVALID_HANDLE_VALUE) {
            CloseHandle(log);
        }
        Fail("lancement relais croise");
        return false;
    }
    if (log != INVALID_HANDLE_VALUE) {
        CloseHandle(log);
    }
    Sleep(200);

    bool ok = false;
    const HMODULE dll = LoadLibraryA(dll_path.c_str());
    using AttachFn = int (*)(const char*, unsigned short);
    const auto attach = dll ? reinterpret_cast<AttachFn>(GetProcAddress(dll, "ScctCoopNet_AttachRelay")) : nullptr;
    Side peer{};
    SOCKET game = INVALID_SOCKET;
    WSAEVENT event = WSA_INVALID_EVENT;
    if (attach == nullptr || attach("127.0.0.1", kListen) != 0) {
        Fail("attache dll");
    } else if (!OpenClient(&peer.control) || !OpenClient(&peer.data)) {
        Fail("sockets du pair");
    } else {
        uint8_t reg[16];
        const int reg_n = scct::relay::BuildRegister(reg, scct::relay::kPortDiscovery);
        SendTo(peer.control, kListen, reg, reg_n);
        uint32_t id = 0;
        const DWORD id_start = GetTickCount();
        while (id == 0 && GetTickCount() - id_start < 2000) {
            uint8_t buf[128];
            const int got = RecvFrom(peer.control, buf, sizeof(buf), 40);
            uint32_t read_id = 0;
            if (got > 0 && ReadId(buf, got, &read_id)) {
                id = read_id;
            }
        }
        uint8_t attach_packet[16];
        const int attach_n = id == 0 ? 0 : scct::relay::BuildAttach(attach_packet, id);
        if (id == 0 || !SendTo(peer.data, kListen, attach_packet, attach_n)) {
            Fail("attach du pair");
        } else {
            bool seen = false;
            const DWORD wait_peer = GetTickCount();
            while (!seen && GetTickCount() - wait_peer < 3000) {
                uint8_t buf[128];
                const int got = RecvFrom(peer.control, buf, sizeof(buf), 40);
                if (got > 0 && SawRelayPeer(buf, got)) {
                    seen = true;
                } else {
                    uint8_t beat[16];
                    const int beat_n = scct::relay::BuildHeartbeat(beat);
                    SendTo(peer.control, kListen, beat, beat_n);
                    SendTo(peer.data, kListen, attach_packet, attach_n);
                    Sleep(100);
                }
            }
            if (!seen) {
                Fail("echange avec la dll");
            } else {
                game = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
                SilenceUdpReset(game);
                sockaddr_in local{};
                local.sin_family = AF_INET;
                local.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
                local.sin_port = htons(scct::relay::kPortDiscovery);
                event = WSACreateEvent();
                WSAOVERLAPPED overlapped{};
                overlapped.hEvent = event;
                uint8_t got_bytes[11]{};
                WSABUF wsa_buf{};
                wsa_buf.buf = reinterpret_cast<char*>(got_bytes);
                wsa_buf.len = sizeof(got_bytes);
                sockaddr_in from{};
                from.sin_family = AF_INET;
                from.sin_port = htons(9);
                int fromlen = static_cast<int>(sizeof(from));
                DWORD flags = 0;
                DWORD ignored = 0;
                const bool bound = game != INVALID_SOCKET && event != WSA_INVALID_EVENT &&
                                   bind(game, reinterpret_cast<sockaddr*>(&local), sizeof(local)) == 0;
                const int posted = bound ? WSARecvFrom(game, &wsa_buf, 1, &ignored, &flags, reinterpret_cast<sockaddr*>(&from),
                                                       &fromlen, &overlapped, nullptr)
                                         : SOCKET_ERROR;
                const bool pending = posted == SOCKET_ERROR && WSAGetLastError() == WSA_IO_PENDING;
                std::printf("croise_attente=%d\n", pending ? 1 : 0);
                std::fflush(stdout);
                if (!bound) {
                    Fail("bind 9100");
                } else if (!pending) {
                    Fail("WSARecvFrom non croise");
                } else {
                    const uint8_t payload[11] = {'o', 'c', 't', 'e', 't', '-', 'p', 'a', 'i', 'r', '!'};
                    Sleep(300);
                    if (!SendTo(peer.data, kRelayData, payload, static_cast<int>(sizeof(payload)))) {
                        Fail("envoi croise");
                    } else {
                        DWORD transferred = 0;
                        DWORD out_flags = 0;
                        BOOL done = FALSE;
                        const DWORD poll = GetTickCount();
                        while (!done && GetTickCount() - poll < 3000) {
                            done = WSAGetOverlappedResult(game, &overlapped, &transferred, FALSE, &out_flags);
                            if (!done && WSAGetLastError() != WSA_IO_INCOMPLETE) {
                                break;
                            }
                            Sleep(10);
                        }
                        const bool bytes = done && transferred == sizeof(payload) &&
                                           std::memcmp(got_bytes, payload, sizeof(payload)) == 0;
                        const bool address = done && from.sin_family == AF_INET &&
                                             from.sin_port == htons(scct::relay::kPortDiscovery) &&
                                             from.sin_addr.s_addr == peer.data.bound.sin_addr.s_addr;
                        const unsigned char* ip = reinterpret_cast<const unsigned char*>(&from.sin_addr);
                        std::printf("croise_octets=%d croise_port=%u port_canal=%u croise_ip=%u.%u.%u.%u\n", bytes ? 1 : 0,
                                    ntohs(from.sin_port), scct::relay::kPortDiscovery, ip[0], ip[1], ip[2], ip[3]);
                        std::fflush(stdout);
                        if (!bytes || !address) {
                            Fail("completion croisee");
                        } else {
                            ok = true;
                        }
                    }
                }
            }
        }
    }
    if (game != INVALID_SOCKET) {
        closesocket(game);
    }
    if (event != WSA_INVALID_EVENT) {
        WSACloseEvent(event);
    }
    CloseSide(peer);
    TerminateProcess(process.hProcess, 0);
    WaitForSingleObject(process.hProcess, 2000);
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return ok;
}

}  // namespace

int main() {
    char self[MAX_PATH];
    const DWORD self_len = GetModuleFileNameA(nullptr, self, MAX_PATH);
    if (self_len == 0 || self_len >= MAX_PATH) {
        std::printf("resultat=echec\n");
        return 1;
    }
    std::string dir = self;
    const auto slash = dir.find_last_of("\\/");
    if (slash != std::string::npos) {
        dir.resize(slash);
    }
    const std::string relay_path = dir + "\\scct-lan-relay.exe";
    const std::string dll_path = dir + "\\ScctCoopNet.dll";
    const std::string log_path = dir + "\\relay-test-out.txt";
    char command[1200];
    std::snprintf(command, sizeof(command), "\"%s\" --listen 127.0.0.1:27100 --expire 4 --rate 4", relay_path.c_str());
    std::printf("commande=%s\n", command);
    std::fflush(stdout);

    SECURITY_ATTRIBUTES inherit{};
    inherit.nLength = sizeof(inherit);
    inherit.bInheritHandle = TRUE;
    HANDLE log = CreateFileA(log_path.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &inherit, CREATE_ALWAYS,
                             FILE_ATTRIBUTE_NORMAL, nullptr);
    STARTUPINFOA startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESHOWWINDOW;
    startup.wShowWindow = SW_HIDE;
    if (log != INVALID_HANDLE_VALUE) {
        startup.dwFlags |= STARTF_USESTDHANDLES;
        startup.hStdOutput = log;
        startup.hStdError = log;
    }
    PROCESS_INFORMATION process{};
    if (!CreateProcessA(relay_path.c_str(), command, nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, dir.c_str(),
                        &startup, &process)) {
        if (log != INVALID_HANDLE_VALUE) {
            CloseHandle(log);
        }
        std::printf("echec=lancement relais %lu\nresultat=echec\n", GetLastError());
        return 1;
    }
    if (log != INVALID_HANDLE_VALUE) {
        CloseHandle(log);
    }

    WSADATA wsa{};
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        TerminateProcess(process.hProcess, 1);
        CloseHandle(process.hThread);
        CloseHandle(process.hProcess);
        std::printf("resultat=echec\n");
        return 1;
    }
    Side first{};
    Side second{};
    const bool opened = OpenClient(&first.control) && OpenClient(&first.data) && OpenClient(&second.control) &&
                        OpenClient(&second.data);
    bool ok = false;
    if (!opened) {
        Fail("sockets locaux");
    } else {
        Sleep(200);
        ok = RegisterBoth(first, second) && DepositBlocking(first, second.data.bound, dll_path) &&
             SameBytes(first, second) && Rate(first, second) && Expire(first, second);
    }
    CloseSide(first);
    CloseSide(second);
    const bool crossed = OverlappedSame(relay_path, dir, dll_path);
    ok = ok && crossed;
    DWORD relay_code = STILL_ACTIVE;
    GetExitCodeProcess(process.hProcess, &relay_code);
    if (relay_code != STILL_ACTIVE) {
        std::printf("relais_code=%lu\n", relay_code);
    }
    PrintRelayLog(log_path);
    TerminateProcess(process.hProcess, 0);
    WaitForSingleObject(process.hProcess, 2000);
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    std::printf("resultat=%s\n", ok && g_failures == 0 ? "ok" : "echec");
    std::fflush(stdout);
    return ok && g_failures == 0 ? 0 : 1;
}
