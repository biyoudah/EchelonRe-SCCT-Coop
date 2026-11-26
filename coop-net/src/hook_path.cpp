// Aller-retour. A envoie, B lit un sockaddr au port du canal, B repond vers
// ce sockaddr, A recoit. Sans relais, les deux sens restent a len=0.
// Pas une partie. Ne lance pas splintercell3.

#include "relay_proto.h"

#define _WINSOCK_DEPRECATED_NO_WARNINGS
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#ifndef SIO_UDP_CONNRESET
#define SIO_UDP_CONNRESET _WSAIOW(IOC_VENDOR, 12)
#endif

namespace {

constexpr unsigned short kRelayPort = 27120;
constexpr int kProbe16 = 16;
constexpr int kAller10 = 10;
constexpr int kRetour12 = 12;

using AttachFn = int (*)(const char*, unsigned short);
using PeerFn = int (*)(unsigned short, sockaddr*, int*);
using StopFn = void (*)();

const char kProbe[] = "voie-9100-abcdef";
const char kReply16[] = "reponse-9100-ok!";
const char kAller[] = "aller9103!";
const char kRetour[] = "retour9103!!";

void Silence(SOCKET sock) {
    BOOL no = FALSE;
    DWORD bytes = 0;
    WSAIoctl(sock, SIO_UDP_CONNRESET, &no, sizeof(no), nullptr, 0, &bytes, nullptr, nullptr);
}

void IpText(uint32_t be, char* out, int cap) {
    const auto* raw = reinterpret_cast<const unsigned char*>(&be);
    std::snprintf(out, static_cast<size_t>(cap), "%u.%u.%u.%u", raw[0], raw[1], raw[2], raw[3]);
}

bool SendDatagram(SOCKET sock, const sockaddr_in& dest, const void* data, int n) {
    WSABUF buf{};
    buf.buf = reinterpret_cast<char*>(const_cast<void*>(data));
    buf.len = static_cast<ULONG>(n);
    DWORD sent = 0;
    return WSASendTo(sock, &buf, 1, &sent, 0, reinterpret_cast<const sockaddr*>(&dest), sizeof(dest), nullptr, nullptr) ==
               0 &&
           sent == static_cast<DWORD>(n);
}

int EndRole(int argc, char** argv) {
    if (argc < 6) {
        std::printf("pret=0\n");
        return 2;
    }
    const char* host = argv[2];
    const unsigned short relay_port = static_cast<unsigned short>(std::atoi(argv[3]));
    const char* bind_text = argv[4];
    const char* unicast = argv[5];
    WSADATA wsa{};
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        std::printf("pret=0\n");
        return 2;
    }
    char self[MAX_PATH];
    GetModuleFileNameA(nullptr, self, MAX_PATH);
    std::string dll = self;
    const auto slash = dll.find_last_of('\\');
    dll.resize(slash == std::string::npos ? 0 : slash);
    dll += "\\ScctCoopNet.dll";
    const HMODULE module = LoadLibraryA(dll.c_str());
    const auto attach = module ? reinterpret_cast<AttachFn>(GetProcAddress(module, "ScctCoopNet_AttachRelay")) : nullptr;
    const auto peer = module ? reinterpret_cast<PeerFn>(GetProcAddress(module, "ScctCoopNet_Peer")) : nullptr;
    const auto halt = module ? reinterpret_cast<StopFn>(GetProcAddress(module, "ScctCoopNet_Stop")) : nullptr;
    if (attach == nullptr || peer == nullptr || attach(host, relay_port) != 0) {
        std::printf("pret=0\n");
        std::fflush(stdout);
        return 2;
    }
    const unsigned long bind_ip = inet_addr(bind_text);
    const unsigned long far_ip = inet_addr(unicast);
    if (bind_ip == INADDR_NONE || far_ip == INADDR_NONE) {
        std::printf("pret=0\n");
        std::fflush(stdout);
        return 2;
    }
    auto bind_game = [&](unsigned short game_port) -> SOCKET {
        const SOCKET sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (sock == INVALID_SOCKET) {
            return INVALID_SOCKET;
        }
        Silence(sock);
        BOOL yes = TRUE;
        setsockopt(sock, SOL_SOCKET, SO_BROADCAST, reinterpret_cast<char*>(&yes), sizeof(yes));
        sockaddr_in local{};
        local.sin_family = AF_INET;
        local.sin_addr.s_addr = bind_ip;
        local.sin_port = htons(game_port);
        if (bind(sock, reinterpret_cast<sockaddr*>(&local), sizeof(local)) != 0) {
            closesocket(sock);
            return INVALID_SOCKET;
        }
        return sock;
    };
    const SOCKET discovery = bind_game(scct::relay::kPortDiscovery);
    const SOCKET session = bind_game(scct::relay::kPortSession);
    const SOCKET outbound = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (outbound != INVALID_SOCKET) {
        Silence(outbound);
        BOOL yes = TRUE;
        setsockopt(outbound, SOL_SOCKET, SO_BROADCAST, reinterpret_cast<char*>(&yes), sizeof(yes));
    }
    if (discovery == INVALID_SOCKET || session == INVALID_SOCKET || outbound == INVALID_SOCKET) {
        std::printf("pret=0 bind=%s\n", bind_text);
        std::fflush(stdout);
        return 2;
    }
    std::printf("pret=1 bind=%s:9100 bind_session=%s:9103\n", bind_text, bind_text);
    std::fflush(stdout);

