// Copie les payloads UDP 9100 et 9103 vers le relais, octets inchanges.
// Le socket du jeu n'est ni relie ni rebind. La livraison locale est un second datagramme.

#include "transport.h"

#include "relay_proto.h"

#include <ws2tcpip.h>

#include <cstdio>
#include <cstring>

#ifndef SIO_UDP_CONNRESET
#define SIO_UDP_CONNRESET _WSAIOW(IOC_VENDOR, 12)
#endif

namespace {

using scct::relay::kMaxPayload;
using scct::relay::kPortDiscovery;
using scct::relay::kPortSession;

struct Outbound {
    unsigned short port;
    int len;
    uint8_t data[kMaxPayload];
};

constexpr int kOutMax = 128;

struct Held {
    int len;
    uint8_t data[kMaxPayload];
};

struct Claim {
    bool used;
    uint32_t id;
    sockaddr_in peer;
};

constexpr int kHoldMax = 8;
constexpr int kClaimMax = 32;
constexpr int kPendingMax = 32;

struct Channel {
    SOCKET sock;
    SOCKET data;
    unsigned short game_port;
    bool registered;
    uint32_t self_id;
    bool peer_known;
    uint32_t peer_ip_be;
    unsigned short peer_port_host;
    uint32_t expected_peer_ip_be;
    int held;
    Held queue[kHoldMax];
    int claim_count;
    Claim claims[kClaimMax];
};

struct Bound {
    bool used;
    unsigned short port;
    uint32_t ip_be;
};

struct Pending {
    bool used;
    SOCKET sock;
    sockaddr* from;
    int* fromlen;
    LPWSAOVERLAPPED overlapped;
    TransportCompleteFn user;
};

TransportSendFn g_send = nullptr;
TransportRecvFn g_recv = nullptr;
TransportLogFn g_log = nullptr;
CRITICAL_SECTION g_lock{};
bool g_lock_ready = false;
volatile LONG g_active = 0;
volatile LONG g_stop = 0;
thread_local int t_sending = 0;
SOCKET g_injector = INVALID_SOCKET;
sockaddr_in g_injector_addr{};
sockaddr_in g_relay{};
sockaddr_in g_relay_data{};
Channel g_channel[2]{};
Bound g_bound[2]{};
Pending g_pending[kPendingMax]{};
uint32_t g_claim_seq = 1;
uint32_t g_local_ip_be = 0;
Outbound g_out[kOutMax]{};
int g_out_head = 0;
int g_out_tail = 0;
int g_out_count = 0;
WSAEVENT g_sock_events[4] = {
    WSA_INVALID_EVENT,
    WSA_INVALID_EVENT,
    WSA_INVALID_EVENT,
    WSA_INVALID_EVENT,
};
HANDLE g_wake_event = nullptr;
HANDLE g_thread = nullptr;
LONG g_once_big = 0;
LONG g_once_send = 0;
LONG g_once_inject = 0;
LONG g_once_refuse[8]{};

int IndexFor(unsigned short port) {
    if (port == kPortDiscovery) {
        return 0;
    }
    if (port == kPortSession) {
        return 1;
    }
    return -1;
}

void LogNote(const char* note) {
    if (g_log == nullptr || note == nullptr) {
        return;
    }
    char line[600];
    std::snprintf(line, sizeof(line),
                  "heure=0 pid=%lu dir=transport local=- pair=- len=0 hex= seq=0 tronque=0 note=%s",
                  GetCurrentProcessId(), note);
    g_log(line);
}

void LogOnce(LONG* flag, const char* note) {
    if (InterlockedCompareExchange(flag, 1, 0) != 0) {
        return;
    }
    LogNote(note);
}

void SilenceUdpReset(SOCKET sock) {
    DWORD bytes = 0;
    BOOL off = FALSE;
    WSAIoctl(sock, SIO_UDP_CONNRESET, &off, sizeof(off), nullptr, 0, &bytes, nullptr, nullptr);
}

int RawSend(SOCKET sock, const void* data, int len, const sockaddr_in& dest) {
    if (g_send == nullptr || sock == INVALID_SOCKET || data == nullptr || len <= 0) {
        return -1;
    }
    ++t_sending;
    const int sent = g_send(sock, static_cast<const char*>(data), len, 0, reinterpret_cast<const sockaddr*>(&dest),
                            sizeof(dest));
    --t_sending;
    return sent;
}

unsigned short SocketPort(SOCKET sock, uint32_t* ip_be) {
    sockaddr_in sa{};
    int len = sizeof(sa);
    if (getsockname(sock, reinterpret_cast<sockaddr*>(&sa), &len) != 0 || sa.sin_family != AF_INET) {
        return 0;
    }
    if (ip_be != nullptr) {
        *ip_be = sa.sin_addr.s_addr;
    }
    return ntohs(sa.sin_port);
}

void RememberBound(unsigned short port, uint32_t ip_be) {
    const int index = IndexFor(port);
    if (index < 0) {
        return;
    }
    g_bound[index].used = true;
    g_bound[index].port = port;
    g_bound[index].ip_be = ip_be;
}

uint32_t BoundIp(unsigned short port) {
    const int index = IndexFor(port);
    if (index < 0 || !g_bound[index].used || g_bound[index].ip_be == 0 ||
        g_bound[index].ip_be == htonl(INADDR_ANY)) {
        return g_local_ip_be != 0 ? g_local_ip_be : htonl(INADDR_LOOPBACK);
    }
    return g_bound[index].ip_be;
}

bool PushClaim(Channel& channel, uint32_t id, const sockaddr_in& peer) {
    if (channel.claim_count >= kClaimMax) {
        return false;
    }
    for (Claim& claim : channel.claims) {
        if (!claim.used) {
            claim.used = true;
            claim.id = id;
            claim.peer = peer;
            ++channel.claim_count;
            return true;
        }
    }
    return false;
}

bool TakeClaim(Channel& channel, sockaddr_in* peer) {
    for (Claim& claim : channel.claims) {
        if (!claim.used) {
            continue;
        }
        claim.used = false;
        --channel.claim_count;
        *peer = claim.peer;
        return true;
    }
    return false;
}

void CancelClaim(Channel& channel, uint32_t id) {
    for (Claim& claim : channel.claims) {
        if (claim.used && claim.id == id) {
            claim.used = false;
            --channel.claim_count;
            return;
        }
    }
}

bool Inject(unsigned short game_port, const uint8_t* data, int len, const sockaddr_in& peer) {
    if (kTransportCut == 1) {
        return false;
    }
    if (g_injector == INVALID_SOCKET || data == nullptr || len <= 0 || peer.sin_addr.s_addr == 0 || peer.sin_port == 0) {
        return false;
    }
    const int index = IndexFor(game_port);
    if (index < 0) {
        return false;
    }
    const uint32_t id = ++g_claim_seq;
    EnterCriticalSection(&g_lock);
    const bool queued = PushClaim(g_channel[index], id, peer);
    const uint32_t dest_ip = BoundIp(game_port);
    LeaveCriticalSection(&g_lock);
    if (!queued) {
        LogOnce(&g_once_inject, "file de livraison pleine, datagramme non injecte");
        return false;
    }
    sockaddr_in dest{};
    dest.sin_family = AF_INET;
    dest.sin_port = htons(game_port);
    dest.sin_addr.s_addr = dest_ip;
    const int sent = RawSend(g_injector, data, len, dest);
    if (sent != len) {
        const int err = WSAGetLastError();
        EnterCriticalSection(&g_lock);
        CancelClaim(g_channel[index], id);
        LeaveCriticalSection(&g_lock);
        char note[96];
        std::snprintf(note, sizeof(note), "injection locale non partie wsa=%d", err);
        LogOnce(&g_once_inject, note);
        return false;
    }
    return true;
}

sockaddr_in PeerAddress(const Channel& channel) {
    sockaddr_in peer{};
    peer.sin_family = AF_INET;
    peer.sin_addr.s_addr = channel.peer_ip_be;
    // Le jeu repond en unicast vers ce sockaddr. Le port est celui du canal.
    // Le port ephemere du socket de donnees reste interne au relais.
    peer.sin_port = htons(channel.game_port);
    return peer;
}

void HoldOrInject(Channel& channel, const uint8_t* data, int len) {
    EnterCriticalSection(&g_lock);
    const bool known = channel.peer_known && channel.peer_port_host != 0;
    const sockaddr_in peer = PeerAddress(channel);
    if (!known) {
        if (channel.held < kHoldMax && len <= kMaxPayload) {
            Held& slot = channel.queue[channel.held];
            slot.len = len;
            std::memcpy(slot.data, data, static_cast<size_t>(len));
            ++channel.held;
        }
        LeaveCriticalSection(&g_lock);
        return;
    }
    LeaveCriticalSection(&g_lock);
    Inject(channel.game_port, data, len, peer);
}

void FlushHeld(Channel& channel) {
    Held local[kHoldMax];
    int count = 0;
    sockaddr_in peer{};
    EnterCriticalSection(&g_lock);
    count = channel.held;
    if (count > kHoldMax) {
        count = kHoldMax;
    }
    for (int i = 0; i < count; ++i) {
        local[i] = channel.queue[i];
    }
    channel.held = 0;
    peer = PeerAddress(channel);
    const unsigned short port = channel.game_port;
    LeaveCriticalSection(&g_lock);
    for (int i = 0; i < count; ++i) {
        Inject(port, local[i].data, local[i].len, peer);
    }
}

void RememberPeer(Channel& channel, const uint8_t ip[4], unsigned short port_host) {
    uint32_t ip_be = 0;
    std::memcpy(&ip_be, ip, 4);
    if (ip_be == 0) {
        return;
    }
    bool changed = false;
    EnterCriticalSection(&g_lock);
    changed = !channel.peer_known || channel.peer_ip_be != ip_be || channel.peer_port_host != port_host;
    channel.peer_known = true;
    channel.peer_ip_be = ip_be;
    channel.peer_port_host = port_host;
    LeaveCriticalSection(&g_lock);
    if (changed) {
        char note[160];
        std::snprintf(note, sizeof(note), "pair observe %u.%u.%u.%u:%u canal=%u", ip[0], ip[1], ip[2], ip[3], port_host,
                      channel.game_port);
        LogNote(note);
    }
    FlushHeld(channel);
}

void SendControl(Channel& channel, const uint8_t* data, int len) {
    RawSend(channel.sock, data, len, g_relay);
}

void OnChannelPacket(Channel& channel, const uint8_t* data, int len) {
    uint32_t peer_id = 0;
    uint8_t has_other = 0;
    uint8_t ip[4]{};
    uint16_t port_host = 0;
    if (scct::relay::ParseRegisterOk(data, len, &peer_id, &has_other, ip, &port_host)) {
        channel.registered = true;
        channel.self_id = peer_id;
        if (has_other == 1 && port_host != 0) {
            RememberPeer(channel, ip, port_host);
        }
        return;
    }
    uint16_t game_port = 0;
    if (scct::relay::ParseAddress(data, len, ip, &port_host, &game_port)) {
        if (game_port == channel.game_port) {
            RememberPeer(channel, ip, port_host);
        }
        return;
    }
    uint16_t reason = 0;
    if (scct::relay::ParseRefused(data, len, &reason)) {
        char note[80];
        std::snprintf(note, sizeof(note), "relais a refuse raison=%u canal=%u", reason, channel.game_port);
        const int slot = reason < 8 ? reason : 0;
        LogOnce(&g_once_refuse[slot], note);
        if (reason == scct::relay::kRefuseUnknown) {
            channel.registered = false;
            channel.self_id = 0;
        }
        return;
    }
}

void OnDataPacket(Channel& channel, const uint8_t* data, int len) {
    if (len <= 0 || len > kMaxPayload) {
        return;
    }
    HoldOrInject(channel, data, len);
}

void DrainOutbound() {
    for (;;) {
        unsigned short port = 0;
        int len = 0;
        uint8_t local[kMaxPayload];
        bool have = false;
        EnterCriticalSection(&g_lock);
        if (g_out_count > 0) {
            const Outbound& slot = g_out[g_out_head];
            port = slot.port;
            len = slot.len;
            std::memcpy(local, slot.data, static_cast<size_t>(len));
            g_out_head = (g_out_head + 1) % kOutMax;
            --g_out_count;
            have = true;
        }
        LeaveCriticalSection(&g_lock);
        if (!have) {
            return;
        }
        TransportCopyPayload(port, local, len);
    }
}

DWORD WINAPI ThreadProc(LPVOID) {
    uint64_t last_beat = 0;
    uint8_t buf[65535];
    HANDLE wait_handles[5] = {
        g_sock_events[0],
        g_sock_events[1],
        g_sock_events[2],
        g_sock_events[3],
        g_wake_event,
    };
    SOCKET sock_map[4] = {
        g_channel[0].sock,
        g_channel[0].data,
        g_channel[1].sock,
        g_channel[1].data,
    };
    while (InterlockedCompareExchange(&g_stop, 0, 0) == 0) {
        DrainOutbound();
        const uint64_t now = GetTickCount64();
        if (now - last_beat >= 500) {
            for (Channel& channel : g_channel) {
                uint8_t packet[16];
                if (!channel.registered || channel.self_id == 0) {
                    const int n = scct::relay::BuildRegister(packet, channel.game_port);
                    SendControl(channel, packet, n);
                } else {
                    const int n = scct::relay::BuildHeartbeat(packet);
                    SendControl(channel, packet, n);
                    const int attached = scct::relay::BuildAttach(packet, channel.self_id);
                    RawSend(channel.data, packet, attached, g_relay);
                }
            }
            last_beat = now;
        }
        DWORD wait_ms = 100;
        const uint64_t since_beat = GetTickCount64() - last_beat;
        if (since_beat < 500) {
            const uint64_t rem = 500 - since_beat;
            if (rem < wait_ms) {
                wait_ms = static_cast<DWORD>(rem);
            }
        } else {
            wait_ms = 0;
        }
        const DWORD res = WSAWaitForMultipleEvents(5, wait_handles, FALSE, wait_ms, FALSE);
        if (InterlockedCompareExchange(&g_stop, 0, 0) != 0) {
            break;
        }
        DrainOutbound();
        if (res == WSA_WAIT_FAILED) {
            Sleep(10);
            continue;
        }
        for (int i = 0; i < 4; ++i) {
            if (sock_map[i] == INVALID_SOCKET || g_sock_events[i] == WSA_INVALID_EVENT) {
                continue;
            }
            WSANETWORKEVENTS net_ev{};
            if (WSAEnumNetworkEvents(sock_map[i], g_sock_events[i], &net_ev) != 0) {
                continue;
            }
            if ((net_ev.lNetworkEvents & FD_READ) == 0 && res != WSA_WAIT_EVENT_0 + static_cast<DWORD>(i)) {
                continue;
            }
            Channel& ch = g_channel[i / 2];
            const bool is_ctrl = (i % 2) == 0;
            for (;;) {
                sockaddr_in src{};
                int src_len = sizeof(src);
                const int got = g_recv(sock_map[i], reinterpret_cast<char*>(buf), sizeof(buf), 0,
                                       reinterpret_cast<sockaddr*>(&src), &src_len);
                if (got <= 0) {
                    break;
                }
                if (is_ctrl) {
                    OnChannelPacket(ch, buf, got);
                } else {
                    OnDataPacket(ch, buf, got);
                }
            }
        }
    }
    return 0;
}

void CloseTransportSockets() {
    for (int i = 0; i < 4; ++i) {
        if (g_sock_events[i] != WSA_INVALID_EVENT) {
            WSACloseEvent(g_sock_events[i]);
            g_sock_events[i] = WSA_INVALID_EVENT;
        }
    }
    if (g_wake_event != nullptr) {
        CloseHandle(g_wake_event);
        g_wake_event = nullptr;
    }
    for (Channel& channel : g_channel) {
        if (channel.sock != INVALID_SOCKET) {
            closesocket(channel.sock);
            channel.sock = INVALID_SOCKET;
        }
        if (channel.data != INVALID_SOCKET) {
            closesocket(channel.data);
            channel.data = INVALID_SOCKET;
        }
    }
    if (g_injector != INVALID_SOCKET) {
        closesocket(g_injector);
        g_injector = INVALID_SOCKET;
    }
}

SOCKET OpenUdp() {
    const SOCKET sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock == INVALID_SOCKET) {
        return INVALID_SOCKET;
    }
    SilenceUdpReset(sock);
    return sock;
}

