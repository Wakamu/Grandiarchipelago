#include "item_tracker.h"

#include "ap_session.h"
#include "game_memory.h"
#include "location_ids.h"
#include "location_labels.h"
#include "log.h"
#include "map_travel.h"
#include "progressions_generated.h"

#include <Windows.h>

#include <unordered_set>

namespace grandia_ap {

namespace {
ItemTracker g_tracker;
std::unordered_set<unsigned> g_sent_checks;
thread_local bool g_ap_delivering = false;
thread_local bool g_in_lockout_sweep = false;

void SweepLockoutChests(unsigned lockout_event_id);
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

    const unsigned location_id = LocationIdForChestEvent(event_id);
    if (WasCheckSent(location_id)) {
        if (!g_in_lockout_sweep) {
            LogInfo("Event 0x%04X already checked (location=0x%08X ctx=%s)", event_id, location_id,
                    context ? context : "");
        }
        return;
    }

    MarkCheckSent(location_id);
    LogInfo("Location check: event=0x%04X location=0x%08X ctx=%s", event_id, location_id,
            context ? context : "");
    GetApSession().EnqueueLocationCheck(location_id);

    if (!g_in_lockout_sweep) {
        SweepLockoutChests(event_id);
    }
}

void ItemTracker::OnItemAcquired(int item_slot_id, const char* context) {
    OnLocationChecked(item_slot_id, context);
}

void ItemTracker::EnqueueReceivedItem(unsigned ap_item_id, const char* item_name) {
    LogInfo("Queued AP item %s (0x%08X)", item_name ? item_name : "?", ap_item_id);

    if (TryHandleMapKeyItem(ap_item_id)) {
        return;
    }

    constexpr unsigned kItemBase = 0x47520000u;
    if (ap_item_id <= kItemBase) {
        return;
    }
    const int slot_id = static_cast<int>(ap_item_id - kItemBase);
    SetApDelivering(true);
    if (!AddStashQuantity(slot_id, 1)) {
        LogWarn("Could not deliver item slot=%d — stash base not resolved (load a save in-game)", slot_id);
    }
    SetApDelivering(false);
}

namespace {

void SweepLockoutChests(unsigned lockout_event_id) {
    for (std::size_t i = 0; i < progressions::kAreaLockoutCount; ++i) {
        const auto& lo = progressions::kAreaLockouts[i];
        if (lo.event_id != static_cast<uint16_t>(lockout_event_id)) {
            continue;
        }
        LogInfo("Area lockout 0x%04X — sweeping %u chest events", lockout_event_id,
                static_cast<unsigned>(lo.sweep_count));
        g_in_lockout_sweep = true;
        for (std::size_t s = 0; s < lo.sweep_count; ++s) {
            GetItemTracker().OnChestEventChecked(lo.sweep_events[s], "lockout-sweep");
        }
        g_in_lockout_sweep = false;
        return;
    }
}

}  // namespace

}  // namespace grandia_ap