    sockaddr_in seen[2]{};
    bool have_seen[2]{};

    char line[180];
    while (std::fgets(line, sizeof(line), stdin) != nullptr) {
        if (std::strncmp(line, "fin", 3) == 0) {
            break;
        }
        if (std::strncmp(line, "pair", 4) == 0) {
            sockaddr_in known{};
            int known_len = sizeof(known);
            const int a = peer(scct::relay::kPortDiscovery, reinterpret_cast<sockaddr*>(&known), &known_len);
            known_len = sizeof(known);
            const int b = peer(scct::relay::kPortSession, reinterpret_cast<sockaddr*>(&known), &known_len);
            std::printf("pair_9100=%d pair_9103=%d\n", a, b);
            std::fflush(stdout);
            continue;
        }
        if (std::strncmp(line, "stop", 4) == 0) {
            if (halt != nullptr) {
                halt();
            }
            std::printf("heartbeat=%d\n", halt != nullptr ? 0 : -1);
            std::fflush(stdout);
            continue;
        }
        if (std::strncmp(line, "probe9100", 9) == 0) {
            sockaddr_in dest{};
            dest.sin_family = AF_INET;
            dest.sin_addr.s_addr = htonl(INADDR_BROADCAST);
            dest.sin_port = htons(scct::relay::kPortDiscovery);
            const int ok = SendDatagram(outbound, dest, kProbe, kProbe16) ? 1 : 0;
            std::printf("envoi_probe9100=%d octets=%d\n", ok, kProbe16);
            std::fflush(stdout);
            continue;
        }
        if (std::strncmp(line, "probe10", 7) == 0) {
            sockaddr_in dest{};
            dest.sin_family = AF_INET;
            dest.sin_addr.s_addr = far_ip;
            dest.sin_port = htons(scct::relay::kPortSession);
            const int ok = SendDatagram(outbound, dest, kAller, kAller10) ? 1 : 0;
            std::printf("envoi_probe10=%d octets=%d vers=%s:9103\n", ok, kAller10, unicast);
            std::fflush(stdout);
            continue;
        }
        if (std::strncmp(line, "reply9100", 9) == 0 || std::strncmp(line, "reply12", 7) == 0) {
            const bool session_reply = std::strncmp(line, "reply12", 7) == 0;
            const int index = session_reply ? 1 : 0;
            const SOCKET sock = session_reply ? session : discovery;
            const void* data = session_reply ? static_cast<const void*>(kRetour) : static_cast<const void*>(kReply16);
            const int n = session_reply ? kRetour12 : kProbe16;
            if (!have_seen[index]) {
                std::printf("envoi_%s=0\n", session_reply ? "reply12" : "reply9100");
                std::fflush(stdout);
                continue;
            }
            char ip[32];
            IpText(seen[index].sin_addr.s_addr, ip, sizeof(ip));
            const int ok = SendDatagram(sock, seen[index], data, n) ? 1 : 0;
            std::printf("envoi_%s=%d octets=%d vers=%s:%u\n", session_reply ? "reply12" : "reply9100", ok, n, ip,
                        ntohs(seen[index].sin_port));
            std::fflush(stdout);
            continue;
        }
        if (std::strncmp(line, "purge ", 6) == 0) {
            const unsigned game = static_cast<unsigned>(std::atoi(line + 6));
            const SOCKET sock = game == scct::relay::kPortSession ? session : discovery;
            u_long nonblock = 1;
            ioctlsocket(sock, FIONBIO, &nonblock);
            int dropped = 0;
            for (;;) {
                char junk[2048];
                sockaddr_in from{};
                int fromlen = sizeof(from);
                const int n = recvfrom(sock, junk, sizeof(junk), 0, reinterpret_cast<sockaddr*>(&from), &fromlen);
                if (n < 0) {
                    break;
                }
                ++dropped;
            }
            nonblock = 0;
            ioctlsocket(sock, FIONBIO, &nonblock);
            std::printf("purge_%u=%d\n", game, dropped);
            std::fflush(stdout);
            continue;
        }
        if (std::strncmp(line, "vers ", 5) == 0) {
            unsigned game = 0;
            char ip_text[64]{};
            unsigned dest_port = 0;
            if (std::sscanf(line + 5, "%u %63s %u", &game, ip_text, &dest_port) != 3) {
                continue;
            }
            sockaddr_in dest{};
            dest.sin_family = AF_INET;
            dest.sin_addr.s_addr = inet_addr(ip_text);
            dest.sin_port = htons(static_cast<unsigned short>(dest_port));
            const bool session_reply = game == scct::relay::kPortSession;
            const SOCKET sock = session_reply ? session : discovery;
            const void* data = session_reply ? static_cast<const void*>(kRetour) : static_cast<const void*>(kReply16);
            const int n = session_reply ? kRetour12 : kProbe16;
            const int ok = SendDatagram(sock, dest, data, n) ? 1 : 0;
            std::printf("envoi_vers=%d octets=%d vers=%s:%u\n", ok, n, ip_text, dest_port);
            std::fflush(stdout);
            continue;
        }

        unsigned game = 0;
        unsigned timeout_ms = 0;
        char tag[16]{};
        if (std::sscanf(line, "arm %u %u %15s", &game, &timeout_ms, tag) != 3) {
            continue;
        }
        const uint8_t* expect = nullptr;
        int expect_n = 0;
        SOCKET sock = INVALID_SOCKET;
        int seen_index = 0;
        if (game == scct::relay::kPortDiscovery && std::strcmp(tag, "probe") == 0) {
            expect = reinterpret_cast<const uint8_t*>(kProbe);
            expect_n = kProbe16;
            sock = discovery;
        } else if (game == scct::relay::kPortDiscovery && std::strcmp(tag, "reply") == 0) {
            expect = reinterpret_cast<const uint8_t*>(kReply16);
            expect_n = kProbe16;
            sock = discovery;
        } else if (game == scct::relay::kPortSession && std::strcmp(tag, "aller") == 0) {
            expect = reinterpret_cast<const uint8_t*>(kAller);
            expect_n = kAller10;
            sock = session;
            seen_index = 1;
        } else if (game == scct::relay::kPortSession && std::strcmp(tag, "retour") == 0) {
            expect = reinterpret_cast<const uint8_t*>(kRetour);
            expect_n = kRetour12;
            sock = session;
            seen_index = 1;
        } else {
            continue;
        }

        uint8_t buf[64]{};
        sockaddr_in from{};
        int fromlen = sizeof(from);
        WSAOVERLAPPED overlapped{};
        overlapped.hEvent = WSACreateEvent();
        WSABUF wbuf{};
        wbuf.buf = reinterpret_cast<char*>(buf);
        wbuf.len = sizeof(buf);
        DWORD got = 0;
        DWORD flags = 0;
        const int posted = WSARecvFrom(sock, &wbuf, 1, &got, &flags, reinterpret_cast<sockaddr*>(&from), &fromlen,
                                        &overlapped, nullptr);
        const int pending = (posted != 0 && WSAGetLastError() == WSA_IO_PENDING) ? 1 : 0;
        std::printf("attente_%u=%d\n", game, pending);
        std::fflush(stdout);

        int len = 0;
        bool done = false;
        if (posted == 0) {
            len = static_cast<int>(got);
            done = true;
        } else if (pending) {
            const DWORD start = GetTickCount();
            while (GetTickCount() - start < timeout_ms) {
                DWORD transferred = 0;
                DWORD out_flags = 0;
                if (WSAGetOverlappedResult(sock, &overlapped, &transferred, FALSE, &out_flags)) {
                    len = static_cast<int>(transferred);
                    done = true;
                    break;
                }
                if (WSAGetLastError() != WSA_IO_INCOMPLETE) {
                    break;
                }
                Sleep(15);
            }
            if (!done) {
                CancelIo(reinterpret_cast<HANDLE>(sock));
                DWORD drained = 0;
                DWORD out_flags = 0;
                WSAGetOverlappedResult(sock, &overlapped, &drained, TRUE, &out_flags);
            }
        }
        int mem = 0;
        unsigned port_vu = 0;
        char ip[32] = "-";
        if (done && len > 0) {
            mem = (len == expect_n && std::memcmp(buf, expect, static_cast<size_t>(expect_n)) == 0) ? 1 : 0;
            port_vu = ntohs(from.sin_port);
            IpText(from.sin_addr.s_addr, ip, sizeof(ip));
            seen[seen_index] = from;
            seen[seen_index].sin_family = AF_INET;
            have_seen[seen_index] = true;
        }
        std::printf("resultat_%u attente=%d len=%d mem=%d port_vu=%u ip=%s tag=%s\n", game, pending, len, mem, port_vu, ip,
                    tag);
        std::fflush(stdout);
        WSACloseEvent(overlapped.hEvent);
    }
    return 0;
}

