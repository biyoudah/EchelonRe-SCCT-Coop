// scct-lan-relay
// Port de controle : REGISTER, HEARTBEAT, ATTACH. Rien n'y est recopie comme payload.
// Port de donnees (controle + 1) : toute longueur, y compris 10 et 12, est recopiee telle quelle.
// Ne fabrique aucune reponse de jeu.

#include "relay_proto.h"

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {

using scct::relay::kRefuseFull;
using scct::relay::kRefuseNoPeer;
using scct::relay::kRefuseRate;
using scct::relay::kRefuseUnknown;
using scct::relay::kRefuseVersion;

constexpr int kMaxPeers = 4;
constexpr int kPeersPerPort = 2;
constexpr int kMaxRates = 32;
constexpr int kControlCapPerSec = 60;

struct Peer {
    bool used;
    uint32_t id;
    uint16_t game_port;
    uint32_t control_ip;
    uint16_t control_port;
    bool data_known;
    uint32_t data_ip;
    uint16_t data_port;
    uint64_t last_ms;
};

struct Rate {
    bool used;
    uint32_t ip_be;
    uint64_t window_ms;
    int payloads;
    int controls;
};

struct Tomb {
    bool used;
    uint32_t data_ip;
    uint16_t data_port;
    sockaddr_in control;
};

SOCKET g_control = INVALID_SOCKET;
SOCKET g_data = INVALID_SOCKET;
Peer g_peers[kMaxPeers]{};
Rate g_rates[kMaxRates]{};
Tomb g_tombs[kMaxPeers]{};
uint32_t g_next_id = 1;
uint64_t g_expire_ms = 5000;
int g_rate = 300;

uint64_t NowMs() {
    return GetTickCount64();
}

void IpText(uint32_t ip_be, char* out, size_t out_len) {
    const unsigned char* ip = reinterpret_cast<const unsigned char*>(&ip_be);
    std::snprintf(out, out_len, "%u.%u.%u.%u", ip[0], ip[1], ip[2], ip[3]);
}

sockaddr_in ControlDest(const Peer& peer);

void SendOn(SOCKET sock, const sockaddr_in& dest, const uint8_t* data, int len) {
    sendto(sock, reinterpret_cast<const char*>(data), len, 0, reinterpret_cast<const sockaddr*>(&dest),
           sizeof(dest));
}

void Refuse(const sockaddr_in& dest, uint16_t reason) {
    uint8_t packet[16];
    const int n = scct::relay::BuildRefused(packet, reason);
    SendOn(g_control, dest, packet, n);
    char ip[32];
    IpText(dest.sin_addr.s_addr, ip, sizeof(ip));
    std::printf("refus ip=%s port=%u raison=%u\n", ip, ntohs(dest.sin_port), reason);
    std::fflush(stdout);
}

Rate* Bucket(uint32_t ip_be, uint64_t now) {
    Rate* free_slot = nullptr;
    for (Rate& rate : g_rates) {
        if (rate.used && rate.ip_be == ip_be) {
            if (now - rate.window_ms >= 1000) {
                rate.window_ms = now;
                rate.payloads = 0;
                rate.controls = 0;
            }
            return &rate;
        }
        if (!rate.used && free_slot == nullptr) {
            free_slot = &rate;
        }
    }
    if (free_slot == nullptr) {
        free_slot = &g_rates[0];
    }
    *free_slot = {};
    free_slot->used = true;
    free_slot->ip_be = ip_be;
    free_slot->window_ms = now;
    return free_slot;
}

void Reap(uint64_t now) {
    for (Peer& peer : g_peers) {
        if (!peer.used || now - peer.last_ms <= g_expire_ms) {
            continue;
        }
        char ip[32];
        const uint32_t shown = peer.data_known ? peer.data_ip : peer.control_ip;
        const uint16_t shown_port = peer.data_known ? peer.data_port : peer.control_port;
        IpText(shown, ip, sizeof(ip));
        std::printf("expire ip=%s port=%u canal=%u\n", ip, ntohs(shown_port), peer.game_port);
        std::fflush(stdout);
        if (peer.data_known) {
            for (Tomb& tomb : g_tombs) {
                if (!tomb.used || (tomb.data_ip == peer.data_ip && tomb.data_port == peer.data_port)) {
                    tomb.used = true;
                    tomb.data_ip = peer.data_ip;
                    tomb.data_port = peer.data_port;
                    tomb.control = ControlDest(peer);
                    break;
                }
            }
        }
        peer = {};
    }
}

int CountPort(uint16_t game_port) {
    int count = 0;
    for (const Peer& peer : g_peers) {
        if (peer.used && peer.game_port == game_port) {
            ++count;
        }
    }
    return count;
}

Peer* FindControl(uint32_t ip_be, uint16_t port_be) {
    for (Peer& peer : g_peers) {
        if (peer.used && peer.control_ip == ip_be && peer.control_port == port_be) {
            return &peer;
        }
    }
    return nullptr;
}

Peer* FindData(uint32_t ip_be, uint16_t port_be) {
    for (Peer& peer : g_peers) {
        if (peer.used && peer.data_known && peer.data_ip == ip_be && peer.data_port == port_be) {
            return &peer;
        }
    }
    return nullptr;
}

Peer* FindId(uint32_t id) {
    for (Peer& peer : g_peers) {
        if (peer.used && peer.id == id) {
            return &peer;
        }
    }
    return nullptr;
}

Peer* Other(const Peer& self) {
    for (Peer& peer : g_peers) {
        if (!peer.used || &peer == &self || peer.game_port != self.game_port) {
            continue;
        }
        return &peer;
    }
    return nullptr;
}

sockaddr_in ControlDest(const Peer& peer) {
    sockaddr_in dest{};
    dest.sin_family = AF_INET;
    dest.sin_addr.s_addr = peer.control_ip;
    dest.sin_port = peer.control_port;
    return dest;
}

void FillOk(const Peer& self) {
    const Peer* other = Other(self);
    uint8_t ip[4]{};
    uint16_t port = 0;
    const bool has = other != nullptr && other->data_known;
    if (has) {
        std::memcpy(ip, &other->data_ip, 4);
        port = ntohs(other->data_port);
    }
    uint8_t packet[32];
    const int n = scct::relay::BuildRegisterOk(packet, self.id, has, ip, port);
    const sockaddr_in dest = ControlDest(self);
    SendOn(g_control, dest, packet, n);
}

void NotifyOther(const Peer& self) {
    if (!self.data_known) {
        return;
    }
    const Peer* other = Other(self);
    if (other == nullptr) {
        return;
    }
    uint8_t ip[4];
    std::memcpy(ip, &self.data_ip, 4);
    uint8_t packet[32];
    const int n = scct::relay::BuildAddress(packet, ip, ntohs(self.data_port), self.game_port);
    const sockaddr_in dest = ControlDest(*other);
    SendOn(g_control, dest, packet, n);
}

void HandleRegister(const sockaddr_in& src, uint16_t game_port, uint64_t now) {
    if (!scct::relay::IsGamePort(game_port)) {
        Refuse(src, kRefuseUnknown);
        return;
    }
    Peer* self = FindControl(src.sin_addr.s_addr, src.sin_port);
    if (self != nullptr) {
        if (self->game_port != game_port) {
            Refuse(src, kRefuseUnknown);
            return;
        }
        self->last_ms = now;
        FillOk(*self);
        return;
    }
    if (CountPort(game_port) >= kPeersPerPort) {
        Refuse(src, kRefuseFull);
        return;
    }
    Peer* slot = nullptr;
    for (Peer& peer : g_peers) {
        if (!peer.used) {
            slot = &peer;
            break;
        }
    }
    if (slot == nullptr) {
        Refuse(src, kRefuseFull);
        return;
    }
    *slot = {};
    slot->used = true;
    slot->id = g_next_id++;
    slot->game_port = game_port;
    slot->control_ip = src.sin_addr.s_addr;
    slot->control_port = src.sin_port;
    slot->last_ms = now;
    FillOk(*slot);
    char ip[32];
    IpText(slot->control_ip, ip, sizeof(ip));
    std::printf("pair id=%u ip=%s port=%u canal=%u\n", slot->id, ip, ntohs(slot->control_port), slot->game_port);
    std::fflush(stdout);
}

void HandleHeartbeat(const sockaddr_in& src, uint64_t now) {
    Peer* self = FindControl(src.sin_addr.s_addr, src.sin_port);
    if (self == nullptr) {
        Refuse(src, kRefuseUnknown);
        return;
    }
    self->last_ms = now;
    FillOk(*self);
}

void HandleAttach(const sockaddr_in& src, uint32_t peer_id, uint64_t now) {
    (void)now;
    Peer* self = FindId(peer_id);
    if (self == nullptr || self->control_ip != src.sin_addr.s_addr) {
        return;
    }
    self->data_known = true;
    self->data_ip = src.sin_addr.s_addr;
    self->data_port = src.sin_port;
    FillOk(*self);
    NotifyOther(*self);
    char ip[32];
    IpText(self->data_ip, ip, sizeof(ip));
    std::printf("donnees id=%u ip=%s port=%u canal=%u\n", self->id, ip, ntohs(self->data_port), self->game_port);
    std::fflush(stdout);
}

void OnControl(const sockaddr_in& src, uint8_t* buf, int len) {
    const uint64_t now = NowMs();
    Reap(now);
    if (len <= 0 || len > 65507) {
        return;
    }
    Rate* rate = Bucket(src.sin_addr.s_addr, now);
    if (rate->controls >= kControlCapPerSec) {
        Refuse(src, kRefuseRate);
        return;
    }
    ++rate->controls;
    uint16_t game_port = 0;
    if (scct::relay::ParseRegister(buf, len, &game_port)) {
        HandleRegister(src, game_port, now);
        return;
    }
    if (scct::relay::ParseHeartbeat(buf, len)) {
        HandleHeartbeat(src, now);
        return;
    }
    uint32_t peer_id = 0;
    if (scct::relay::ParseAttach(buf, len, &peer_id)) {
        HandleAttach(src, peer_id, now);
        return;
    }
    uint16_t version = 0;
    uint16_t type = 0;
    if (scct::relay::MagicVersion(buf, len, &version, &type) && version != scct::relay::kVersion) {
        Refuse(src, kRefuseVersion);
        return;
    }
    Refuse(src, kRefuseUnknown);
}

void OnData(const sockaddr_in& src, const uint8_t* buf, int len) {
    const uint64_t now = NowMs();
    Peer* self = FindData(src.sin_addr.s_addr, src.sin_port);
    if (self != nullptr && now - self->last_ms > g_expire_ms) {
        const sockaddr_in dest = ControlDest(*self);
        *self = {};
        Refuse(dest, kRefuseUnknown);
        return;
    }
    Reap(now);
    self = FindData(src.sin_addr.s_addr, src.sin_port);
    if (self == nullptr || len <= 0 || len > 65507) {
        for (Tomb& tomb : g_tombs) {
            if (tomb.used && tomb.data_ip == src.sin_addr.s_addr && tomb.data_port == src.sin_port) {
                const sockaddr_in dest = tomb.control;
                tomb.used = false;
                Refuse(dest, kRefuseUnknown);
                break;
            }
        }
        return;
    }
    Rate* rate = Bucket(src.sin_addr.s_addr, now);
    if (rate->payloads >= g_rate) {
        Refuse(ControlDest(*self), kRefuseRate);
        return;
    }
    ++rate->payloads;
    Peer* other = Other(*self);
    if (other == nullptr || !other->data_known) {
        Refuse(ControlDest(*self), kRefuseNoPeer);
        return;
    }
    sockaddr_in dest{};
    dest.sin_family = AF_INET;
    dest.sin_addr.s_addr = other->data_ip;
    dest.sin_port = other->data_port;
    SendOn(g_data, dest, buf, len);
}

bool ParseListen(const char* text, sockaddr_in* out) {
    const char* colon = std::strrchr(text, ':');
    if (colon == nullptr || colon == text || colon[1] == '\0') {
        return false;
    }
    char host[256];
    const size_t host_len = static_cast<size_t>(colon - text);
    if (host_len == 0 || host_len >= sizeof(host)) {
        return false;
    }
    std::memcpy(host, text, host_len);
    host[host_len] = '\0';
    char* end = nullptr;
    const long port = std::strtol(colon + 1, &end, 10);
    if (end == colon + 1 || *end != '\0' || port < 1 || port > 65534) {
        return false;
    }
    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    hints.ai_protocol = IPPROTO_UDP;
    addrinfo* info = nullptr;
    if (getaddrinfo(host, nullptr, &hints, &info) != 0 || info == nullptr) {
        return false;
    }
    *out = {};
    out->sin_family = AF_INET;
    out->sin_addr = reinterpret_cast<sockaddr_in*>(info->ai_addr)->sin_addr;
    out->sin_port = htons(static_cast<unsigned short>(port));
    freeaddrinfo(info);
    return true;
}

bool BindUdp(SOCKET* out, const sockaddr_in& addr) {
    const SOCKET sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock == INVALID_SOCKET) {
        return false;
    }
    BOOL reuse = TRUE;
    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&reuse), sizeof(reuse));
    if (bind(sock, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) != 0) {
        closesocket(sock);
        return false;
    }
    *out = sock;
    return true;
}

