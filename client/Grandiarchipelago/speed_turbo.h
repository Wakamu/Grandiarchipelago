#pragma once

namespace grandia_ap {

// 2x game speed (CE-style time API warp).
// Toggle: gamepad Select/Back (when debug mode is off). Hold: Right Ctrl.
// Video cinematics may ignore this — same limitation as CE speedhack.
bool InstallSpeedTurbo();
void RemoveSpeedTurbo();
bool IsSpeedTurboInstalled();

// Call from the watcher loop: updates toggle / hold-key state.
void PollSpeedTurboHotkey();

}  // namespace grandia_ap