Pending* FindPending(LPWSAOVERLAPPED overlapped) {
    if (overlapped == nullptr) {
        return nullptr;
    }
    for (Pending& slot : g_pending) {
        if (slot.used && slot.overlapped == overlapped) {
            return &slot;
        }
    }
    return nullptr;
}

void CALLBACK TransportApc(DWORD error, DWORD transferred, LPWSAOVERLAPPED overlapped, DWORD flags) {
    SOCKET sock = INVALID_SOCKET;
    sockaddr* from = nullptr;
    int* fromlen = nullptr;
    TransportCompleteFn user = nullptr;
    EnterCriticalSection(&g_lock);
    Pending* slot = FindPending(overlapped);
    if (slot != nullptr) {
        sock = slot->sock;
        from = slot->from;
        fromlen = slot->fromlen;
        user = slot->user;
        slot->used = false;
    }
    LeaveCriticalSection(&g_lock);
    if (error == 0 && transferred > 0 && from != nullptr && fromlen != nullptr) {
        TransportRewriteFrom(sock, from, fromlen);
    }
    if (user != nullptr) {
        user(error, transferred, overlapped, flags);
    }
}

int CopyKnownPeer(unsigned short game_port, sockaddr* out, int* out_len) {
    if (!TransportActive() || out == nullptr || out_len == nullptr ||
        *out_len < static_cast<int>(sizeof(sockaddr_in)) || !g_lock_ready) {
        return 0;
    }
    const int index = IndexFor(game_port);
    if (index < 0) {
        return 0;
    }
    sockaddr_in peer{};
    bool known = false;
    EnterCriticalSection(&g_lock);
    if (g_channel[index].peer_known && g_channel[index].peer_port_host != 0) {
        peer = PeerAddress(g_channel[index]);
        known = peer.sin_addr.s_addr != 0 && peer.sin_port != 0;
    }
    LeaveCriticalSection(&g_lock);
    if (!known) {
        return 0;
    }
    std::memcpy(out, &peer, sizeof(peer));
    *out_len = static_cast<int>(sizeof(peer));
    return 1;
}

