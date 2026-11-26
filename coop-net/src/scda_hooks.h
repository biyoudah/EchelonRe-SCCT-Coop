#pragma once

#include <cstddef>
#include <cstdint>

using ScdaLogFn = void (*)(const char* msg);

// Install runtime compatibility hooks for zero-retail-mod SCDA ported maps
// (loose MapRotation.ini/.int lookup, scda_*_pat redirect, SCDA_* audio bank
// redirect, null-anim/null-hash safety guards, and SCDA gameplay hooks).
// Safe no-op when SCDA files are not present on disk.
void InstallScdaHooks(uintptr_t exe_base, ScdaLogFn log_fn);

// Returns the currently loaded SCDA map name (e.g. L"scda_ice_a") or L"" when
// running stock Chaos Theory maps.
const wchar_t* GetActiveScdaMap();

// Shared helpers for scda_gameplay.cpp
void ScdaLogMsg(const char* fmt, ...);
void* ScdaStaticLoadObject(void* cls, void* outer, const wchar_t* name, const wchar_t* filename, uint32_t flags,
                           void* sandbox);