struct Child {
    PROCESS_INFORMATION pi{};
    HANDLE in_write = nullptr;
    HANDLE out_read = nullptr;
    std::string pending;
    bool alive = false;
};

void KillChild(Child* child) {
    if (child->in_write != nullptr) {
        CloseHandle(child->in_write);
        child->in_write = nullptr;
    }
    if (child->alive) {
        TerminateProcess(child->pi.hProcess, 1);
        WaitForSingleObject(child->pi.hProcess, 2000);
        CloseHandle(child->pi.hThread);
        CloseHandle(child->pi.hProcess);
        child->alive = false;
    }
    if (child->out_read != nullptr) {
        CloseHandle(child->out_read);
        child->out_read = nullptr;
    }
}

bool Spawn(Child* child, const std::string& exe, const std::string& args, const std::string& dir) {
    SECURITY_ATTRIBUTES inherit{};
    inherit.nLength = sizeof(inherit);
    inherit.bInheritHandle = TRUE;
    HANDLE out_read = nullptr;
    HANDLE out_write = nullptr;
    HANDLE in_read = nullptr;
    HANDLE in_write = nullptr;
    if (!CreatePipe(&out_read, &out_write, &inherit, 0) || !CreatePipe(&in_read, &in_write, &inherit, 0)) {
        return false;
    }
    SetHandleInformation(out_read, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(in_write, HANDLE_FLAG_INHERIT, 0);
    STARTUPINFOA startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    startup.wShowWindow = SW_HIDE;
    startup.hStdInput = in_read;
    startup.hStdOutput = out_write;
    startup.hStdError = out_write;
    std::string command = "\"" + exe + "\" " + args;
    if (!CreateProcessA(exe.c_str(), command.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, dir.c_str(),
                        &startup, &child->pi)) {
        CloseHandle(out_read);
        CloseHandle(out_write);
        CloseHandle(in_read);
        CloseHandle(in_write);
        return false;
    }
    CloseHandle(out_write);
    CloseHandle(in_read);
    child->in_write = in_write;
    child->out_read = out_read;
    child->alive = true;
    return true;
}

void WriteLine(HANDLE pipe, const char* text) {
    std::string line = std::string(text) + "\n";
    DWORD wrote = 0;
    WriteFile(pipe, line.data(), static_cast<DWORD>(line.size()), &wrote, nullptr);
}

std::string ReadLine(HANDLE pipe, std::string* pending, int timeout_ms) {
    const DWORD start = GetTickCount();
    while (static_cast<int>(GetTickCount() - start) < timeout_ms) {
        const auto pos = pending->find('\n');
        if (pos != std::string::npos) {
            std::string line = pending->substr(0, pos);
            pending->erase(0, pos + 1);
            if (!line.empty() && line.back() == '\r') {
                line.pop_back();
            }
            return line;
        }
        DWORD avail = 0;
        if (!PeekNamedPipe(pipe, nullptr, 0, nullptr, &avail, nullptr)) {
            return {};
        }
        if (avail == 0) {
            Sleep(15);
            continue;
        }
        char buf[256];
        DWORD got = 0;
        const DWORD chunk = avail > sizeof(buf) ? sizeof(buf) : avail;
        if (!ReadFile(pipe, buf, chunk, &got, nullptr) || got == 0) {
            return {};
        }
        pending->append(buf, got);
    }
    return {};
}

std::string ReadTagged(Child* child, const char* prefix, int timeout_ms, const char* who) {
    const DWORD start = GetTickCount();
    while (static_cast<int>(GetTickCount() - start) < timeout_ms) {
        const int left = timeout_ms - static_cast<int>(GetTickCount() - start);
        std::string line = ReadLine(child->out_read, &child->pending, left > 0 ? left : 1);
        if (line.empty()) {
            return {};
        }
        std::printf("%s %s\n", who, line.c_str());
        std::fflush(stdout);
        if (line.compare(0, std::strlen(prefix), prefix) == 0) {
            return line;
        }
    }
    return {};
}

struct Hit {
    int attente = 0;
    int len = 0;
    int mem = 0;
    unsigned port_vu = 0;
    bool line = false;
    std::string ip;
};

bool ParseHit(const std::string& text, Hit* hit) {
    unsigned attente = 0;
    unsigned len = 0;
    unsigned mem = 0;
    unsigned port_vu = 0;
    char ip[64]{};
    if (std::sscanf(text.c_str(), "resultat_%*u attente=%u len=%u mem=%u port_vu=%u ip=%63s", &attente, &len, &mem,
                    &port_vu, ip) != 5) {
        return false;
    }
    hit->attente = static_cast<int>(attente);
    hit->len = static_cast<int>(len);
    hit->mem = static_cast<int>(mem);
    hit->port_vu = port_vu;
    hit->ip = ip;
    hit->line = true;
    return true;
}

SOCKET HoldPort(unsigned short port) {
    const SOCKET sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock == INVALID_SOCKET) {
        return INVALID_SOCKET;
    }
    sockaddr_in local{};
    local.sin_family = AF_INET;
    local.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    local.sin_port = htons(port);
    if (bind(sock, reinterpret_cast<sockaddr*>(&local), sizeof(local)) != 0) {
        closesocket(sock);
        return INVALID_SOCKET;
    }
    return sock;
}