void StopHeartbeats() {
    InterlockedExchange(&g_stop, 1);
    if (g_wake_event != nullptr) {
        SetEvent(g_wake_event);
    }
    if (g_thread != nullptr) {
        WaitForSingleObject(g_thread, 3000);
        CloseHandle(g_thread);
        g_thread = nullptr;
    }
}

}  // namespace

void TransportStop() {
    StopHeartbeats();
}

void TransportShutdown() {
    StopHeartbeats();
    CloseTransportSockets();
    InterlockedExchange(&g_active, 0);
}

bool TransportActive() {
    return InterlockedCompareExchange(&g_active, 0, 0) == 1;
}

bool TransportSending() {
    return t_sending > 0;
}

bool TransportStart(const char* host, unsigned short port, TransportSendFn send_fn, TransportRecvFn recv_fn,
                    TransportLogFn log_fn, uint32_t local_ip_be) {
    if (TransportActive()) {
        return true;
    }
    g_send = send_fn;
    g_recv = recv_fn;
    g_log = log_fn;
    g_local_ip_be = local_ip_be;
    if (g_send == nullptr || g_recv == nullptr || host == nullptr || host[0] == '\0' || port == 0 || port == 65535) {
        LogNote("transport demarrage refuse, relais ou envoi brut absent");
        return false;
    }
    WSADATA wsa{};
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        LogNote("WSAStartup du transport a echoue");
        return false;
    }
    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    addrinfo* info = nullptr;
    if (getaddrinfo(host, nullptr, &hints, &info) != 0 || info == nullptr) {
        LogNote("adresse relay illisible, transport coupe");
        return false;
    }
    g_relay = {};
    g_relay.sin_family = AF_INET;
    g_relay.sin_addr = reinterpret_cast<sockaddr_in*>(info->ai_addr)->sin_addr;
    g_relay.sin_port = htons(port);
    g_relay_data = g_relay;
    g_relay_data.sin_port = htons(static_cast<unsigned short>(port + 1));
    freeaddrinfo(info);

    if (!g_lock_ready) {
        InitializeCriticalSection(&g_lock);
        g_lock_ready = true;
    }
    g_out_head = 0;
    g_out_tail = 0;
    g_out_count = 0;
    g_channel[0] = {};
    g_channel[1] = {};
    g_channel[0].sock = INVALID_SOCKET;
    g_channel[0].data = INVALID_SOCKET;
    g_channel[1].sock = INVALID_SOCKET;
    g_channel[1].data = INVALID_SOCKET;
    g_channel[0].game_port = kPortDiscovery;
    g_channel[1].game_port = kPortSession;
    g_channel[0].sock = OpenUdp();
    g_channel[0].data = OpenUdp();
    g_channel[1].sock = OpenUdp();
    g_channel[1].data = OpenUdp();
    g_injector = OpenUdp();
    if (g_channel[0].sock == INVALID_SOCKET || g_channel[0].data == INVALID_SOCKET ||
        g_channel[1].sock == INVALID_SOCKET || g_channel[1].data == INVALID_SOCKET || g_injector == INVALID_SOCKET) {
        CloseTransportSockets();
        LogNote("sockets du transport indisponibles");
        return false;
    }
    sockaddr_in any{};
    any.sin_family = AF_INET;
    any.sin_addr.s_addr = (local_ip_be != 0) ? local_ip_be : htonl(INADDR_ANY);
    if (bind(g_channel[0].sock, reinterpret_cast<sockaddr*>(&any), sizeof(any)) != 0 ||
        bind(g_channel[0].data, reinterpret_cast<sockaddr*>(&any), sizeof(any)) != 0 ||
        bind(g_channel[1].sock, reinterpret_cast<sockaddr*>(&any), sizeof(any)) != 0 ||
        bind(g_channel[1].data, reinterpret_cast<sockaddr*>(&any), sizeof(any)) != 0) {
        CloseTransportSockets();
        LogNote("bind du transport a echoue");
        return false;
    }
    sockaddr_in inj_bind{};
    inj_bind.sin_family = AF_INET;
    inj_bind.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(g_injector, reinterpret_cast<sockaddr*>(&inj_bind), sizeof(inj_bind)) != 0) {
        CloseTransportSockets();
        LogNote("bind de l'injecteur a echoue");
        return false;
    }
    int name_len = sizeof(g_injector_addr);
    if (getsockname(g_injector, reinterpret_cast<sockaddr*>(&g_injector_addr), &name_len) != 0) {
        CloseTransportSockets();
        LogNote("port injecteur illisible");
        return false;
    }
    SOCKET chan_socks[4] = {
        g_channel[0].sock,
        g_channel[0].data,
        g_channel[1].sock,
        g_channel[1].data,
    };
    for (int i = 0; i < 4; ++i) {
        g_sock_events[i] = WSACreateEvent();
        if (g_sock_events[i] == WSA_INVALID_EVENT ||
            WSAEventSelect(chan_socks[i], g_sock_events[i], FD_READ) != 0) {
            CloseTransportSockets();
            LogNote("evenement reseau du transport indisponible");
            return false;
        }
    }
    g_wake_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (g_wake_event == nullptr) {
        CloseTransportSockets();
        LogNote("evenement reveil du transport indisponible");
        return false;
    }
    g_stop = 0;
    if (kTransportCut != 3) {
        g_thread = CreateThread(nullptr, 0, ThreadProc, nullptr, 0, nullptr);
        if (g_thread == nullptr) {
            CloseTransportSockets();
            LogNote("thread du transport non cree");
            return false;
        }
    }
    InterlockedExchange(&g_active, 1);
    const unsigned char* ip = reinterpret_cast<const unsigned char*>(&g_relay.sin_addr);
    const char* cut = kTransportCut == 1 ? "depot_recv" : kTransportCut == 2 ? "WSASendTo" : kTransportCut == 3 ? "thread_heartbeat" : "aucune";
    char note[200];
    std::snprintf(note, sizeof(note),
                  "transport pret relay=%u.%u.%u.%u:%u donnees=%u payloads 9100 et 9103 tels quels coupe=%s", ip[0],
                  ip[1], ip[2], ip[3], port, static_cast<unsigned>(port + 1), cut);
    LogNote(note);
    return true;
}

