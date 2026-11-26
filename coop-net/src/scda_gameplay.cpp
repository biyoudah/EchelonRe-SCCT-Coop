#include "scda_gameplay.h"
#include "scda_hooks.h"

#include <windows.h>

#include <MinHook.h>

#include <cmath>
#include <cstdlib>
#include <cstring>
#include <cwchar>

namespace {

using CoopDispatchIsAvailableFn = int(__fastcall*)(void* self, void* edx);
using CoopDispatchInitInteractFn = void(__fastcall*)(void* self, void* edx, void* instigator, uint8_t action_idx);
using GameEngineTickFn = void(__fastcall*)(void* self, void* edx, float dt);
using ENpcZoneGetDescriptionFn = void(__fastcall*)(void* self, void* edx, void* actions_arr, void* epc);
using SpeechDispatcherTriggerFn = int(__fastcall*)(void* self, void* edx, void* npc, void* speech_type_ptr,
                                                 void* out_pattern);
using SetLocationFn = int(__thiscall*)(void* actor, const float* dest_xyz, int bNoCheck);
using GetOtherPlayerFn = void*(__thiscall*)(void* epc);
using PlayAnimCoreFn = int(__thiscall*)(void* mesh_inst, int channel, uint32_t anim_fname, int* out_seq,
                                        int* out_flags, int arg5, int rate, int bLoop, int tween, int flags);
using AppStrihashFn = uint32_t(__cdecl*)(const wchar_t* str);

constexpr uintptr_t kRvaSetLocation = 0x90230u;
constexpr uintptr_t kRvaGetOtherPlayer = 0x1037C0u;
constexpr uintptr_t kRvaGameEngineTick = 0x13DD30u;
constexpr uintptr_t kRvaPlayAnimCore = 0x158010u;
constexpr uintptr_t kRvaCoopDispatchIsAvailable = 0x301BF0u;
constexpr uintptr_t kRvaCoopDispatchInitInteract = 0x302580u;
constexpr uintptr_t kRvaENpcZoneGetDescription = 0x304170u;
constexpr uintptr_t kRvaSpeechDispatcherTrigger = 0x30EE40u;
constexpr uintptr_t kRvaAppStrihash = 0x3D4230u;
constexpr uintptr_t kRvaFNameHash = 0xA09FB8u;

constexpr float kB2BLimitSentinel = 404.25f;
constexpr float kDualInterroPartnerRadius = 250.0f;

uintptr_t g_exe_base = 0;
GameEngineTickFn g_orig_game_engine_tick = nullptr;
CoopDispatchIsAvailableFn g_orig_coop_dispatch_is_available = nullptr;
CoopDispatchInitInteractFn g_orig_coop_dispatch_init_interact = nullptr;
ENpcZoneGetDescriptionFn g_orig_npc_zone_get_desc = nullptr;
SpeechDispatcherTriggerFn g_orig_speech_trigger = nullptr;

volatile LONG g_interro_stage = 0;             // 0 = initial, 1 = solo refusal played, 2 = team interro played
volatile LONG g_current_coop_is_b2b = 0;       // 1 while active dispatch is a B2B volume
volatile LONG g_b2b_prompt_mode = 0;           // 0 = none, 1 = Init, 2 = Complete, 3 = Cancel
volatile LONG g_dual_interro_prompt_active = 0;

struct B2BState {
    uint8_t* active_vol = nullptr;
    int phase = 0;  // 0 = IDLE, 1 = P1_WAITING, 2 = CLIMBING
    uint8_t* p1_epc = nullptr;
    uint8_t* p2_epc = nullptr;
    float start_z = 0.0f;
    float top_z = 0.0f;
    float cur_z = 0.0f;
    DWORD last_tick_ms = 0;
    DWORD wait_start_ms = 0;
    bool loop_anim_started = false;
    float p1_wait_pos[3]{};
};

B2BState g_b2b{};
uint8_t* g_local_epc = nullptr;
uint8_t* g_local_disp = nullptr;
int g_autotest_mode = 0;  // 0 = off, 1 = host, 2 = guest
int g_autotest_step = 0;
DWORD g_autotest_t0 = 0;
uint8_t* g_known_b2b_vol4 = nullptr;
uint8_t* g_known_vol8 = nullptr;

bool CopyLabel(wchar_t* out_buf, size_t out_cap, const wchar_t* text) {
    if (!out_buf || out_cap == 0 || !text) {
        return false;
    }
    wcsncpy_s(out_buf, out_cap, text, _TRUNCATE);
    return true;
}

bool IsValidPtr(const void* p) {
    const auto v = reinterpret_cast<uintptr_t>(p);
    return v >= 0x10000u && v < 0x7FFF0000u;
}

float Dist2D(const float* a, const float* b) {
    const float dx = a[0] - b[0];
    const float dy = a[1] - b[1];
    return std::sqrt(dx * dx + dy * dy);
}

float Dist3D(const float* a, const float* b) {
    const float dx = a[0] - b[0];
    const float dy = a[1] - b[1];
    const float dz = a[2] - b[2];
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

int SafeSetActorLocation(void* actor, const float* xyz, int bNoCheck) {
    if (!IsValidPtr(actor) || !xyz || g_exe_base == 0) {
        return 0;
    }
    auto set_loc = reinterpret_cast<SetLocationFn>(g_exe_base + kRvaSetLocation);
    return set_loc(actor, xyz, bNoCheck);
}

int SafeMoveActorLocation(void* actor, const float* xyz) {
    if (SafeSetActorLocation(actor, xyz, 0) != 0) {
        return 1;
    }
    return SafeSetActorLocation(actor, xyz, 1);
}

void ZeroPawnVelocity(uint8_t* pawn) {
    if (!IsValidPtr(pawn)) {
        return;
    }
    auto* vel = reinterpret_cast<float*>(pawn + 0x100);
    vel[0] = 0.0f;
    vel[1] = 0.0f;
    vel[2] = 0.0f;
}

uint32_t FindLoadedFName(const wchar_t* name) {
    if (!name || !*name || g_exe_base == 0) {
        return 0;
    }
    auto strihash = reinterpret_cast<AppStrihashFn>(g_exe_base + kRvaAppStrihash);
    auto** hash_table = reinterpret_cast<uint8_t**>(g_exe_base + kRvaFNameHash);
    const uint32_t bucket = strihash(name) & 0xFFFu;
    uint8_t* entry = hash_table[bucket];
    for (int guard = 0; guard < 128 && IsValidPtr(entry); ++guard) {
        const auto* entry_name = reinterpret_cast<const wchar_t*>(entry + 0x0C);
        if (_wcsicmp(entry_name, name) == 0) {
            return *reinterpret_cast<const uint32_t*>(entry);
        }
        entry = *reinterpret_cast<uint8_t**>(entry + 0x08);
    }
    return 0;
}

void TryPlayB2BAnim(uint8_t* pawn, const wchar_t* anim_name, int bLoop) {
    if (!IsValidPtr(pawn) || g_exe_base == 0) {
        return;
    }
    uint8_t* mesh_inst = *reinterpret_cast<uint8_t**>(pawn + 0x518);
    if (!IsValidPtr(mesh_inst)) {
        return;
    }
    const uint32_t fname = FindLoadedFName(anim_name);
    if (fname == 0) {
        return;
    }
    int out_seq = -1;
    int out_flags = 0;
    auto play_anim = reinterpret_cast<PlayAnimCoreFn>(g_exe_base + kRvaPlayAnimCore);
    play_anim(mesh_inst, 0, fname, &out_seq, &out_flags, -1, 1, bLoop, 0, 0x1001);
}

bool IsAuthorizedPawnForMove(uint8_t* pawn) {
    if (!IsValidPtr(pawn)) {
        return false;
    }
    uint8_t* local_pawn = IsValidPtr(g_local_epc) ? *reinterpret_cast<uint8_t**>(g_local_epc + 0x4A0) : nullptr;
    return !IsValidPtr(local_pawn) || pawn == local_pawn;
}

void* SafeGetOtherPlayer(void* epc) {
    if (!IsValidPtr(epc) || g_exe_base == 0) {
        return nullptr;
    }
    auto get_other = reinterpret_cast<GetOtherPlayerFn>(g_exe_base + kRvaGetOtherPlayer);
    return get_other(epc);
}

void ComputeVolAxis(uint8_t* vol, float* out_fx, float* out_fy) {
    const int32_t yaw = *reinterpret_cast<int32_t*>(vol + 0xF8);
    const double rad = static_cast<double>(yaw) * (3.14159265358979323846 / 32768.0);
    *out_fx = static_cast<float>(std::cos(rad));
    *out_fy = static_cast<float>(std::sin(rad));
}

bool IsB2BVolume(uint8_t* vol) {
    if (!IsValidPtr(vol)) {
        return false;
    }
    if (*(vol + 0x3A8) != 0) {
        return false;
    }
    const float limit = *reinterpret_cast<float*>(vol + 0x3AC);
    return std::fabs(limit - kB2BLimitSentinel) < 0.5f;
}

bool IsPartnerWaitingInVolume(uint8_t* vol, uint8_t* epc, uint8_t** out_partner_epc) {
    auto* other_epc = reinterpret_cast<uint8_t*>(SafeGetOtherPlayer(epc));
    if (out_partner_epc) {
        *out_partner_epc = other_epc;
    }
    if (!IsValidPtr(other_epc)) {
        return false;
    }
    if (g_b2b.phase == 1 && g_b2b.active_vol == vol && g_b2b.p1_epc == other_epc) {
        return true;
    }
    auto* my_pawn = *reinterpret_cast<uint8_t**>(epc + 0x4A0);
    auto* other_pawn = *reinterpret_cast<uint8_t**>(other_epc + 0x4A0);
    if (!IsValidPtr(my_pawn) || !IsValidPtr(other_pawn)) {
        return false;
    }
    const auto* vol_loc = reinterpret_cast<const float*>(vol + 0xE8);
    const auto* my_loc = reinterpret_cast<const float*>(my_pawn + 0xE8);
    const auto* other_loc = reinterpret_cast<const float*>(other_pawn + 0xE8);
    return Dist2D(other_loc, vol_loc) <= 85.0f && std::fabs(other_loc[2] - my_loc[2]) < 110.0f &&
           other_loc[2] <= vol_loc[2] + 80.0f;
}

void ResetB2BState() {
    g_b2b = B2BState{};
    InterlockedExchange(&g_current_coop_is_b2b, 0);
    InterlockedExchange(&g_b2b_prompt_mode, 0);
}

void BeginB2BClimb(uint8_t* vol, uint8_t* p1_epc, uint8_t* p2_epc, float start_z) {
    if (!IsValidPtr(vol)) {
        return;
    }
    const auto* vol_loc = reinterpret_cast<const float*>(vol + 0xE8);
    const int32_t vol_yaw = *reinterpret_cast<const int32_t*>(vol + 0xF8);
    const float rope = *reinterpret_cast<float*>(vol + 0x3B0);
    float fx = 1.0f, fy = 0.0f;
    ComputeVolAxis(vol, &fx, &fy);

    g_b2b.active_vol = vol;
    g_b2b.phase = 2;
    g_b2b.p1_epc = p1_epc;
    g_b2b.p2_epc = p2_epc;
    g_b2b.start_z = start_z;
    g_b2b.cur_z = start_z;
    g_b2b.top_z = vol_loc[2] + rope * 0.5f + 56.0f;
    g_b2b.last_tick_ms = GetTickCount();
    g_b2b.loop_anim_started = false;

    if (IsValidPtr(p1_epc)) {
        auto* p1_pawn = *reinterpret_cast<uint8_t**>(p1_epc + 0x4A0);
        if (IsValidPtr(p1_pawn)) {
            ZeroPawnVelocity(p1_pawn);
            *reinterpret_cast<int32_t*>(p1_pawn + 0xF8) = vol_yaw + 32768;
            if (IsAuthorizedPawnForMove(p1_pawn)) {
                const float pos1[3] = {vol_loc[0] - fx * 26.0f, vol_loc[1] - fy * 26.0f, start_z};
                SafeMoveActorLocation(p1_pawn, pos1);
            }
            TryPlayB2BAnim(p1_pawn, L"coopbktobk_climb_ini_p", 0);
        }
    }
    if (IsValidPtr(p2_epc)) {
        auto* p2_pawn = *reinterpret_cast<uint8_t**>(p2_epc + 0x4A0);
        if (IsValidPtr(p2_pawn)) {
            ZeroPawnVelocity(p2_pawn);
            *reinterpret_cast<int32_t*>(p2_pawn + 0xF8) = vol_yaw;
            if (IsAuthorizedPawnForMove(p2_pawn)) {
                const float pos2[3] = {vol_loc[0] + fx * 26.0f, vol_loc[1] + fy * 26.0f, start_z};
                SafeMoveActorLocation(p2_pawn, pos2);
            }
            TryPlayB2BAnim(p2_pawn, L"coopbktobk_climb_ini_p", 0);
        }
    }

    ScdaLogMsg("scda_b2b_start_climb vol=0x%p start_z=%.1f top_z=%.1f", vol, static_cast<double>(start_z),
               static_cast<double>(g_b2b.top_z));
}

void HandleB2BInteract(uint8_t* vol, uint8_t* instigator) {
    if (!IsValidPtr(vol) || !IsValidPtr(instigator)) {
        return;
    }
    auto* pawn = *reinterpret_cast<uint8_t**>(instigator + 0x4A0);
    if (!IsValidPtr(pawn)) {
        return;
    }
    const auto* vol_loc = reinterpret_cast<const float*>(vol + 0xE8);
    const auto* pawn_loc = reinterpret_cast<const float*>(pawn + 0xE8);
    const float rope = *reinterpret_cast<float*>(vol + 0x3B0);
    float fx = 1.0f, fy = 0.0f;
    ComputeVolAxis(vol, &fx, &fy);

    if (g_b2b.phase == 1 && g_b2b.active_vol == vol && g_b2b.p1_epc == instigator) {
        ResetB2BState();
        ScdaLogMsg("scda_b2b_cancel vol=0x%p", vol);
        return;
    }

    uint8_t* other_epc = nullptr;
    if (IsPartnerWaitingInVolume(vol, instigator, &other_epc) && IsValidPtr(other_epc)) {
        auto* other_pawn = *reinterpret_cast<uint8_t**>(other_epc + 0x4A0);
        const float start_z = IsValidPtr(other_pawn) ? *reinterpret_cast<const float*>(other_pawn + 0xF0) : pawn_loc[2];
        BeginB2BClimb(vol, other_epc, instigator, start_z);
        return;
    }

    const float wait_pos[3] = {vol_loc[0] - fx * 26.0f, vol_loc[1] - fy * 26.0f, pawn_loc[2]};
    SafeSetActorLocation(pawn, wait_pos, 1);

    g_b2b.active_vol = vol;
    g_b2b.phase = 1;
    g_b2b.p1_epc = instigator;
    g_b2b.p2_epc = other_epc;
    g_b2b.start_z = pawn_loc[2];
    g_b2b.cur_z = pawn_loc[2];
    g_b2b.top_z = vol_loc[2] + rope * 0.5f + 56.0f;
    g_b2b.wait_start_ms = GetTickCount();
    g_b2b.last_tick_ms = g_b2b.wait_start_ms;
    std::memcpy(g_b2b.p1_wait_pos, wait_pos, sizeof(wait_pos));

    ScdaLogMsg("scda_b2b_init_p1 vol=0x%p pos=(%.1f,%.1f,%.1f) top_z=%.1f", vol, static_cast<double>(wait_pos[0]),
               static_cast<double>(wait_pos[1]), static_cast<double>(wait_pos[2]), static_cast<double>(g_b2b.top_z));
}

void PlaceAtShaftExit(uint8_t* pawn, uint8_t* vol, float top_z, float sign) {
    if (!IsValidPtr(pawn) || !IsValidPtr(vol)) {
        return;
    }
    const auto* vol_loc = reinterpret_cast<const float*>(vol + 0xE8);
    float fx = 1.0f, fy = 0.0f;
    ComputeVolAxis(vol, &fx, &fy);

    ZeroPawnVelocity(pawn);
    if (IsAuthorizedPawnForMove(pawn)) {
        const float exit_pos[3] = {vol_loc[0] + fx * 68.0f * sign, vol_loc[1] + fy * 68.0f * sign, top_z + 24.0f};
        SafeMoveActorLocation(pawn, exit_pos);
    }
    TryPlayB2BAnim(pawn, L"coopbktobk_climb_end_p", 0);
}

int __fastcall Hook_CoopDispatchIsAvailable(void* self, void* edx) {
    if (GetActiveScdaMap()[0] == L'\0' || !IsValidPtr(self)) {
        return g_orig_coop_dispatch_is_available(self, edx);
    }

    auto* disp = reinterpret_cast<uint8_t*>(self);
    auto* ebp = *reinterpret_cast<uint8_t**>(disp + 0x2B8);
    auto* vol = *reinterpret_cast<uint8_t**>(disp + 0x2C8);

    if (IsValidPtr(ebp)) {
        g_local_disp = disp;
        g_local_epc = ebp;
    }

    if (IsB2BVolume(vol)) {
        g_known_b2b_vol4 = vol;
        InterlockedExchange(&g_current_coop_is_b2b, 1);

        if (!IsValidPtr(ebp)) {
            InterlockedExchange(&g_b2b_prompt_mode, 0);
            return 0;
        }
        auto* pawn = *reinterpret_cast<uint8_t**>(ebp + 0x4A0);
        if (!IsValidPtr(pawn)) {
            InterlockedExchange(&g_b2b_prompt_mode, 0);
            return 0;
        }
        const auto* vol_loc = reinterpret_cast<const float*>(vol + 0xE8);
        const auto* pawn_loc = reinterpret_cast<const float*>(pawn + 0xE8);
        if (g_b2b.phase == 2 || pawn_loc[2] > vol_loc[2] + 80.0f) {
            InterlockedExchange(&g_b2b_prompt_mode, 0);
            return 0;
        }

        if (g_b2b.phase == 1 && g_b2b.active_vol == vol && g_b2b.p1_epc == ebp) {
            InterlockedExchange(&g_b2b_prompt_mode, 3);
            return 1;
        }
        if (IsPartnerWaitingInVolume(vol, ebp, nullptr)) {
            InterlockedExchange(&g_b2b_prompt_mode, 2);
            return 1;
        }
        InterlockedExchange(&g_b2b_prompt_mode, 1);
        return 1;
    }

    InterlockedExchange(&g_current_coop_is_b2b, 0);
    InterlockedExchange(&g_b2b_prompt_mode, 0);

    int avail = 0;
    bool lifted = false;
    float* vol_z = nullptr;
    float orig_z = 0.0f;
    if (IsValidPtr(vol) && *(vol + 0x3A8) == 0 && IsValidPtr(ebp)) {
        auto* pawn = *reinterpret_cast<uint8_t**>(ebp + 0x4A0);
        if (IsValidPtr(pawn)) {
            vol_z = reinterpret_cast<float*>(vol + 0xF0);
            orig_z = *vol_z;
            const float pawn_z = *reinterpret_cast<const float*>(pawn + 0xF0);
            if (pawn_z > orig_z && pawn_z <= orig_z + 450.0f) {
                *vol_z = pawn_z + 64.0f;
                lifted = true;
            }
        }
    }

    avail = g_orig_coop_dispatch_is_available(self, edx);
    if (lifted) {
        *vol_z = orig_z;
    }

    if (IsValidPtr(vol) && (avail != 0 || g_autotest_mode != 0)) {
        static DWORD last_log_ms = 0;
        const DWORD now = GetTickCount();
        if (avail != 0 && now - last_log_ms > 500) {
            last_log_ms = now;
            const auto* loc = reinterpret_cast<const float*>(vol + 0xE8);
            ScdaLogMsg("scda_coop_avail vol=0x%p type=%u limit=%.1f rope=%.1f loc=(%.1f,%.1f,%.1f) result=%d",
                       vol, static_cast<unsigned>(*(vol + 0x3A8)),
                       static_cast<double>(*reinterpret_cast<const float*>(vol + 0x3AC)),
                       static_cast<double>(*reinterpret_cast<const float*>(vol + 0x3B0)),
                       static_cast<double>(loc[0]), static_cast<double>(loc[1]), static_cast<double>(loc[2]), avail);
        }
    }
    return avail;
}

void __fastcall Hook_CoopDispatchInitInteract(void* self, void* edx, void* instigator, uint8_t action_idx) {
    if (GetActiveScdaMap()[0] == L'\0' || !IsValidPtr(self)) {
        g_orig_coop_dispatch_init_interact(self, edx, instigator, action_idx);
        return;
    }

    auto* disp = reinterpret_cast<uint8_t*>(self);
    auto* vol = *reinterpret_cast<uint8_t**>(disp + 0x2C8);

    if (IsB2BVolume(vol)) {
        *(disp + 0x2D0) &= static_cast<uint8_t>(~1u);
        HandleB2BInteract(vol, reinterpret_cast<uint8_t*>(instigator));
        return;
    }

    bool lifted = false;
    float* vol_z = nullptr;
    float orig_z = 0.0f;
    auto* epc = reinterpret_cast<uint8_t*>(instigator);
    if (IsValidPtr(vol) && *(vol + 0x3A8) == 0 && IsValidPtr(epc)) {
        auto* pawn = *reinterpret_cast<uint8_t**>(epc + 0x4A0);
        if (IsValidPtr(pawn)) {
            vol_z = reinterpret_cast<float*>(vol + 0xF0);
            orig_z = *vol_z;
            const float pawn_z = *reinterpret_cast<const float*>(pawn + 0xF0);
            if (pawn_z > orig_z && pawn_z <= orig_z + 450.0f) {
                *vol_z = pawn_z + 64.0f;
                lifted = true;
            }
        }
    }

    g_orig_coop_dispatch_init_interact(self, edx, instigator, action_idx);
    if (lifted) {
        *vol_z = orig_z;
    }

    if (*reinterpret_cast<void**>(disp + 0x2CC) == nullptr) {
        *(disp + 0x2D0) &= static_cast<uint8_t>(~1u);
    }
}

enum class DualInterroState {
    Normal = 0,
    SoloStage = 1,
    WaitingForPartner = 2,
    DualReady = 3,
};

DualInterroState InspectDualInterroState(uint8_t* npc_pawn, uint8_t* epc, float* out_dist = nullptr) {
    if (!IsValidPtr(npc_pawn)) {
        return DualInterroState::Normal;
    }
    auto* sdisp = *reinterpret_cast<uint8_t**>(npc_pawn + 0xCF8);
    if (!IsValidPtr(sdisp)) {
        return DualInterroState::Normal;
    }
    const int32_t cnt = *reinterpret_cast<int32_t*>(sdisp + 0x2B8);
    auto* infos = *reinterpret_cast<uint8_t**>(sdisp + 0x2B4);
    if (cnt < 2 || !IsValidPtr(infos)) {
        return DualInterroState::Normal;
    }

    const uint8_t t0 = *(infos + 0x04);
    const uint32_t d0 = *reinterpret_cast<uint32_t*>(infos + 0x08);
    const uint8_t t1 = *(infos + 0x10 + 0x04);
    const uint32_t d1 = *reinterpret_cast<uint32_t*>(infos + 0x10 + 0x08);
    if ((t0 != 0 && t0 != 2) || (t1 != 0 && t1 != 2)) {
        return DualInterroState::Normal;
    }
    if ((d0 & 1u) == 0) {
        return DualInterroState::SoloStage;
    }
    if ((d1 & 1u) != 0) {
        return DualInterroState::Normal;
    }

    uint8_t* ref_epc = IsValidPtr(epc) ? epc : g_local_epc;
    auto* other_epc = reinterpret_cast<uint8_t*>(SafeGetOtherPlayer(ref_epc));
    auto* other_pawn = IsValidPtr(other_epc) ? *reinterpret_cast<uint8_t**>(other_epc + 0x4A0) : nullptr;
    if (!IsValidPtr(other_pawn)) {
        if (out_dist) {
            *out_dist = 9999.0f;
        }
        return DualInterroState::WaitingForPartner;
    }
    const float dist = Dist3D(reinterpret_cast<const float*>(npc_pawn + 0xE8),
                              reinterpret_cast<const float*>(other_pawn + 0xE8));
    if (out_dist) {
        *out_dist = dist;
    }
    return (dist <= kDualInterroPartnerRadius) ? DualInterroState::DualReady : DualInterroState::WaitingForPartner;
}

void __fastcall Hook_ENpcZoneGetDescription(void* self, void* edx, void* actions_arr, void* epc) {
    if (GetActiveScdaMap()[0] == L'\0' || !IsValidPtr(self)) {
        g_orig_npc_zone_get_desc(self, edx, actions_arr, epc);
        return;
    }

    auto* zone = reinterpret_cast<uint8_t*>(self);
    const uint8_t state = *(zone + 0x2D0);
    if (state != 2) {
        g_orig_npc_zone_get_desc(self, edx, actions_arr, epc);
        return;
    }

    auto* npc = *reinterpret_cast<uint8_t**>(zone + 0x2C8);
    auto* grabber = IsValidPtr(epc) ? reinterpret_cast<uint8_t*>(epc) : *reinterpret_cast<uint8_t**>(zone + 0x2CC);
    float dist = 9999.0f;
    const DualInterroState istate = InspectDualInterroState(npc, grabber, &dist);
    if (istate == DualInterroState::WaitingForPartner) {
        static DWORD last_wait_log = 0;
        const DWORD now = GetTickCount();
        if (now - last_wait_log > 1500) {
            last_wait_log = now;
            ScdaLogMsg("scda_dual_interro_wait_partner dist=%.1f", static_cast<double>(dist));
        }
        return;
    }

    if (istate == DualInterroState::DualReady) {
        InterlockedExchange(&g_interro_stage, 1);
        InterlockedExchange(&g_dual_interro_prompt_active, 1);
        g_orig_npc_zone_get_desc(self, edx, actions_arr, epc);
        InterlockedExchange(&g_dual_interro_prompt_active, 0);
        return;
    }

    g_orig_npc_zone_get_desc(self, edx, actions_arr, epc);
}

int __fastcall Hook_SpeechDispatcherTrigger(void* self, void* edx, void* npc, void* speech_type_ptr,
                                            void* out_pattern) {
    if (GetActiveScdaMap()[0] == L'\0' || !IsValidPtr(self)) {
        return g_orig_speech_trigger(self, edx, npc, speech_type_ptr, out_pattern);
    }

    auto* sdisp = reinterpret_cast<uint8_t*>(self);
    const int32_t cnt = *reinterpret_cast<int32_t*>(sdisp + 0x2B8);
    auto* infos = *reinterpret_cast<uint8_t**>(sdisp + 0x2B4);
    if (cnt >= 2 && IsValidPtr(infos)) {
        const uint32_t d0 = *reinterpret_cast<uint32_t*>(infos + 0x08);
        const uint32_t d1 = *reinterpret_cast<uint32_t*>(infos + 0x10 + 0x08);
        if ((d0 & 1u) != 0 && (d1 & 1u) == 0 && IsValidPtr(npc)) {
            float dist = 9999.0f;
            const DualInterroState istate = InspectDualInterroState(reinterpret_cast<uint8_t*>(npc), g_local_epc, &dist);
            if (istate == DualInterroState::WaitingForPartner) {
                ScdaLogMsg("scda_dual_interro_blocked dist=%.1f", static_cast<double>(dist));
                return 0;
            }
            const int r = g_orig_speech_trigger(self, edx, npc, speech_type_ptr, out_pattern);
            ScdaLogMsg("scda_dual_interro_dispatched ret=%d dist=%.1f", r, static_cast<double>(dist));
            return r;
        }
    }
    return g_orig_speech_trigger(self, edx, npc, speech_type_ptr, out_pattern);
}

void FindAutotestVolumes(void* glevel) {
    if (!IsValidPtr(glevel)) {
        return;
    }
    auto** actors = *reinterpret_cast<uint8_t***>(reinterpret_cast<uint8_t*>(glevel) + 0x28u);
    const int32_t num = *reinterpret_cast<const int32_t*>(reinterpret_cast<uint8_t*>(glevel) + 0x2Cu);
    if (!IsValidPtr(actors) || num <= 0 || num >= 16384) {
        return;
    }
    for (int32_t i = 0; i < num; ++i) {
        uint8_t* a = actors[i];
        if (!IsValidPtr(a)) {
            continue;
        }
        const auto* loc = reinterpret_cast<const float*>(a + 0xE8u);
        const float lim = *reinterpret_cast<const float*>(a + 0x3ACu);
        if (!IsValidPtr(g_known_b2b_vol4) && std::fabs(lim - kB2BLimitSentinel) < 0.05f &&
            std::fabs(loc[0] - 328.0f) < 20.0f && std::fabs(loc[1] - 448.0f) < 20.0f) {
            g_known_b2b_vol4 = a;
        }
        if (!IsValidPtr(g_known_vol8) && std::fabs(lim - 530.0f) < 0.5f &&
            std::fabs(loc[0] - 1348.0f) < 20.0f && std::fabs(loc[1] - 1224.0f) < 20.0f) {
            g_known_vol8 = a;
        }
        if (IsValidPtr(g_known_b2b_vol4) && IsValidPtr(g_known_vol8)) {
            break;
        }
    }
}

void RunAutotestStep() {
    if (g_autotest_mode == 0 || !IsValidPtr(g_local_epc)) {
        return;
    }
    auto* pawn = *reinterpret_cast<uint8_t**>(g_local_epc + 0x4A0);
    if (!IsValidPtr(pawn)) {
        return;
    }
    const DWORD now = GetTickCount();
    if (g_autotest_t0 == 0) {
        g_autotest_t0 = now;
        ScdaLogMsg("scda_autotest_start role=%d epc=0x%p pawn=0x%p", g_autotest_mode, g_local_epc, pawn);
    }
    const DWORD elapsed = now - g_autotest_t0;

    if (g_autotest_mode == 1) {
        // Host verification checkpoints:
        // Step 0 (t >= 3000ms): test HangoverVol8
        // Step 1 (t >= 4500ms): test RappelVol5
        // Step 2 (t >= 6000ms): test RappelVol6
        // Step 3 (t >= 7500ms): move to B2B Vol4 bottom & trigger P1 Init
        // Step 4 (t >= 11500ms): move near EAIGenericNPC0 (1100, 1185, 690) for guard pose shot
        if (g_autotest_step == 0 && elapsed >= 3000) {
            const float p[3] = {1348.0f, 1224.0f, 710.0f};
            SafeSetActorLocation(pawn, p, 1);
            auto* other_epc = reinterpret_cast<uint8_t*>(SafeGetOtherPlayer(g_local_epc));
            auto* other_pawn = IsValidPtr(other_epc) ? *reinterpret_cast<uint8_t**>(other_epc + 0x4A0) : nullptr;
            if (IsValidPtr(other_pawn)) {
                const float p2[3] = {1370.0f, 1224.0f, 710.0f};
                SafeSetActorLocation(other_pawn, p2, 1);
            }
            int avail_r = -1;
            if (IsValidPtr(g_known_vol8)) {
                auto* disp = *reinterpret_cast<uint8_t**>(g_local_epc + 0x978u);
                auto* other_disp = IsValidPtr(other_epc) ? *reinterpret_cast<uint8_t**>(other_epc + 0x978u) : nullptr;
                if (IsValidPtr(disp)) {
                    auto* prev = *reinterpret_cast<uint8_t**>(disp + 0x2C8u);
                    auto* prev_other = IsValidPtr(other_disp) ? *reinterpret_cast<uint8_t**>(other_disp + 0x2C8u) : nullptr;
                    *reinterpret_cast<uint8_t**>(disp + 0x2C8u) = g_known_vol8;
                    if (IsValidPtr(other_disp)) {
                        *reinterpret_cast<uint8_t**>(other_disp + 0x2C8u) = g_known_vol8;
                    }
                    avail_r = Hook_CoopDispatchIsAvailable(disp, nullptr);
                    *reinterpret_cast<uint8_t**>(disp + 0x2C8u) = prev;
                    if (IsValidPtr(other_disp)) {
                        *reinterpret_cast<uint8_t**>(other_disp + 0x2C8u) = prev_other;
                    }
                }
            }
            g_autotest_step = 1;
            ScdaLogMsg("scda_autotest_step=1_vol8 avail=%d other=0x%p", avail_r, other_epc);
        } else if (g_autotest_step == 1 && elapsed >= 4500) {
            const float p[3] = {996.0f, 1440.0f, 1085.0f};
            SafeSetActorLocation(pawn, p, 1);
            g_autotest_step = 2;
            ScdaLogMsg("scda_autotest_step=2_vol5");
        } else if (g_autotest_step == 2 && elapsed >= 6000) {
            const float p[3] = {-640.0f, -360.0f, 120.0f};
            SafeSetActorLocation(pawn, p, 1);
            g_autotest_step = 3;
            ScdaLogMsg("scda_autotest_step=3_vol6");
        } else if (g_autotest_step == 3 && elapsed >= 7500) {
            const float p[3] = {328.0f, 448.0f, -260.0f};
            SafeSetActorLocation(pawn, p, 1);
            g_autotest_step = 4;
            ScdaLogMsg("scda_autotest_step=3_b2b_pos");
        } else if (g_autotest_step == 4 && elapsed >= 8500 && (IsValidPtr(g_known_b2b_vol4) || elapsed >= 12000)) {
            if (IsValidPtr(g_known_b2b_vol4)) {
                HandleB2BInteract(g_known_b2b_vol4, g_local_epc);
            }
            g_autotest_step = 5;
            ScdaLogMsg("scda_autotest_step=4_b2b_init vol4=0x%p", g_known_b2b_vol4);
        } else if (g_autotest_step == 5 && elapsed >= 9500) {
            auto* other_epc = reinterpret_cast<uint8_t*>(SafeGetOtherPlayer(g_local_epc));
            if (IsValidPtr(other_epc) && IsValidPtr(g_known_b2b_vol4)) {
                auto* other_pawn = *reinterpret_cast<uint8_t**>(other_epc + 0x4A0);
                if (IsValidPtr(other_pawn)) {
                    const float p2[3] = {350.0f, 448.0f, -230.0f};
                    SafeSetActorLocation(other_pawn, p2, 1);
                }
                HandleB2BInteract(g_known_b2b_vol4, other_epc);
            }
            g_autotest_step = 6;
            ScdaLogMsg("scda_autotest_step=4b_b2b_p2 other=0x%p", other_epc);
        } else if (g_autotest_step == 6 && elapsed >= 18500) {
            const float p[3] = {1020.0f, 1185.0f, 690.0f};
            SafeSetActorLocation(pawn, p, 1);
            *reinterpret_cast<int32_t*>(g_local_epc + 0xF4) = 0;
            *reinterpret_cast<int32_t*>(g_local_epc + 0xF8) = 0;
            *reinterpret_cast<int32_t*>(g_local_epc + 0xFC) = 0;
            *reinterpret_cast<int32_t*>(pawn + 0xF4) = 0;
            *reinterpret_cast<int32_t*>(pawn + 0xF8) = 0;
            *reinterpret_cast<int32_t*>(pawn + 0xFC) = 0;
            g_autotest_step = 7;
            ScdaLogMsg("scda_autotest_step=5_guard_pose");
        }
    } else if (g_autotest_mode == 2) {
        // Guest mirrors vol8 at 2.5s, B2B bottom at 8.5s, and triggers Complete at t >= 9.5s
        if (g_autotest_step == 0 && elapsed >= 2500) {
            const float p[3] = {1370.0f, 1224.0f, 710.0f};
            SafeSetActorLocation(pawn, p, 1);
            g_autotest_step = 1;
            ScdaLogMsg("scda_autotest_guest_vol8_pos");
        } else if (g_autotest_step == 1 && elapsed >= 8500) {
            const float p[3] = {350.0f, 448.0f, -260.0f};
            SafeSetActorLocation(pawn, p, 1);
            g_autotest_step = 2;
            ScdaLogMsg("scda_autotest_guest_b2b_pos");
        } else if (g_autotest_step == 2 && elapsed >= 9500) {
            if (IsValidPtr(g_known_b2b_vol4)) {
                HandleB2BInteract(g_known_b2b_vol4, g_local_epc);
            }
            g_autotest_step = 3;
            ScdaLogMsg("scda_autotest_guest_b2b_complete vol4=0x%p", g_known_b2b_vol4);
        }
    }
}

void __fastcall Hook_GameEngineTick(void* self, void* edx, float dt) {
    g_orig_game_engine_tick(self, edx, dt);
    if (!IsValidPtr(self) || GetActiveScdaMap()[0] == L'\0') {
        return;
    }
    auto* eng = reinterpret_cast<uint8_t*>(self);
    void* glevel = *reinterpret_cast<void**>(eng + 0x17Cu);
    if (IsValidPtr(glevel) && !IsValidPtr(g_local_epc)) {
        auto* client = *reinterpret_cast<uint8_t**>(eng + 0x44u);
        if (IsValidPtr(client) && *reinterpret_cast<const int32_t*>(client + 0x30u) > 0) {
            auto** vps = *reinterpret_cast<uint8_t***>(client + 0x2Cu);
            if (IsValidPtr(vps) && IsValidPtr(vps[0])) {
                auto* epc = *reinterpret_cast<uint8_t**>(vps[0] + 0x30u);
                if (IsValidPtr(epc) && *reinterpret_cast<void**>(epc + 0x7Cu) == glevel) {
                    g_local_epc = epc;
                }
            }
        }
    }
    if (IsValidPtr(glevel) && g_autotest_mode != 0 && (!IsValidPtr(g_known_b2b_vol4) || !IsValidPtr(g_known_vol8))) {
        FindAutotestVolumes(glevel);
    }
    ScdaGameplayTick(dt, IsValidPtr(glevel));
}

uint8_t* ResolveRvaOrScan(uintptr_t exe_base, uintptr_t rva, const uint8_t* exp, size_t exp_len) {
    auto* primary = reinterpret_cast<uint8_t*>(exe_base + rva);
    if (std::memcmp(primary, exp, exp_len) == 0) {
        return primary;
    }
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(exe_base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) {
        return nullptr;
    }
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS32*>(exe_base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) {
        return nullptr;
    }
    const auto* sec = IMAGE_FIRST_SECTION(nt);
    uint8_t* unique_hit = nullptr;
    for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++sec) {
        if ((sec->Characteristics & IMAGE_SCN_MEM_EXECUTE) == 0 || sec->Misc.VirtualSize < exp_len) {
            continue;
        }
        auto* start = reinterpret_cast<uint8_t*>(exe_base + sec->VirtualAddress);
        const size_t limit = sec->Misc.VirtualSize - exp_len;
        for (size_t off = 0; off <= limit; ++off) {
            if (std::memcmp(start + off, exp, exp_len) == 0) {
                if (unique_hit != nullptr) {
                    return nullptr;
                }
                unique_hit = start + off;
            }
        }
    }
    return unique_hit;
}

bool InstallGameplayHook(const char* name, uintptr_t rva, const uint8_t* exp, size_t exp_len, void* detour,
                         void** orig) {
    uint8_t* target = ResolveRvaOrScan(g_exe_base, rva, exp, exp_len);
    if (!target) {
        auto* raw = reinterpret_cast<const uint8_t*>(g_exe_base + rva);
        ScdaLogMsg("scda_hook_%s_prologue_mismatch got=%02X_%02X_%02X_%02X", name,
                   raw[0], raw[1], raw[2], raw[3]);
        return false;
    }
    const MH_STATUS c = MH_CreateHook(target, detour, orig);
    const MH_STATUS e = (c == MH_OK) ? MH_EnableHook(target) : c;
    if (e != MH_OK) {
        ScdaLogMsg("scda_hook_%s_err (%s)", name, MH_StatusToString(e));
        return false;
    }
    return true;
}

}  // namespace