std::string ReadFileShare(const std::string& path) {
    HANDLE file = CreateFileA(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                              OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return {};
    }
    std::string text;
    char chunk[1024];
    DWORD got = 0;
    while (ReadFile(file, chunk, sizeof(chunk), &got, nullptr) && got > 0) {
        text.append(chunk, got);
    }
    CloseHandle(file);
    return text;
}

bool JournalLists(const std::string& log, unsigned port, unsigned canal) {
    char needle[80];
    std::snprintf(needle, sizeof(needle), "port=%u canal=%u", port, canal);
    return log.find(needle) != std::string::npos;
}

bool IniStillOff(const std::string& path) {
    const std::string text = ReadFileShare(path);
    std::string enabled = "?";
    std::string transport = "?";
    std::string relay = "?";
    std::string cur;
    for (size_t i = 0; i <= text.size(); ++i) {
        if (i == text.size() || text[i] == '\n') {
            if (!cur.empty() && cur.back() == '\r') {
                cur.pop_back();
            }
            if (cur.rfind("enabled=", 0) == 0 && cur[0] != ';') {
                enabled = cur.substr(8);
            } else if (cur.rfind("transport=", 0) == 0 && cur[0] != ';') {
                transport = cur.substr(10);
            } else if (cur.rfind("relay=", 0) == 0 && cur[0] != ';') {
                relay = cur.substr(6);
            }
            cur.clear();
        } else {
            cur.push_back(text[i]);
        }
    }
    std::printf("ini_enabled=%s ini_transport=%s ini_relay=%s\n", enabled.c_str(), transport.c_str(),
                relay.empty() ? "vide" : relay.c_str());
    return enabled == "0" && transport == "0" && relay.empty();
}

