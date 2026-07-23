#include "item_tracker.h"

#include "ap_session.h"
#include "chest_pickup.h"
#include "game_memory.h"
#include "location_ids.h"
#include "location_labels.h"
#include "log.h"
#include "map_travel.h"
#include "pipe_bridge.h"
#include "progressions_generated.h"

#include <Windows.h>

#include <cstdio>
#include <string>
#include <unordered_set>

namespace grandia_ap {

namespace {
ItemTracker g_tracker;
std::unordered_set<unsigned> g_sent_checks;
thread_local bool g_ap_delivering = false;

void RequestLockoutProgressionSweep(unsigned lockout_event_id);
}  // namespace

void SetApDelivering(bool delivering) { g_ap_delivering = delivering; }

bool IsApDelivering() { return g_ap_delivering; }

ItemTracker& GetItemTracker() { return g_tracker; }

bool ItemTracker::WasCheckSent(unsigned location_id) const {
    return g_sent_checks.find(location_id) != g_sent_checks.end();
}

void ItemTracker::MarkCheckSent(unsigned location_id) {
    g_sent_checks.insert(location_id);
}

void ItemTracker::OnLocationChecked(int item_slot_id, const char* context) {
    unsigned location_id = 0;
    if (!LocationIdForItemSlot(item_slot_id, &location_id)) {
        LogDebug("Ignored location check slot=%d ctx=%s", item_slot_id, context ? context : "");
        return;
    }

    if (WasCheckSent(location_id)) {
        return;
    }

    MarkCheckSent(location_id);
    const char* label = LocationLabelForSlot(item_slot_id);
    if (label) {
        LogInfo("Location check: slot=%d location=0x%08X ctx=%s — %s", item_slot_id, location_id,
                context ? context : "", label);
    } else {
        LogInfo("Location check: slot=%d location=0x%08X ctx=%s", item_slot_id, location_id,
                context ? context : "");
    }
    GetApSession().EnqueueLocationCheck(location_id);
}

void ItemTracker::OnChestEventChecked(unsigned event_id, const char* context) {
    if (event_id == 0 || event_id > 0xFFFFu) {
        return;
    }
    if (!progressions::IsApCheckEvent(static_cast<uint16_t>(event_id))) {
        return;
    }

    const unsigned location_id = LocationIdForChestEvent(event_id);
    if (WasCheckSent(location_id)) {
        return;
    }

    MarkCheckSent(location_id);
    LogInfo("Location check: event=0x%04X location=0x%08X ctx=%s", event_id, location_id,
            context ? context : "");
    GetApSession().EnqueueLocationCheck(location_id);

    // Companion lockout location (apworld event item) — seals blocked maps for UT.
    for (std::size_t i = 0; i < progressions::kAreaLockoutCount; ++i) {
        if (progressions::kAreaLockouts[i].event_id != static_cast<uint16_t>(event_id)) {
            continue;
        }
        const unsigned lockout_loc = LocationIdForAreaLockout(event_id);
        if (!WasCheckSent(lockout_loc)) {
            MarkCheckSent(lockout_loc);
            LogInfo("Area lockout check: event=0x%04X location=0x%08X", event_id, lockout_loc);
            GetApSession().EnqueueLocationCheck(lockout_loc);
        }
        break;
    }

    RequestLockoutProgressionSweep(event_id);
}

void ItemTracker::OnItemAcquired(int item_slot_id, const char* context) {
    OnLocationChecked(item_slot_id, context);
}

void ItemTracker::EnqueueReceivedItem(unsigned ap_item_id, const char* item_name) {
    LogInfo("Queued AP item %s (0x%08X)", item_name ? item_name : "?", ap_item_id);

    if (TryHandleMapKeyItem(ap_item_id)) {
        return;
    }

    // Area-lockout tokens are logic-only (0x47540000 + event_id); ignore for inventory.
    constexpr unsigned kLockoutItemBase = 0x47540000u;
    if (ap_item_id >= kLockoutItemBase && ap_item_id < kLockoutItemBase + 0x10000u) {
        LogInfo("Received area lockout token 0x%08X (logic only)", ap_item_id);
        return;
    }

    constexpr unsigned kItemBase = 0x47520000u;
    // Must match tools/sync_apworld_from_mdp_catalog.py GOLD_AP_ID_BASE.
    constexpr unsigned kGoldHelperIdBase = 0x1000u;
    if (ap_item_id <= kItemBase) {
        return;
    }
    const unsigned slot_id = ap_item_id - kItemBase;
    if (slot_id >= kGoldHelperIdBase) {
        const unsigned amount = slot_id - kGoldHelperIdBase;
        if (!AddGoldAmount(amount)) {
            LogWarn("Could not deliver gold +%u", amount);
        }
        return;
    }

    SetApDelivering(true);
    if (!AddStashQuantity(static_cast<int>(slot_id), 1)) {
        LogWarn("Could not deliver item slot=%u — stash base not resolved (load a save in-game)", slot_id);
    }
    SetApDelivering(false);
}

namespace {

void RequestLockoutProgressionSweep(unsigned lockout_event_id) {
    for (std::size_t i = 0; i < progressions::kAreaLockoutCount; ++i) {
        const auto& lo = progressions::kAreaLockouts[i];
        if (lo.event_id != static_cast<uint16_t>(lockout_event_id)) {
            continue;
        }
        // Client decides which of these hold progression items in this seed.
        std::string line;
        line.reserve(16 + lo.sweep_count * 9);
        char head[32];
        snprintf(head, sizeof(head), "LOCKOUT %04X", lockout_event_id);
        line = head;
        unsigned pending = 0;
        for (std::size_t s = 0; s < lo.sweep_count; ++s) {
            const uint16_t sweep_eid = lo.sweep_events[s];
            if (!IncludeGoldChests() && progressions::IsGoldChestEvent(sweep_eid)) {
                continue;
            }
            if (IsExcludedOptionalDungeonEvent(sweep_eid)) {
                continue;
            }
            const unsigned location_id = LocationIdForChestEvent(sweep_eid);
            if (g_sent_checks.find(location_id) != g_sent_checks.end()) {
                continue;
            }
            char tok[16];
            snprintf(tok, sizeof(tok), " %08X", location_id);
            line += tok;
            ++pending;
        }
        LogInfo("Area lockout 0x%04X — requesting progression sweep (%u unchecked chests)",
                lockout_event_id, pending);
        PipeEnqueueLockoutMessage(line);
        return;
    }
}

}  // namespace

}  // namespace grandia_ap
