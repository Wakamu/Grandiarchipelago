#pragma once

namespace grandia_ap {

bool InstallMenuSavePartyTabHook();
void RemoveMenuSavePartyTabHook();
bool IsMenuSavePartyTabHookInstalled();

// Clears Party panel if Save menu closed / strip left Party (call from watcher tick).
void PollMenuSavePartyTab();

}  // namespace grandia_ap