struct Ends {
    Child a{};
    Child b{};
    bool ready = false;
};

bool StartEnds(Ends* ends, const std::string& exe, const std::string& dir, const char* unicast, unsigned short relay_port) {
    char args_a[240];
    char args_b[240];
    std::snprintf(args_a, sizeof(args_a), "--end 127.0.0.1 %u 127.0.0.2 %s", relay_port, unicast);
    std::snprintf(args_b, sizeof(args_b), "--end 127.0.0.1 %u 127.0.0.1 %s", relay_port, unicast);
    if (!Spawn(&ends->a, exe, args_a, dir) || !Spawn(&ends->b, exe, args_b, dir)) {
        return false;
    }
    const std::string pret_a = ReadTagged(&ends->a, "pret=", 4000, "a");
    const std::string pret_b = ReadTagged(&ends->b, "pret=", 4000, "b");
    ends->ready = pret_a.find("pret=1") != std::string::npos && pret_b.find("pret=1") != std::string::npos;
    return ends->ready;
}

void StopEnds(Ends* ends) {
    if (ends->a.in_write) {
        WriteLine(ends->a.in_write, "fin");
    }
    if (ends->b.in_write) {
        WriteLine(ends->b.in_write, "fin");
    }
    KillChild(&ends->a);
    KillChild(&ends->b);
}

bool Exchange(Child* waiter, const char* waiter_name, Child* sender, const char* sender_name, const char* arm,
              const char* go, const char* go_prefix, int wait_ms, Hit* hit) {
    WriteLine(waiter->in_write, arm);
    const std::string attente = ReadTagged(waiter, "attente_", 3000, waiter_name);
    if (attente.empty()) {
        return false;
    }
    WriteLine(sender->in_write, go);
    if (ReadTagged(sender, go_prefix, 3000, sender_name).empty()) {
        return false;
    }
    const std::string result = ReadTagged(waiter, "resultat_", wait_ms + 1500, waiter_name);
    return ParseHit(result, hit);
}

bool WaitPeers(Ends* ends) {
    const DWORD start = GetTickCount();
    while (GetTickCount() - start < 6000) {
        WriteLine(ends->a.in_write, "pair");
        WriteLine(ends->b.in_write, "pair");
        const std::string line_a = ReadTagged(&ends->a, "pair_", 1500, "a");
        const std::string line_b = ReadTagged(&ends->b, "pair_", 1500, "b");
        int a0 = 0;
        int a1 = 0;
        int b0 = 0;
        int b1 = 0;
        std::sscanf(line_a.c_str(), "pair_9100=%d pair_9103=%d", &a0, &a1);
        std::sscanf(line_b.c_str(), "pair_9100=%d pair_9103=%d", &b0, &b1);
        if (a0 == 1 && a1 == 1 && b0 == 1 && b1 == 1) {
            return true;
        }
        Sleep(200);
    }
    return false;
}