int InstallScdaGameplayHooks(uintptr_t exe_base) {
    g_exe_base = exe_base;
    if (exe_base == 0) {
        return 0;
    }

    char env_buf[32]{};
    if (GetEnvironmentVariableA("SCCT_SCDA_AUTOTEST", env_buf, sizeof(env_buf)) > 0) {
        if (_stricmp(env_buf, "host") == 0 || std::strcmp(env_buf, "1") == 0) {
            g_autotest_mode = 1;
        } else if (_stricmp(env_buf, "guest") == 0 || std::strcmp(env_buf, "2") == 0) {
            g_autotest_mode = 2;
        }
    }

    static const uint8_t exp_tick[8] = {0x56, 0x8B, 0xF1, 0x8B, 0x4E, 0x44, 0x85, 0xC9};
    static const uint8_t exp_avail[10] = {0x81, 0xEC, 0xC4, 0x01, 0x00, 0x00, 0x53, 0x55, 0x8B, 0xD9};
    static const uint8_t exp_init[10] = {0x53, 0x8B, 0x5C, 0x24, 0x08, 0x55, 0x56, 0x8B, 0xF1, 0x57};
    static const uint8_t exp_npc_desc[10] = {0xA0, 0x08, 0xF1, 0x22, 0x11, 0x83, 0xEC, 0x48, 0xA8, 0x01};
    static const uint8_t exp_speech[10] = {0x51, 0x55, 0x8B, 0xE9, 0x56, 0x8B, 0xB5, 0xB8, 0x02, 0x00};

    int ok = 0;
    if (InstallGameplayHook("GameEngineTick", kRvaGameEngineTick, exp_tick, sizeof(exp_tick),
                            reinterpret_cast<void*>(&Hook_GameEngineTick),
                            reinterpret_cast<void**>(&g_orig_game_engine_tick))) {
        ++ok;
    }
    if (InstallGameplayHook("CoopDispatchIsAvailable", kRvaCoopDispatchIsAvailable, exp_avail, sizeof(exp_avail),
                            reinterpret_cast<void*>(&Hook_CoopDispatchIsAvailable),
                            reinterpret_cast<void**>(&g_orig_coop_dispatch_is_available))) {
        ++ok;
    }
    if (InstallGameplayHook("CoopDispatchInitInteract", kRvaCoopDispatchInitInteract, exp_init, sizeof(exp_init),
                            reinterpret_cast<void*>(&Hook_CoopDispatchInitInteract),
                            reinterpret_cast<void**>(&g_orig_coop_dispatch_init_interact))) {
        ++ok;
    }
    if (InstallGameplayHook("ENpcZoneGetDescription", kRvaENpcZoneGetDescription, exp_npc_desc, sizeof(exp_npc_desc),
                            reinterpret_cast<void*>(&Hook_ENpcZoneGetDescription),
                            reinterpret_cast<void**>(&g_orig_npc_zone_get_desc))) {
        ++ok;
    }
    if (InstallGameplayHook("SpeechDispatcherTrigger", kRvaSpeechDispatcherTrigger, exp_speech, sizeof(exp_speech),
                            reinterpret_cast<void*>(&Hook_SpeechDispatcherTrigger),
                            reinterpret_cast<void**>(&g_orig_speech_trigger))) {
        ++ok;
    }
    return ok;
}

