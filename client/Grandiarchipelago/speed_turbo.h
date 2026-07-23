#pragma once

namespace grandia_ap {

// 2x game speed (CE-style time API warp).
// Toggle: Select+L1. Hold: Right Ctrl. (Select alone still skips movies.)
// Video cinematics may ignore this — same limitation as CE speedhack.
bool InstallSpeedTurbo();
void RemoveSpeedTurbo();
bool IsSpeedTurboInstalled();

// Call from the watcher loop: updates toggle / hold-key state.
void PollSpeedTurboHotkey();

}  // namespace grandia_ap
