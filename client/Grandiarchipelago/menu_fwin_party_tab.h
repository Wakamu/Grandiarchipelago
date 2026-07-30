#pragma once

namespace grandia_ap {

// FWIN pause menu (type 0 / +0x1C3EB0): widen Items..Status tab count 5→6.
// Phase 1: count pushes only — L1/R1 already clamps on [0x701160].
bool InstallMenuFwinPartyTabHook();
void RemoveMenuFwinPartyTabHook();
bool IsMenuFwinPartyTabHookInstalled();

}  // namespace grandia_ap