void TransportEnqueuePayload(unsigned short game_port, const void* data, int len) {
    if (!TransportActive() || TransportSending() || !g_lock_ready || game_port == 0 || data == nullptr || len <= 0 ||
        len > kMaxPayload) {
        return;
    }
    bool stored = false;
    EnterCriticalSection(&g_lock);
    if (g_out_count < kOutMax) {
        Outbound& slot = g_out[g_out_tail];
        slot.port = game_port;
        slot.len = len;
        std::memcpy(slot.data, data, static_cast<size_t>(len));
        g_out_tail = (g_out_tail + 1) % kOutMax;
        ++g_out_count;
        stored = true;
    }
    LeaveCriticalSection(&g_lock);
    if (stored && g_wake_event != nullptr) {
        SetEvent(g_wake_event);
    } else if (!stored) {
        LogOnce(&g_once_send, "file d'envoi pleine, copie WSASendTo non prise");
    }
}

void TransportCopyPayload(unsigned short game_port, const void* data, int len) {
    if (!TransportActive() || TransportSending()) {
        return;
    }
    const int index = IndexFor(game_port);
    if (index < 0) {
        return;
    }
    if (len <= 0 || data == nullptr) {
        return;
    }
    if (len > kMaxPayload) {
        LogOnce(&g_once_big, "payload au-dela de 8192 octets, copie non envoyee");
        return;
    }
    const int sent = RawSend(g_channel[index].data, data, len, g_relay_data);
    if (sent != len) {
        LogOnce(&g_once_send, "copie vers le relais non partie");
    }
}

