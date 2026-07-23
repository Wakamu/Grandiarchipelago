#include "hooks.h"

#include "chest_pickup.h"
#include "debug_mode.h"
#include "d3d_overlay.h"
#include "game_memory.h"
#include "item_tracker.h"
#include "log.h"
#include "map_overview.h"
#include "map_travel.h"
#include "m_dat_balance.h"
#include "movie_skip.h"
#include "party_custom.h"
#include "save_sync.h"
#include "speed_turbo.h"
#include "windt_balance.h"
#include "shop_balance.h"
#include "xp_multiplier.h"

namespace grandia_ap {

bool InstallHooks() {
    if (!InstallSpeedTurbo()) {
        LogWarn("Speed turbo not installed — Select+L1 / RCtrl 2x unavailable");
    } else {
        LogInfo("Speed turbo active — Select+L1 toggles / RCtrl holds 2x");
    }
    if (!InstallMovieSkipHook()) {
        LogWarn("Movie skip not installed — cinematics stay unskippable");
    } else {
        LogInfo("Movie skip active — Select/Backspace skips MP4 cinematics");
    }
    if (!InstallShopBalanceHooks()) {
        LogWarn("fopen overlay not installed — Redux content remap unavailable");
    }
    if (!InstallPartyCustomHook()) {
        LogWarn("Party custom not installed");
    }
    if (!InstallDebugOverlayHook()) {
        LogWarn("Debug overlay not installed");
    }
    if (!InstallMapOverviewHook()) {
        LogWarn("Map overview not installed — F4 bird view unavailable");
    }
    if (IsChestPickupHookInstalled()) {
        LogInfo("Chest event hook active (flag write AOB / +0x70505, loot +0x53C45, story +0x6F03F/+0x7CB4F)");
        if (IsEventFlagTraceEnabled()) {
            LogInfo(
                "Event flag trace ON — PROGRESS? mode (mask!=0, hides map-travel noise +0x61487 etc.)");
        }
    } else {
        LogWarn("Chest event hook not installed — location checks disabled");
    }
    if (IsAssignUiHookInstalled()) {
        LogInfo("Assign UI hook active (+0x1DC100, suppress vanilla chest loot on AP location checks)");
    } else {
        LogWarn("Assign UI hook not installed — vanilla chest loot may enter party inventory");
    }
    if (IsFieldGoldAddHookInstalled()) {
        LogInfo("Field gold suppress active (+0x7612E add [eax+4],edx — AP gold chests add 0)");
    } else {
        LogWarn("Field gold suppress not installed — vanilla gold chests may still grant gold");
    }
    if (IsXpMultiplierHookInstalled()) {
        LogInfo("XP multipliers active (magic +0xA4E96, skill +0xA4FA7, level-fight +0x1387C3)");
    } else {
        LogWarn("XP multiplier hooks not installed — magic/skill/level XP stay vanilla");
    }
    if (!IsMdatBalanceHookInstalled()) {
        LogWarn("M_DAT balance not ready");
    }
    if (!InstallWindtSec3Hooks()) {
        LogWarn("WINDT sec3 finalize hooks not installed — shop may bake vanilla prices");
    }
    if (IsSaveSyncHookInstalled()) {
        LogInfo("Save sync hooks active (confirm-UI gate + fopen backstop / GAP1 trailer)");
    } else {
        LogWarn("Save sync hooks not installed — AP trailer will not persist on save");
    }
    if (IsMapTravelHookInstalled()) {
        LogInfo("World-map gate active (+0x58491 confirm FSM; Key base 0x47523000)");
    } else {
        LogWarn("World-map gate not installed — world-map keys inactive");
    }
    if (IsD3dOverlayInstalled()) {
        LogInfo("D3D11 Present overlay active (GDI toast top-left)");
    }
    return true;
}

void RemoveHooks() {
    RemoveShopBalanceHooks();
    RemoveWindtSec3Hooks();
    RemoveMdatBalanceHook();
    RemoveMapOverviewHook();
    RemoveDebugOverlayHook();
    RemovePartyCustomHook();
    RemoveMovieSkipHook();
    RemoveSpeedTurbo();
    RemoveXpMultiplierHooks();
    RemoveMapTravelHook();
    RemoveSaveSyncHooks();
}

void NotifyItemAcquired(int item_slot_id, const char* context) {
    GetItemTracker().OnLocationChecked(item_slot_id, context);
}

}  // namespace grandia_ap
