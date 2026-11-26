#pragma once

// Controle et donnees sont deux ports. Le controle (REGISTER, HEARTBEAT, ATTACH)
// n'est lu que sur le port de controle. Le port de donnees recopie toute longueur.

#include <cstdint>
#include <cstring>

namespace scct::relay {

constexpr uint32_t kMagic = 0x31524353u;  // 'SCR1' petit-boutiste
constexpr uint16_t kVersion = 1;
constexpr uint16_t kMarker = 0x7EC3;

constexpr uint16_t kRegister = 1;
constexpr uint16_t kRegisterOk = 2;
constexpr uint16_t kHeartbeat = 3;
constexpr uint16_t kAttach = 7;
constexpr uint16_t kRefused = 5;
constexpr uint16_t kAddress = 6;

constexpr uint16_t kRefuseRate = 1;
constexpr uint16_t kRefuseVersion = 2;
constexpr uint16_t kRefuseFull = 3;
constexpr uint16_t kRefuseUnknown = 4;
constexpr uint16_t kRefuseNoPeer = 5;

constexpr uint16_t kPortDiscovery = 9100;
constexpr uint16_t kPortSession = 9103;

constexpr int kMaxPayload = 8192;

inline void Store16(uint8_t* p, uint16_t value) {
    p[0] = static_cast<uint8_t>(value);
    p[1] = static_cast<uint8_t>(value >> 8);
}

inline void Store32(uint8_t* p, uint32_t value) {
    p[0] = static_cast<uint8_t>(value);
    p[1] = static_cast<uint8_t>(value >> 8);
    p[2] = static_cast<uint8_t>(value >> 16);
    p[3] = static_cast<uint8_t>(value >> 24);
}

inline uint16_t Load16(const uint8_t* p) {
    return static_cast<uint16_t>(p[0] | (static_cast<uint16_t>(p[1]) << 8));
}

inline uint32_t Load32(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}

inline bool IsGamePort(uint16_t port) {
    return port == kPortDiscovery || port == kPortSession;
}

inline bool MagicVersion(const uint8_t* p, int len, uint16_t* version, uint16_t* type) {
    if (len < 8 || Load32(p) != kMagic) {
        return false;
    }
    *version = Load16(p + 4);
    *type = Load16(p + 6);
    return true;
}

inline int BuildRegister(uint8_t* out, uint16_t game_port) {
    Store32(out, kMagic);
    Store16(out + 4, kVersion);
    Store16(out + 6, kRegister);
    Store16(out + 8, game_port);
    Store16(out + 10, kMarker);
    return 12;
}

inline bool ParseRegister(const uint8_t* p, int len, uint16_t* game_port) {
    uint16_t version = 0;
    uint16_t type = 0;
    if (len != 12 || !MagicVersion(p, len, &version, &type)) {
        return false;
    }
    if (version != kVersion || type != kRegister || Load16(p + 10) != kMarker) {
        return false;
    }
    *game_port = Load16(p + 8);
    return true;
}

inline int BuildHeartbeat(uint8_t* out) {
    Store32(out, kMagic);
    Store16(out + 4, kVersion);
    Store16(out + 6, kHeartbeat);
    Store16(out + 8, kMarker);
    return 10;
}

inline bool ParseHeartbeat(const uint8_t* p, int len) {
    uint16_t version = 0;
    uint16_t type = 0;
    if (len != 10 || !MagicVersion(p, len, &version, &type)) {
        return false;
    }
    return version == kVersion && type == kHeartbeat && Load16(p + 8) == kMarker;
}

inline int BuildAttach(uint8_t* out, uint32_t peer_id) {
    Store32(out, kMagic);
    Store16(out + 4, kVersion);
    Store16(out + 6, kAttach);
    Store32(out + 8, peer_id);
    Store16(out + 12, kMarker);
    return 14;
}

inline bool ParseAttach(const uint8_t* p, int len, uint32_t* peer_id) {
    uint16_t version = 0;
    uint16_t type = 0;
    if (len != 14 || !MagicVersion(p, len, &version, &type)) {
        return false;
    }
    if (version != kVersion || type != kAttach || Load16(p + 12) != kMarker) {
        return false;
    }
    *peer_id = Load32(p + 8);
    return true;
}

inline int BuildRegisterOk(uint8_t* out, uint32_t peer_id, bool has_other, const uint8_t ip[4], uint16_t port_host) {
    std::memset(out, 0, 21);
    Store32(out, kMagic);
    Store16(out + 4, kVersion);
    Store16(out + 6, kRegisterOk);
    Store32(out + 8, peer_id);
    out[12] = has_other ? 1 : 0;
    if (has_other && ip != nullptr) {
        std::memcpy(out + 13, ip, 4);
        Store16(out + 17, port_host);
    }
    Store16(out + 19, kMarker);
    return 21;
}

inline bool ParseRegisterOk(const uint8_t* p, int len, uint32_t* peer_id, uint8_t* has_other, uint8_t ip[4],
                            uint16_t* port_host) {
    uint16_t version = 0;
    uint16_t type = 0;
    if (len != 21 || !MagicVersion(p, len, &version, &type)) {
        return false;
    }
    if (version != kVersion || type != kRegisterOk || Load16(p + 19) != kMarker) {
        return false;
    }
    *peer_id = Load32(p + 8);
    *has_other = p[12];
    std::memcpy(ip, p + 13, 4);
    *port_host = Load16(p + 17);
    return true;
}

inline int BuildRefused(uint8_t* out, uint16_t reason) {
    Store32(out, kMagic);
    Store16(out + 4, kVersion);
    Store16(out + 6, kRefused);
    Store16(out + 8, reason);
    Store16(out + 10, kMarker);
    return 12;
}

inline bool ParseRefused(const uint8_t* p, int len, uint16_t* reason) {
    uint16_t version = 0;
    uint16_t type = 0;
    if (len != 12 || !MagicVersion(p, len, &version, &type)) {
        return false;
    }
    if (version != kVersion || type != kRefused || Load16(p + 10) != kMarker) {
        return false;
    }
    *reason = Load16(p + 8);
    return true;
}

inline int BuildAddress(uint8_t* out, const uint8_t ip[4], uint16_t port_host, uint16_t game_port) {
    Store32(out, kMagic);
    Store16(out + 4, kVersion);
    Store16(out + 6, kAddress);
    std::memcpy(out + 8, ip, 4);
    Store16(out + 12, port_host);
    Store16(out + 14, game_port);
    Store16(out + 16, kMarker);
    return 18;
}

inline bool ParseAddress(const uint8_t* p, int len, uint8_t ip[4], uint16_t* port_host, uint16_t* game_port) {
    uint16_t version = 0;
    uint16_t type = 0;
    if (len != 18 || !MagicVersion(p, len, &version, &type)) {
        return false;
    }
    if (version != kVersion || type != kAddress || Load16(p + 16) != kMarker) {
        return false;
    }
    std::memcpy(ip, p + 8, 4);
    *port_host = Load16(p + 12);
    *game_port = Load16(p + 14);
    return true;
}

// Controle seulement si le datagramme est exactement un REGISTER ou un HEARTBEAT.
// Toute autre taille, y compris un payload qui commencerait par le magic, reste un payload.
inline bool IsExactControl(const uint8_t* p, int len, uint16_t* game_port, bool* heartbeat) {
    if (ParseRegister(p, len, game_port)) {
        *heartbeat = false;
        return true;
    }
    if (ParseHeartbeat(p, len)) {
        *heartbeat = true;
        *game_port = 0;
        return true;
    }
    return false;
}

// Magic et type de controle, mais ce n'est pas un message exact.
// Seulement les tailles de controle : un payload plus long est toujours un payload.
inline bool IsBrokenControl(const uint8_t* p, int len) {
    if (len != 10 && len != 12 && len != 14 && len != 18 && len != 21) {
        return false;
    }
    uint16_t version = 0;
    uint16_t type = 0;
    if (!MagicVersion(p, len, &version, &type)) {
        return false;
    }
    if (type != kRegister && type != kHeartbeat && type != kAttach && type != kRegisterOk && type != kRefused &&
        type != kAddress) {
        return false;
    }
    uint16_t game_port = 0;
    bool heartbeat = false;
    return !IsExactControl(p, len, &game_port, &heartbeat);
}

}  // namespace scct::relay