void TransportNoteSocket(SOCKET sock) {
    if (!TransportActive() || sock == INVALID_SOCKET) {
        return;
    }
    uint32_t ip_be = 0;
    const unsigned short port = SocketPort(sock, &ip_be);
    if (!scct::relay::IsGamePort(port)) {
        return;
    }
    EnterCriticalSection(&g_lock);
    RememberBound(port, ip_be);
    LeaveCriticalSection(&g_lock);
}

void TransportNoteOutboundTarget(unsigned short game_port, const sockaddr* to, int tolen) {
    if (!TransportActive() || TransportSending() || game_port != kPortSession ||
        to == nullptr || tolen < static_cast<int>(sizeof(sockaddr_in))) {
        return;
    }
    sockaddr_in dst{};
    std::memcpy(&dst, to, sizeof(dst));
    if (dst.sin_family != AF_INET || dst.sin_port != htons(game_port)) {
        return;
    }
    const uint32_t ip_be = dst.sin_addr.s_addr;
    const uint32_t ip_h = ntohl(ip_be);
    if (ip_be == 0 || ip_be == INADDR_BROADCAST || (ip_h & 0xFFu) == 0xFFu) {
        return;
    }
    const int index = IndexFor(game_port);
    if (index < 0) {
        return;
    }
    EnterCriticalSection(&g_lock);
    g_channel[index].expected_peer_ip_be = ip_be;
    LeaveCriticalSection(&g_lock);
}

