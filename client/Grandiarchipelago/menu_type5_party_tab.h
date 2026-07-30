#pragma once

namespace grandia_ap {

// Experimental Options-menu (type 5 / +0x62350) tab widen — currently DISABLED.
// Real Items/Equip/Magic/Moves/Status pause menu is FWIN (menu type 0 / +0x1C3EB0).
// See FWIN 5→6 checklist: page at [0x701162], L1/R1 at +0x1CD3E0, count at [0x701160].
bool InstallMenuType5PartyTabHook();
void RemoveMenuType5PartyTabHook();
bool IsMenuType5PartyTabHookInstalled();

}  // namespace grandia_ap