void OnScdaMapLoaded(const wchar_t* map_name) {
    InterlockedExchange(&g_interro_stage, 0);
    InterlockedExchange(&g_dual_interro_prompt_active, 0);
    ResetB2BState();
    g_local_epc = nullptr;
    g_local_disp = nullptr;
    g_known_b2b_vol4 = nullptr;
    g_known_vol8 = nullptr;
    g_autotest_step = 0;
    g_autotest_t0 = 0;

    if (!map_name || map_name[0] == L'\0') {
        return;
    }

    char map_u8[64]{};
    WideCharToMultiByte(CP_UTF8, 0, map_name, -1, map_u8, sizeof(map_u8) - 1, nullptr, nullptr);
    ScdaLogMsg("scda_gameplay_ready map=%s", map_u8);
}

void ScdaGameplayTick(float dt, bool in_level) {
    if (!in_level || GetActiveScdaMap()[0] == L'\0') {
        if (g_b2b.phase != 0) {
            ResetB2BState();
        }
        return;
    }

    RunAutotestStep();

    if (g_b2b.phase == 0 || !IsValidPtr(g_b2b.active_vol)) {
        return;
    }

    const DWORD now = GetTickCount();
    float step_dt = dt;
    if (step_dt <= 0.0f || step_dt > 0.1f) {
        step_dt = static_cast<float>(now - g_b2b.last_tick_ms) * 0.001f;
        if (step_dt <= 0.0f || step_dt > 0.1f) {
            step_dt = 0.016f;
        }
    }
    g_b2b.last_tick_ms = now;

    if (g_b2b.phase == 1) {
        // Hold P1 in place while waiting; detect P2 entering center or cancel if P1 moves out.
        if (IsValidPtr(g_b2b.p1_epc)) {
            auto* p1_pawn = *reinterpret_cast<uint8_t**>(g_b2b.p1_epc + 0x4A0);
            if (IsValidPtr(p1_pawn)) {
                const auto* cur = reinterpret_cast<const float*>(p1_pawn + 0xE8);
                if (now - g_b2b.wait_start_ms > 600 && Dist2D(cur, g_b2b.p1_wait_pos) > 55.0f) {
                    ResetB2BState();
                    ScdaLogMsg("scda_b2b_cancel_moved");
                    return;
                }
                SafeSetActorLocation(p1_pawn, g_b2b.p1_wait_pos, 1);
            }
        }
        if (IsValidPtr(g_b2b.p1_epc)) {
            auto* other_epc = reinterpret_cast<uint8_t*>(SafeGetOtherPlayer(g_b2b.p1_epc));
            auto* other_pawn = IsValidPtr(other_epc) ? *reinterpret_cast<uint8_t**>(other_epc + 0x4A0) : nullptr;
            if (IsValidPtr(other_pawn)) {
                const auto* oloc = reinterpret_cast<const float*>(other_pawn + 0xE8);
                const auto* vloc = reinterpret_cast<const float*>(g_b2b.active_vol + 0xE8);
                if (Dist2D(oloc, vloc) <= 40.0f && oloc[2] > g_b2b.start_z + 10.0f) {
                    BeginB2BClimb(g_b2b.active_vol, g_b2b.p1_epc, other_epc, g_b2b.start_z);
                    return;
                }
            }
        }
        return;
    }

    if (g_b2b.phase == 2) {
        if (!IsValidPtr(g_b2b.active_vol)) {
            ResetB2BState();
            return;
        }
        g_b2b.cur_z += 165.0f * step_dt;
        const auto* vol_loc = reinterpret_cast<const float*>(g_b2b.active_vol + 0xE8);
        const int32_t vol_yaw = *reinterpret_cast<const int32_t*>(g_b2b.active_vol + 0xF8);
        float fx = 1.0f, fy = 0.0f;
        ComputeVolAxis(g_b2b.active_vol, &fx, &fy);

        uint8_t* p1_pawn = IsValidPtr(g_b2b.p1_epc) ? *reinterpret_cast<uint8_t**>(g_b2b.p1_epc + 0x4A0) : nullptr;
        uint8_t* p2_pawn = IsValidPtr(g_b2b.p2_epc) ? *reinterpret_cast<uint8_t**>(g_b2b.p2_epc + 0x4A0) : nullptr;

        if (g_b2b.cur_z >= g_b2b.top_z) {
            PlaceAtShaftExit(p1_pawn, g_b2b.active_vol, g_b2b.top_z, 1.0f);
            PlaceAtShaftExit(p2_pawn, g_b2b.active_vol, g_b2b.top_z, -1.0f);
            ScdaLogMsg("scda_b2b_complete z=%.1f", static_cast<double>(g_b2b.top_z));
            ResetB2BState();
            return;
        }

        const bool start_loop = !g_b2b.loop_anim_started && (g_b2b.cur_z > g_b2b.start_z + 24.0f);
        if (start_loop) {
            g_b2b.loop_anim_started = true;
        }

        if (IsValidPtr(p1_pawn)) {
            ZeroPawnVelocity(p1_pawn);
            *reinterpret_cast<int32_t*>(p1_pawn + 0xF8) = vol_yaw + 32768;
            if (IsAuthorizedPawnForMove(p1_pawn)) {
                const float pos1[3] = {vol_loc[0] - fx * 26.0f, vol_loc[1] - fy * 26.0f, g_b2b.cur_z};
                SafeMoveActorLocation(p1_pawn, pos1);
            }
            if (start_loop) {
                TryPlayB2BAnim(p1_pawn, L"coopbktobk_climb_loop_p", 1);
            }
        }
        if (IsValidPtr(p2_pawn)) {
            ZeroPawnVelocity(p2_pawn);
            *reinterpret_cast<int32_t*>(p2_pawn + 0xF8) = vol_yaw;
            if (IsAuthorizedPawnForMove(p2_pawn)) {
                const float pos2[3] = {vol_loc[0] + fx * 26.0f, vol_loc[1] + fy * 26.0f, g_b2b.cur_z};
                SafeMoveActorLocation(p2_pawn, pos2);
            }
            if (start_loop) {
                TryPlayB2BAnim(p2_pawn, L"coopbktobk_climb_loop_p", 1);
            }
        }
    }
}