bool TransportTakePeer(SOCKET sock, const sockaddr* observed, int observed_len, sockaddr_in* peer) {
    if (!TransportActive() || TransportSending() || observed == nullptr || peer == nullptr ||
        observed_len < static_cast<int>(sizeof(sockaddr_in)) || g_injector == INVALID_SOCKET) {
        return false;
    }
    sockaddr_in src{};
    std::memcpy(&src, observed, sizeof(src));
    if (src.sin_family != AF_INET || src.sin_port != g_injector_addr.sin_port) {
        return false;
    }
    uint32_t local_ip = 0;
    const unsigned short local_port = SocketPort(sock, &local_ip);
    const bool is_local_src = (src.sin_addr.s_addr == htonl(INADDR_LOOPBACK)) ||
                              ((ntohl(src.sin_addr.s_addr) >> 24) == 127) ||
                              (local_ip != 0 && src.sin_addr.s_addr == local_ip);
    if (!is_local_src) {
        return false;
    }
    const int index = IndexFor(local_port);
    if (index < 0) {
        return false;
    }
    sockaddr_in found{};
    bool ok = false;
    EnterCriticalSection(&g_lock);
    ok = TakeClaim(g_channel[index], &found);
    if (!ok && g_channel[index].peer_known && g_channel[index].peer_port_host != 0) {
        found = PeerAddress(g_channel[index]);
        ok = found.sin_addr.s_addr != 0 && found.sin_port != 0;
    }
    if (ok && local_port == kPortSession && g_channel[index].expected_peer_ip_be != 0) {
        found.sin_addr.s_addr = g_channel[index].expected_peer_ip_be;
    }
    LeaveCriticalSection(&g_lock);
    if (!ok) {
        return false;
    }
    *peer = found;
    return true;
}

