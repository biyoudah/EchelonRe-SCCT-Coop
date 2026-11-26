#pragma once

#include <cstddef>
#include <cstdint>

// Installs SCDA-specific runtime gameplay hooks (safe no-op on stock CT maps).
int InstallScdaGameplayHooks(uintptr_t exe_base);

// Called when an SCDA map (e.g. L"scda_ice_a") becomes active.
void OnScdaMapLoaded(const wchar_t* map_name);

// Called from ConfigGetStringBuf to allow SCDA-specific interaction prompt
// overrides (Back-to-Back Climb, Dual Interrogation, Pick Up Document, etc.).
bool ScdaOverrideInteractionLabel(const wchar_t* section, const wchar_t* key, wchar_t* out_buf, size_t out_cap);

// Called when a section/key is localized so gameplay state (e.g. interrogation stage)
// can track pattern progression without touching retail Echelon*.u.
void ScdaNotifyLocalizedKey(const wchar_t* section, const wchar_t* key);

// Called every frame on the main game thread from UGameEngine::Tick.
void ScdaGameplayTick(float dt, bool in_level);
