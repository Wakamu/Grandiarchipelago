#pragma once

namespace grandia_ap {

// Poll keyboard: ² toggles console-style debug flag (ASCII "4000" / "0000").
void PollDebugModeHotkey();

// True when the in-game debug flag is ON (Select = 9999 damage, etc.).
bool IsDebugModeEnabled();

}  // namespace grandia_ap