bool TransportRewriteFrom(SOCKET sock, sockaddr* from, int* fromlen) {
    sockaddr_in peer{};
    if (from == nullptr || fromlen == nullptr || *fromlen < static_cast<int>(sizeof(sockaddr_in)) ||
        !TransportTakePeer(sock, from, *fromlen, &peer)) {
        return false;
    }
    std::memcpy(from, &peer, sizeof(peer));
    *fromlen = static_cast<int>(sizeof(peer));
    return true;
}

extern "C" int ScctCoopNet_Deposit(const void* payload, int payload_len, const sockaddr* peer, int peer_len, void* buf,
                                  int buf_cap, sockaddr* from, int* fromlen) {
    if (payload_len < 0 || (payload == nullptr && payload_len != 0) || buf == nullptr || buf_cap < payload_len ||
        peer == nullptr || peer_len < static_cast<int>(sizeof(sockaddr_in)) || from == nullptr || fromlen == nullptr ||
        *fromlen < static_cast<int>(sizeof(sockaddr_in))) {
        return -1;
    }
    sockaddr_in src{};
    std::memcpy(&src, peer, sizeof(src));
    if (src.sin_family != AF_INET || src.sin_addr.s_addr == 0 || src.sin_port == 0) {
        return -1;
    }
    if (payload_len > 0) {
        std::memmove(buf, payload, static_cast<size_t>(payload_len));
    }
    std::memcpy(from, &src, sizeof(src));
    *fromlen = static_cast<int>(sizeof(src));
    return payload_len;
}