int RunVie(const std::string& exe, const std::string& dir, const std::string& relay_path) {
    constexpr unsigned short kPort = 27140;
    const char* unicast = "203.0.113.9";
    const std::string log_path = dir + "\\hook-vie-relay.txt";
    char command[1200];
    std::snprintf(command, sizeof(command), "\"%s\" --listen 127.0.0.1:%u --expire 4 --rate 40", relay_path.c_str(), kPort);
    std::printf("commande=%s\n", command);
    std::fflush(stdout);

    SECURITY_ATTRIBUTES inherit{};
    inherit.nLength = sizeof(inherit);
    inherit.bInheritHandle = TRUE;
    HANDLE relay_log = CreateFileA(log_path.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &inherit, CREATE_ALWAYS,
                                   FILE_ATTRIBUTE_NORMAL, nullptr);
    STARTUPINFOA startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESHOWWINDOW;
    startup.wShowWindow = SW_HIDE;
    if (relay_log != INVALID_HANDLE_VALUE) {
        startup.dwFlags |= STARTF_USESTDHANDLES;
        startup.hStdOutput = relay_log;
        startup.hStdError = relay_log;
    }
    PROCESS_INFORMATION relay{};
    const bool relay_alive =
        CreateProcessA(relay_path.c_str(), command, nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, dir.c_str(), &startup,
                       &relay) != 0;
    if (relay_log != INVALID_HANDLE_VALUE) {
        CloseHandle(relay_log);
    }
    Sleep(250);

    const DWORD origin = GetTickCount();
    Ends ends{};
    Hit first_9100{};
    Hit second_9100{};
    Hit first_9103{};
    Hit second_9103{};
    Hit after_stop{};
    DWORD silence_9100 = 0;
    DWORD silence_9103 = 0;
    DWORD silence_stop = 0;
    const bool ready = relay_alive && StartEnds(&ends, exe, dir, unicast, kPort);
    const bool peers = ready && WaitPeers(&ends);
    bool exchanged = false;
    if (peers && Exchange(&ends.b, "b", &ends.a, "a", "arm 9100 3000 probe", "probe9100", "envoi_probe9100=", 3000,
                          &first_9100)) {
        std::printf("t_premier_9100_ms=%lu\n", GetTickCount() - origin);
        std::fflush(stdout);
        const DWORD mark = GetTickCount();
        Sleep(6100);
        silence_9100 = GetTickCount() - mark;
        std::printf("silence_9100_ms=%lu\n", silence_9100);
        std::fflush(stdout);
        if (Exchange(&ends.b, "b", &ends.a, "a", "arm 9100 3000 probe", "probe9100", "envoi_probe9100=", 3000,
                     &second_9100)) {
            std::printf("t_second_9100_ms=%lu\n", GetTickCount() - origin);
            std::fflush(stdout);
            if (Exchange(&ends.b, "b", &ends.a, "a", "arm 9103 3000 aller", "probe10", "envoi_probe10=", 3000,
                         &first_9103)) {
                const DWORD mark_session = GetTickCount();
                Sleep(6100);
                silence_9103 = GetTickCount() - mark_session;
                std::printf("silence_9103_ms=%lu\n", silence_9103);
                std::fflush(stdout);
                exchanged = Exchange(&ends.b, "b", &ends.a, "a", "arm 9103 3000 aller", "probe10", "envoi_probe10=", 3000,
                                     &second_9103);
                if (exchanged) {
                    std::printf("t_second_9103_ms=%lu\n", GetTickCount() - origin);
                    std::fflush(stdout);
                }
            }
        }
    }
    const std::string before = ReadFileShare(log_path);
    const bool still_alive = exchanged && before.find("expire ip=") == std::string::npos;
    std::printf("expire_pendant_silence=%d\n", still_alive ? 0 : 1);
    std::fflush(stdout);

    bool stopped = false;
    if (ends.a.in_write != nullptr) {
        WriteLine(ends.a.in_write, "stop");
        const std::string line = ReadTagged(&ends.a, "heartbeat=", 3000, "a");
        stopped = line.find("heartbeat=0") != std::string::npos;
        std::printf("t_arret_a_ms=%lu\n", GetTickCount() - origin);
        std::fflush(stdout);
    }
    const DWORD mark_stop = GetTickCount();
    Sleep(5100);
    silence_stop = GetTickCount() - mark_stop;
    std::printf("silence_apres_arret_ms=%lu\n", silence_stop);
    std::fflush(stdout);
    const bool failed_delivery =
        stopped && Exchange(&ends.b, "b", &ends.a, "a", "arm 9100 2000 probe", "probe9100", "envoi_probe9100=", 2000,
                            &after_stop) &&
        after_stop.attente == 1 && after_stop.len == 0;
    const std::string journal = ReadFileShare(log_path);
    std::printf("relais_journal<<\n%srelais_journal>>\n", journal.c_str());
    const bool alive_ok = still_alive && silence_9100 >= 6000 && silence_9103 >= 6000 && first_9100.attente == 1 &&
                          first_9100.len == 16 && first_9100.mem == 1 && first_9100.port_vu == 9100 &&
                          second_9100.attente == 1 && second_9100.len == 16 && second_9100.mem == 1 &&
                          second_9100.port_vu == 9100 && first_9103.attente == 1 && first_9103.len == 10 &&
                          first_9103.mem == 1 && first_9103.port_vu == 9103 && second_9103.attente == 1 &&
                          second_9103.len == 10 && second_9103.mem == 1 && second_9103.port_vu == 9103;
    const bool dead_ok = failed_delivery && silence_stop >= 5000 && journal.find("expire ip=") != std::string::npos &&
                         journal.find("raison=4") != std::string::npos;
    std::printf("survit_9100=%d survit_9103=%d apres_expiration=%d raison4=%d\n",
                first_9100.mem == 1 && second_9100.mem == 1 && second_9100.port_vu == 9100,
                first_9103.mem == 1 && second_9103.mem == 1 && second_9103.port_vu == 9103, failed_delivery ? 1 : 0,
                journal.find("raison=4") != std::string::npos ? 1 : 0);
    StopEnds(&ends);
    if (relay_alive) {
        TerminateProcess(relay.hProcess, 1);
        WaitForSingleObject(relay.hProcess, 2000);
        CloseHandle(relay.hThread);
        CloseHandle(relay.hProcess);
    }
    const bool ini_ok = IniStillOff("D:\\Jeux\\Tom Clancys Splinter Cell Chaos Theory\\System\\scct-coop-net.ini");
    const bool ok = alive_ok && dead_ok && ini_ok;
    std::printf("resultat=%s\n", ok ? "ok" : "echec");
    std::fflush(stdout);
    return ok ? 0 : 1;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc >= 2 && std::strcmp(argv[1], "--end") == 0) {
        return EndRole(argc, argv);
    }
    if (argc >= 2 && std::strcmp(argv[1], "--vie") == 0) {
        char self_vie[MAX_PATH];
        GetModuleFileNameA(nullptr, self_vie, MAX_PATH);
        std::string dir_vie = self_vie;
        const auto slash_vie = dir_vie.find_last_of('\\');
        dir_vie.resize(slash_vie == std::string::npos ? 0 : slash_vie);
        return RunVie(dir_vie + "\\scct-hook-path.exe", dir_vie, dir_vie + "\\scct-lan-relay.exe");
    }
    const char* unicast = argc >= 2 ? argv[1] : "203.0.113.9";
    char self[MAX_PATH];
    GetModuleFileNameA(nullptr, self, MAX_PATH);
    std::string dir = self;
    const auto slash = dir.find_last_of('\\');
    dir.resize(slash == std::string::npos ? 0 : slash);
    const std::string exe = dir + "\\scct-hook-path.exe";
    const std::string relay_path = dir + "\\scct-lan-relay.exe";
    const std::string log_path = dir + "\\hook-path-relay.txt";

    WSADATA wsa{};
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        std::printf("resultat=echec\n");
        return 1;
    }
    std::printf("unicast=%s relais=127.0.0.1:%u bind_a=127.0.0.2 bind_b=127.0.0.1\n", unicast, kRelayPort);
    std::fflush(stdout);

    bool sans_failed = false;
    {
        const SOCKET hold_control = HoldPort(kRelayPort);
        const SOCKET hold_data = HoldPort(static_cast<unsigned short>(kRelayPort + 1));
        if (hold_control == INVALID_SOCKET || hold_data == INVALID_SOCKET) {
            std::printf("phase=sans_relais port_occupe=1\nresultat=echec\n");
            if (hold_control != INVALID_SOCKET) {
                closesocket(hold_control);
            }
            if (hold_data != INVALID_SOCKET) {
                closesocket(hold_data);
            }
            return 1;
        }
        std::printf("phase=sans_relais relais_lance=0\n");
        std::fflush(stdout);
        Ends ends{};
        Hit aller_9100{};
        Hit retour_9100{};
        Hit aller_9103{};
        Hit retour_9103{};
        const bool ready = StartEnds(&ends, exe, dir, unicast, kRelayPort);
        if (ready) {
            Exchange(&ends.b, "b", &ends.a, "a", "arm 9100 1000 probe", "probe9100", "envoi_probe9100=", 1000, &aller_9100);
            WriteLine(ends.a.in_write, "purge 9100");
            ReadTagged(&ends.a, "purge_", 2000, "a");
            Exchange(&ends.a, "a", &ends.b, "b", "arm 9100 1000 reply", "vers 9100 127.0.0.1 9100", "envoi_vers=", 1000,
                     &retour_9100);
            Exchange(&ends.b, "b", &ends.a, "a", "arm 9103 1000 aller", "probe10", "envoi_probe10=", 1000, &aller_9103);
            WriteLine(ends.a.in_write, "purge 9103");
            ReadTagged(&ends.a, "purge_", 2000, "a");
            Exchange(&ends.a, "a", &ends.b, "b", "arm 9103 1000 retour", "vers 9103 127.0.0.1 9103", "envoi_vers=", 1000,
                     &retour_9103);
        }
        StopEnds(&ends);
        closesocket(hold_control);
        closesocket(hold_data);
        sans_failed = ready && aller_9100.attente == 1 && aller_9100.len == 0 && retour_9100.attente == 1 &&
                      retour_9100.len == 0 && aller_9103.attente == 1 && aller_9103.len == 0 && retour_9103.attente == 1 &&
                      retour_9103.len == 0;
        std::printf("sans_relais=%s\n", sans_failed ? "echec" : "recu");
        std::fflush(stdout);
    }

    bool avec_ok = false;
    PROCESS_INFORMATION relay{};
    bool relay_alive = false;
    {
        char command[1200];
        std::snprintf(command, sizeof(command), "\"%s\" --listen 127.0.0.1:%u --expire 30 --rate 40", relay_path.c_str(),
                      kRelayPort);
        std::printf("phase=avec_relais\ncommande=%s\n", command);
        std::fflush(stdout);
        SECURITY_ATTRIBUTES inherit{};
        inherit.nLength = sizeof(inherit);
        inherit.bInheritHandle = TRUE;
        HANDLE relay_log = CreateFileA(log_path.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &inherit,
                                       CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        STARTUPINFOA startup{};
        startup.cb = sizeof(startup);
        startup.dwFlags = STARTF_USESHOWWINDOW;
        startup.wShowWindow = SW_HIDE;
        if (relay_log != INVALID_HANDLE_VALUE) {
            startup.dwFlags |= STARTF_USESTDHANDLES;
            startup.hStdOutput = relay_log;
            startup.hStdError = relay_log;
        }
        if (CreateProcessA(relay_path.c_str(), command, nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, dir.c_str(),
                           &startup, &relay)) {
            relay_alive = true;
        }
        if (relay_log != INVALID_HANDLE_VALUE) {
            CloseHandle(relay_log);
        }
        Sleep(250);

        Ends ends{};
        Hit aller_9100{};
        Hit retour_9100{};
        Hit aller_9103{};
        Hit retour_9103{};
        const bool ready = relay_alive && StartEnds(&ends, exe, dir, unicast, kRelayPort);
        const bool peers = ready && WaitPeers(&ends);
        const bool exchanged =
            peers && Exchange(&ends.b, "b", &ends.a, "a", "arm 9100 4000 probe", "probe9100", "envoi_probe9100=", 4000,
                              &aller_9100) &&
            (WriteLine(ends.a.in_write, "purge 9100"), !ReadTagged(&ends.a, "purge_", 2000, "a").empty()) &&
            Exchange(&ends.a, "a", &ends.b, "b", "arm 9100 4000 reply", "reply9100", "envoi_reply9100=", 4000,
                     &retour_9100) &&
            Exchange(&ends.b, "b", &ends.a, "a", "arm 9103 4000 aller", "probe10", "envoi_probe10=", 4000, &aller_9103) &&
            (WriteLine(ends.a.in_write, "purge 9103"), !ReadTagged(&ends.a, "purge_", 2000, "a").empty()) &&
            Exchange(&ends.a, "a", &ends.b, "b", "arm 9103 4000 retour", "reply12", "envoi_reply12=", 4000, &retour_9103);
        const std::string journal = ReadFileShare(log_path);
        std::printf("relais_journal<<\n%srelais_journal>>\n", journal.c_str());
        // Quazal PRUDP validates incoming replies on port 9103 against the outbound unicast peer IP.
        const bool ports = exchanged && aller_9100.mem == 1 && aller_9100.len == 16 && aller_9100.port_vu == 9100 &&
                           aller_9100.ip == "127.0.0.1" && retour_9100.mem == 1 && retour_9100.len == 16 &&
                           retour_9100.port_vu == 9100 && retour_9100.ip == "127.0.0.1" && aller_9103.mem == 1 &&
                           aller_9103.len == 10 && aller_9103.port_vu == 9103 && aller_9103.ip == "127.0.0.1" &&
                           retour_9103.mem == 1 && retour_9103.len == 12 && retour_9103.port_vu == 9103 &&
                           retour_9103.ip == unicast && !JournalLists(journal, 9100, 9100) &&
                           !JournalLists(journal, 9103, 9103) && journal.find("canal=9100") != std::string::npos &&
                           journal.find("canal=9103") != std::string::npos;
        std::printf("aller_9100=%d retour_9100=%d aller_9103=%d retour_9103=%d\n",
                    aller_9100.mem == 1 && aller_9100.port_vu == 9100, retour_9100.mem == 1 && retour_9100.port_vu == 9100,
                    aller_9103.mem == 1 && aller_9103.port_vu == 9103, retour_9103.mem == 1 && retour_9103.port_vu == 9103);
        avec_ok = ports;
        StopEnds(&ends);
    }
    if (relay_alive) {
        TerminateProcess(relay.hProcess, 1);
        WaitForSingleObject(relay.hProcess, 2000);
        CloseHandle(relay.hThread);
        CloseHandle(relay.hProcess);
    }

    const bool ini_ok = IniStillOff("D:\\Jeux\\Tom Clancys Splinter Cell Chaos Theory\\System\\scct-coop-net.ini");
    const bool ok = sans_failed && avec_ok && ini_ok;
    std::printf("resultat=%s\n", ok ? "ok" : "echec");
    std::fflush(stdout);
    return ok ? 0 : 1;
}
