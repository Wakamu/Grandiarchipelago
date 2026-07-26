#pragma once

namespace grandia_ap {

// Variable game speed (CE-style time API warp + unlocked Present pacing).
// Select+R1: raise speed (2x → 3x → 4x → 5x). Select+L1: lower (min OFF).
// Hold Right Ctrl: temporary 2x while latched speed is OFF.
// Select alone still skips movies.
// Video cinematics may ignore this — same limitation as CE speedhack.
bool InstallSpeedTurbo();
void RemoveSpeedTurbo();
bool IsSpeedTurboInstalled();

// 0 = OFF, else 2..5. Used by Present hook to drop vsync and pace FPS.
int GetSpeedTurboLevel();

// Call from the watcher loop: updates toggle / hold-key state.
void PollSpeedTurboHotkey();

// After Present: sleep on real time so unlocked frames stay near 60 * level FPS.
void PaceSpeedTurboFrame();

}  // namespace grandia_ap