extern "C" int ScctCoopNet_Peer(unsigned short game_port, sockaddr* out, int* out_len) {
    return CopyKnownPeer(game_port, out, out_len);
}

TransportCompleteFn TransportPrepareRecv(SOCKET sock, sockaddr* from, int* fromlen, LPWSAOVERLAPPED overlapped,
                                         TransportCompleteFn user_complete) {
    if (!TransportActive() || overlapped == nullptr || from == nullptr || fromlen == nullptr) {
        return user_complete;
    }
    uint32_t ip_be = 0;
    if (!scct::relay::IsGamePort(SocketPort(sock, &ip_be))) {
        return user_complete;
    }
    EnterCriticalSection(&g_lock);
    Pending* slot = FindPending(overlapped);
    if (slot == nullptr) {
        for (Pending& candidate : g_pending) {
            if (!candidate.used) {
                slot = &candidate;
                break;
            }
        }
    }
    if (slot == nullptr) {
        LeaveCriticalSection(&g_lock);
        return user_complete;
    }
    slot->used = true;
    slot->sock = sock;
    slot->from = from;
    slot->fromlen = fromlen;
    slot->overlapped = overlapped;
    slot->user = user_complete;
    LeaveCriticalSection(&g_lock);
    // La routine du jeu reste en place. La remplacer par TransportApc croise
    // le rappel Winsock avec le sockaddr du jeu et fait fault ntdll 0xc000041d.
    return user_complete;
}

bool TransportCompleteOverlapped(SOCKET sock, LPWSAOVERLAPPED overlapped) {
    if (!TransportActive() || overlapped == nullptr || !g_lock_ready) {
        return false;
    }
    sockaddr* from = nullptr;
    int* fromlen = nullptr;
    EnterCriticalSection(&g_lock);
    Pending* slot = FindPending(overlapped);
    if (slot != nullptr && slot->sock == sock && slot->user == nullptr) {
        from = slot->from;
        fromlen = slot->fromlen;
        slot->used = false;
    }
    LeaveCriticalSection(&g_lock);
    if (from == nullptr || fromlen == nullptr) {
        return false;
    }
    return TransportRewriteFrom(sock, from, fromlen);
}

void TransportRecvDone(SOCKET sock, LPWSAOVERLAPPED overlapped, int result, bool pending) {
    (void)sock;
    if (pending || overlapped == nullptr || !g_lock_ready) {
        return;
    }
    EnterCriticalSection(&g_lock);
    Pending* slot = FindPending(overlapped);
    if (slot != nullptr && (result == 0 || !pending)) {
        slot->used = false;
    }
    LeaveCriticalSection(&g_lock);
}