void Usage() {
    std::fprintf(stderr,
                 "usage: scct-lan-relay --listen hote:port [--expire secondes] [--rate paquets_par_seconde_par_ip]\n");
}

}  // namespace

int main(int argc, char** argv) {
    const char* listen = "0.0.0.0:27100";
    int expire_s = 5;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--listen") == 0 && i + 1 < argc) {
            listen = argv[++i];
        } else if (std::strcmp(argv[i], "--expire") == 0 && i + 1 < argc) {
            expire_s = std::atoi(argv[++i]);
        } else if (std::strcmp(argv[i], "--rate") == 0 && i + 1 < argc) {
            g_rate = std::atoi(argv[++i]);
        } else {
            Usage();
            return 2;
        }
    }
    if (expire_s < 1 || expire_s > 3600 || g_rate < 1 || g_rate > 10000) {
        Usage();
        return 2;
    }
    g_expire_ms = static_cast<uint64_t>(expire_s) * 1000ull;

    WSADATA wsa{};
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        std::fprintf(stderr, "WSAStartup a echoue\n");
        return 1;
    }
    sockaddr_in addr{};
    if (!ParseListen(listen, &addr)) {
        std::fprintf(stderr, "adresse d'ecoute illisible: %s\n", listen);
        WSACleanup();
        return 2;
    }
    sockaddr_in data_addr = addr;
    data_addr.sin_port = htons(static_cast<unsigned short>(ntohs(addr.sin_port) + 1));
    if (!BindUdp(&g_control, addr) || !BindUdp(&g_data, data_addr)) {
        std::fprintf(stderr, "bind a echoue (%d)\n", WSAGetLastError());
        if (g_control != INVALID_SOCKET) {
            closesocket(g_control);
        }
        if (g_data != INVALID_SOCKET) {
            closesocket(g_data);
        }
        WSACleanup();
        return 1;
    }
    const unsigned char* ip = reinterpret_cast<const unsigned char*>(&addr.sin_addr);
    std::printf("ecoute=%u.%u.%u.%u:%u donnees=%u version=%u expire_s=%d debit_par_ip=%d\n", ip[0], ip[1], ip[2], ip[3],
                ntohs(addr.sin_port), ntohs(data_addr.sin_port), scct::relay::kVersion, expire_s, g_rate);
    std::fflush(stdout);

    uint8_t buf[65535];
    for (;;) {
        fd_set read_set;
        FD_ZERO(&read_set);
        FD_SET(g_control, &read_set);
        FD_SET(g_data, &read_set);
        timeval wait{};
        wait.tv_sec = 0;
        wait.tv_usec = 200000;
        const int ready = select(0, &read_set, nullptr, nullptr, &wait);
        Reap(NowMs());
        if (ready <= 0) {
            continue;
        }
        if (FD_ISSET(g_control, &read_set)) {
            sockaddr_in src{};
            int src_len = sizeof(src);
            const int got = recvfrom(g_control, reinterpret_cast<char*>(buf), sizeof(buf), 0,
                                     reinterpret_cast<sockaddr*>(&src), &src_len);
            if (got >= 0 && src.sin_family == AF_INET) {
                OnControl(src, buf, got);
            }
        }
        if (FD_ISSET(g_data, &read_set)) {
            sockaddr_in src{};
            int src_len = sizeof(src);
            const int got = recvfrom(g_data, reinterpret_cast<char*>(buf), sizeof(buf), 0,
                                     reinterpret_cast<sockaddr*>(&src), &src_len);
            if (got >= 0 && src.sin_family == AF_INET) {
                OnData(src, buf, got);
            }
        }
    }
}