void ScdaNotifyLocalizedKey(const wchar_t* section, const wchar_t* key) {
    if (!section || !key || GetActiveScdaMap()[0] == L'\0') {
        return;
    }
    if (_wcsicmp(section, L"p_x01_coop_ice_a_interro") == 0 &&
        (_wcsicmp(key, L"Speech_0001L") == 0 || _wcsicmp(key, L"Speech_0002L") == 0 ||
         _wcsicmp(key, L"Speech_0003L") == 0)) {
        if (InterlockedCompareExchange(&g_interro_stage, 1, 0) == 0) {
            ScdaLogMsg("scda_interro_stage=1 (solo refusal -> Dual Interrogation armed)");
        }
    } else if (_wcsicmp(section, L"p_x01_coop_ice_a_teaminterro") == 0 &&
               (_wcsicmp(key, L"Speech_0001L") == 0 || _wcsicmp(key, L"Speech_0002L") == 0)) {
        if (InterlockedExchange(&g_interro_stage, 2) != 2) {
            ScdaLogMsg("scda_interro_stage=2 (Dual Interrogation triggered)");
        }
    }
}

bool ScdaOverrideInteractionLabel(const wchar_t* section, const wchar_t* key, wchar_t* out_buf, size_t out_cap) {
    if (!section || !key || !out_buf || out_cap == 0 || GetActiveScdaMap()[0] == L'\0') {
        return false;
    }
    if (_wcsicmp(section, L"Interaction") != 0) {
        return false;
    }

    if (InterlockedCompareExchange(&g_current_coop_is_b2b, 0, 0) != 0) {
        const LONG mode = InterlockedCompareExchange(&g_b2b_prompt_mode, 0, 0);
        if (_wcsicmp(key, L"InitRappelling") == 0 || _wcsicmp(key, L"CompleteRappelling") == 0) {
            if (mode == 3) {
                return CopyLabel(out_buf, out_cap, L"Cancel Back-to-Back");
            }
            if (mode == 2 || _wcsicmp(key, L"CompleteRappelling") == 0) {
                return CopyLabel(out_buf, out_cap, L"Complete Back-to-Back Climb");
            }
            return CopyLabel(out_buf, out_cap, L"Back-to-Back Climb");
        }
    }

    if (_wcsicmp(key, L"NpcZone2") == 0 &&
        InterlockedCompareExchange(&g_dual_interro_prompt_active, 0, 0) != 0) {
        return CopyLabel(out_buf, out_cap, L"Dual Interrogation");
    }

    if (_wcsicmp(key, L"PickupObject") == 0) {
        return CopyLabel(out_buf, out_cap, L"Pick Up Document");
    }

    return false;
}
