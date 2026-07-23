#pragma once

namespace grandia_ap {

// ² : toggle ASCII debug flag ("4000"/"0000") — 9999 damage etc.
// F4–F7 debug overlay menus: parked (see kDebugOverlayEnabled in debug_mode.cpp).
// F8 / Select+R1: toggle random encounters off/on (same as Map debug "ENCOUNT OFF").
void PollDebugModeHotkey();

bool IsDebugModeEnabled();
bool IsDebugOverlayActive();

bool InstallDebugOverlayHook();
void RemoveDebugOverlayHook();

// Called from pad-refresh detour while overlay is active.
void ApplyDebugPadAssist();

}  // namespace grandia_ap
