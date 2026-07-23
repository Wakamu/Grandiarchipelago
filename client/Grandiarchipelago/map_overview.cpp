#include "map_overview.h"

#include "log.h"

namespace grandia_ap {

// Bird-view Toggle is deferred — F4 was reassigned to the debug overlay menus
// discovered via the Select-edge experiments. Keep stubs so call sites compile.

bool InstallMapOverviewHook() {
    LogInfo("Map overview: deferred (F4 is debug overlay — see debug_mode)");
    return true;
}

void RemoveMapOverviewHook() {}

void PollMapOverviewHotkey() {}

bool IsMapOverviewForced() { return false; }

}  // namespace grandia_ap
