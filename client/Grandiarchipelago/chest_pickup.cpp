#include "chest_pickup.h"

#include "game_memory.h"
#include "item_tracker.h"
#include "location_ids.h"
#include "log.h"
#include "progressions_generated.h"

#include <Windows.h>

#include <cstdint>
#include <vector>

namespace grandia_ap {

namespace {

// Verbose RE logging of every flag-hook hit. Keep false for normal play.
constexpr bool kLogAllEventFlagWrites = false;

// Map-travel / init callers discovered via RE (Jul 2026) — suppressed in filtered trace.
constexpr std::uintptr_t kNoiseFlagCallerRvas[] = {
    0x61487u,  // map load bulk flag sync (0x1D80-0x1DFF sweep)
    0x5AE35u,  // map entry init (0x080F, 0x0810)
    0x51474u,  // area transition
    0x514A0u,
    0x54244u,
    0x59293u,
    0x6156Du,
    0x5922Fu,
    0x55F4Cu,
};

constexpr std::uintptr_t kChestLootCallerRva = 0x53C45u;
constexpr std::uintptr_t kStoryScriptCallerRva = 0x6F03Fu;      // dialogue / cutscene script (RE Jul 2026)
constexpr std::uintptr_t kStoryScriptAltCallerRva = 0x7CB4Fu;  // scene kickoff before dialogue loop
constexpr std::uintptr_t kCallerSlack = 0x10u;

// Assign UI handler (+1DC100) is entered via call [grandia+0x2C302C] at +61E07; returns to +61E0D.
constexpr std::uintptr_t kAssignUiEntryRva = 0x1DC100u;
constexpr std::uintptr_t kAssignUiReturnRva = 0x61E0Du;

constexpr unsigned kBulkMapInitEventMin = 0x1D80u;
constexpr unsigned kBulkMapInitEventMax = 0x1DFFu;

// Legacy RE heuristic: early story progress flags clustered under 0x00FF.
// AP story checks are no longer limited to this range — see IsStoryEventEligible.
constexpr unsigned kStoryProgressEventMax = 0x00FFu;

constexpr unsigned kStorySceneNoiseEventIds[] = {
    0x080Eu,
    0x080Fu,
    0x08F3u,
    0x08F4u,
    0x08F9u,
};

// RAM flag_off 0x142 -> file save offset 0x9D2 (observed on Herbs chests).
constexpr unsigned kRamToFileFlagOffset = 0x890u;

struct ChestEventWork {
    unsigned event_id;
    unsigned flag_offset;
    unsigned flag_value;
    unsigned mask;
    unsigned ecx_index;
    std::uintptr_t save_base;
    std::uintptr_t caller;
};

CRITICAL_SECTION g_chest_queue_lock{};
bool g_chest_queue_ready = false;
std::vector<ChestEventWork> g_chest_queue;
// Set by chest flag hook (+53C45 loot caller); consumed once at +1DC100 assign UI entry.
volatile unsigned g_pending_chest_assign_event_id = 0;
// Set with pending for AP field chests; consumed by +0x7612E gold-add suppress (gold chests)
// or cleared when assign UI intercepts (item chests).
volatile unsigned g_suppress_field_gold_event_id = 0;

const char* KnownChestLabel(unsigned event_id) {
    switch (event_id) {
        case 0x0A11:
            return "RE: Herbs chest A";
        case 0x0A16:
            return "RE: Herbs chest B";
        default:
            return nullptr;
    }
}

void EnsureChestQueueReady() {
    if (!g_chest_queue_ready) {
        InitializeCriticalSection(&g_chest_queue_lock);
        g_chest_queue_ready = true;
    }
}

bool IsNearRva(std::uintptr_t rva, std::uintptr_t target) {
    return rva + kCallerSlack >= target && rva <= target + kCallerSlack;
}

bool IsBulkMapInitEventId(unsigned event_id) {
    return event_id >= kBulkMapInitEventMin && event_id <= kBulkMapInitEventMax;
}

bool IsLootCallerRva(std::uintptr_t rva) {
    return IsNearRva(rva, kChestLootCallerRva);
}

bool IsStoryScriptCallerRva(std::uintptr_t rva) {
    return IsNearRva(rva, kStoryScriptCallerRva) || IsNearRva(rva, kStoryScriptAltCallerRva);
}

unsigned ConsumePendingChestAssignEvent() {
    const unsigned event_id = g_pending_chest_assign_event_id;
    g_pending_chest_assign_event_id = 0;
    return event_id;
}

// Returns 1 if event_id maps to a known chest we suppress vanilla assign UI for.
// Does not deliver vanilla loot — AP server sends received items separately.
int VanillaItemIdForChestEvent(unsigned event_id) {
    switch (event_id) {
        case 0x0A11:
        case 0x0A16:
            return 346;  // Herbs (RE: DX/BX = 0x15A at +1E0BFD)
        default:
            return 0;
    }
}

bool IsLootFlagCaller(std::uintptr_t caller) {
    const std::uintptr_t base = GetGrandiaModuleBase();
    if (base == 0 || caller < base) {
        return false;
    }
    const std::uintptr_t rva = caller - base;
    if (rva > 0x2000000u) {
        return false;
    }
    return IsLootCallerRva(rva);
}

bool IsStoryFlagCaller(std::uintptr_t caller) {
    const std::uintptr_t base = GetGrandiaModuleBase();
    if (base == 0 || caller < base) {
        return false;
    }
    const std::uintptr_t rva = caller - base;
    if (rva > 0x2000000u) {
        return false;
    }
    return IsStoryScriptCallerRva(rva);
}

bool IsStoryProgressEventId(unsigned event_id) {
    return event_id > 0 && event_id <= kStoryProgressEventMax;
}

bool IsStorySceneNoiseEventId(unsigned event_id) {
    for (const unsigned noise_id : kStorySceneNoiseEventIds) {
        if (event_id == noise_id) {
            return true;
        }
    }
    return false;
}

bool IsStoryEventEligible(unsigned event_id, unsigned mask) {
    if ((mask & 0xFFu) == 0) {
        return false;
    }
    if (IsBulkMapInitEventId(event_id)) {
        return false;
    }
    // progressions.json story ids routinely exceed 0x00FF — allowlist is source of truth.
    return progressions::IsApCheckEvent(static_cast<uint16_t>(event_id));
}

std::uintptr_t CallerRva(std::uintptr_t caller) {
    const std::uintptr_t base = GetGrandiaModuleBase();
    if (base == 0 || caller < base) {
        return 0;
    }
    return caller - base;
}

unsigned FileFlagOffset(unsigned ram_flag_offset) {
    return ram_flag_offset + kRamToFileFlagOffset;
}


bool IsNoiseFlagCallerRva(std::uintptr_t caller_rva) {
    for (const std::uintptr_t noise : kNoiseFlagCallerRvas) {
        if (IsNearRva(caller_rva, noise)) {
            return true;
        }
    }
    return false;
}

const char* ClassifyFlagCallerRva(std::uintptr_t caller_rva) {
    if (caller_rva == 0) {
        return "unknown";
    }
    if (IsLootCallerRva(caller_rva)) {
        return "chest-loot";
    }
    if (IsNearRva(caller_rva, kStoryScriptCallerRva)) {
        return "story-script";
    }
    if (IsNearRva(caller_rva, kStoryScriptAltCallerRva)) {
        return "story-script-alt";
    }
    if (IsNoiseFlagCallerRva(caller_rva)) {
        return "map-noise";
    }
    return "unclassified";
}

bool ShouldLogEventFlagTrace(unsigned event_id, unsigned mask, std::uintptr_t caller_rva) {
    if (IsStorySceneNoiseEventId(event_id)) {
        return false;
    }
    if (IsLootCallerRva(caller_rva)) {
        return true;
    }
    if (IsStoryScriptCallerRva(caller_rva)) {
        return (mask & 0xFFu) != 0;
    }
    if (IsNoiseFlagCallerRva(caller_rva)) {
        return false;
    }
    if (IsBulkMapInitEventId(event_id)) {
        return false;
    }
    if ((mask & 0xFFu) == 0) {
        return false;
    }
    return true;
}

void LogEventFlagWrite(unsigned event_id, unsigned flag_offset, unsigned flag_value, unsigned mask,
                       unsigned ecx_index, std::uintptr_t save_base, std::uintptr_t caller_rva) {
    if (!ShouldLogEventFlagTrace(event_id, mask, caller_rva)) {
        return;
    }

    const unsigned file_off = FileFlagOffset(flag_offset);
    const char* caller_class = ClassifyFlagCallerRva(caller_rva);
    const char* known = KnownChestLabel(event_id);
    const bool is_story_scene =
        IsStoryScriptCallerRva(caller_rva) && !IsStoryProgressEventId(event_id) && (mask & 0xFFu) != 0;
    const bool is_progress =
        !IsLootCallerRva(caller_rva) && !IsStoryScriptCallerRva(caller_rva) && (mask & 0xFFu) != 0;
    const char* headline = IsLootCallerRva(caller_rva) ? "Event flag trace"
                              : is_story_scene           ? "Event flag story-scene"
                              : IsStoryScriptCallerRva(caller_rva) ? "Event flag story"
                              : is_progress                      ? "Event flag PROGRESS?"
                                                               : "Event flag trace";

    if (known) {
        LogInfo(
            "%s: event=0x%04X (%u) %s | flag_ram=0x%X file~0x%X value=0x%02X mask=0x%02X ecx=%u "
            "save_base=0x%08X caller=+0x%X (%s) -> location=0x%08X",
            headline, event_id, event_id, known, flag_offset, file_off, flag_value & 0xFFu, mask & 0xFFu,
            ecx_index, static_cast<unsigned>(save_base), static_cast<unsigned>(caller_rva), caller_class,
            LocationIdForChestEvent(event_id));
    } else {
        LogInfo(
            "%s: event=0x%04X (%u) | flag_ram=0x%X file~0x%X value=0x%02X mask=0x%02X ecx=%u "
            "save_base=0x%08X caller=+0x%X (%s) -> location=0x%08X",
            headline, event_id, event_id, flag_offset, file_off, flag_value & 0xFFu, mask & 0xFFu, ecx_index,
            static_cast<unsigned>(save_base), static_cast<unsigned>(caller_rva), caller_class,
            LocationIdForChestEvent(event_id));
    }
}

}  // namespace

bool IsChestPickupHookInstalled() { return IsChestFlagHookInstalled(); }

bool IsAssignUiHookInstalled() { return IsAssignUiEntryHookInstalled(); }

bool IsEventFlagTraceEnabled() { return kLogAllEventFlagWrites; }

void QueueChestEventPickup(unsigned event_id, unsigned flag_offset, unsigned flag_value, unsigned mask,
                           unsigned ecx_index, std::uintptr_t save_base, std::uintptr_t caller) {
    if (event_id == 0) {
        return;
    }

    const std::uintptr_t caller_rva = CallerRva(caller);
    const unsigned file_off = FileFlagOffset(flag_offset);
    const char* known = KnownChestLabel(event_id);
    const bool is_chest = IsLootFlagCaller(caller);
    const bool is_story = IsStoryFlagCaller(caller) && IsStoryEventEligible(event_id, mask);

    if (kLogAllEventFlagWrites) {
        LogEventFlagWrite(event_id, flag_offset, flag_value, mask, ecx_index, save_base, caller_rva);
    }

    if (!is_chest && !is_story) {
        return;
    }

    // Only locations defined by progressions + MDP catalog (logic-scoped) become AP checks.
    if (!progressions::IsApCheckEvent(static_cast<uint16_t>(event_id))) {
        return;
    }

    if (is_chest) {
        if (!kLogAllEventFlagWrites) {
            if (known) {
                LogInfo(
                    "Chest opened: event=0x%04X (%u) %s | flag_ram=0x%X file~0x%X value=0x%02X mask=0x%02X ecx=%u "
                    "save_base=0x%08X caller=+0x%X -> location=0x%08X",
                    event_id, event_id, known, flag_offset, file_off, flag_value & 0xFFu, mask & 0xFFu, ecx_index,
                    static_cast<unsigned>(save_base), static_cast<unsigned>(caller_rva),
                    LocationIdForChestEvent(event_id));
            } else {
                LogInfo(
                    "Chest opened: event=0x%04X (%u) | flag_ram=0x%X file~0x%X value=0x%02X mask=0x%02X ecx=%u "
                    "save_base=0x%08X caller=+0x%X -> location=0x%08X",
                    event_id, event_id, flag_offset, file_off, flag_value & 0xFFu, mask & 0xFFu, ecx_index,
                    static_cast<unsigned>(save_base), static_cast<unsigned>(caller_rva),
                    LocationIdForChestEvent(event_id));
            }
        } else {
            LogInfo("Chest AP check queued: event=0x%04X caller=+0x%X (chest-loot)", event_id,
                    static_cast<unsigned>(caller_rva));
        }

        g_pending_chest_assign_event_id = event_id;
        g_suppress_field_gold_event_id = event_id;
    } else {
        LogInfo("Story AP check queued: event=0x%04X caller=+0x%X (%s) -> location=0x%08X", event_id,
                static_cast<unsigned>(caller_rva), ClassifyFlagCallerRva(caller_rva),
                LocationIdForChestEvent(event_id));
    }

    EnsureChestQueueReady();
    EnterCriticalSection(&g_chest_queue_lock);
    g_chest_queue.push_back(ChestEventWork{event_id, flag_offset, flag_value, mask, ecx_index, save_base, caller});
    LeaveCriticalSection(&g_chest_queue_lock);
}

int TryInterceptChestAssignUi(std::uintptr_t return_addr, std::uintptr_t /*stack_pointer*/) {
    if (IsApDelivering()) {
        return 0;
    }

    const std::uintptr_t base = GetGrandiaModuleBase();
    if (base == 0 || return_addr < base) {
        return 0;
    }

    const std::uintptr_t return_rva = return_addr - base;
    if (!IsNearRva(return_rva, kAssignUiReturnRva)) {
        return 0;
    }

    const unsigned event_id = ConsumePendingChestAssignEvent();
    if (event_id == 0) {
        LogDebug("Assign UI +0x%X return=+0x61E0D — no pending chest event (not field chest loot)",
                 static_cast<unsigned>(kAssignUiEntryRva));
        return 0;
    }

    g_suppress_field_gold_event_id = 0;
    return 1;
}

int TrySuppressVanillaFieldGold() {
    const unsigned event_id = g_suppress_field_gold_event_id;
    if (event_id == 0) {
        return 0;
    }
    g_suppress_field_gold_event_id = 0;
    // Also clear assign pending — gold chests never hit assign UI.
    if (g_pending_chest_assign_event_id == event_id) {
        g_pending_chest_assign_event_id = 0;
    }
    return 1;
}

void ProcessChestPickupQueue() {
    EnsureChestQueueReady();

    std::vector<ChestEventWork> pending;
    EnterCriticalSection(&g_chest_queue_lock);
    pending.swap(g_chest_queue);
    LeaveCriticalSection(&g_chest_queue_lock);

    for (const ChestEventWork& work : pending) {
        if (IsApDelivering()) {
            continue;
        }
        const char* context = IsStoryFlagCaller(work.caller) ? "story-event" : "chest-event";
        GetItemTracker().OnChestEventChecked(work.event_id, context);
    }
}

}  // namespace grandia_ap

extern "C" {
volatile unsigned g_ap_flag_event_id = 0;
volatile unsigned g_ap_flag_offset = 0;
volatile unsigned g_ap_flag_value = 0;
volatile unsigned g_ap_flag_mask = 0;
volatile unsigned g_ap_flag_ecx = 0;
volatile std::uintptr_t g_ap_flag_save_base = 0;
volatile std::uintptr_t g_ap_flag_caller = 0;
volatile uint8_t g_ap_flag_value_byte = 0;
extern volatile std::uintptr_t g_ap_assign_return_addr;
extern volatile std::uintptr_t g_ap_assign_stack_pointer;
}

extern "C" void ApChestEventNotify() {
    if (grandia_ap::IsApDelivering()) {
        return;
    }
    grandia_ap::QueueChestEventPickup(
        g_ap_flag_event_id, g_ap_flag_offset, g_ap_flag_value, g_ap_flag_mask, g_ap_flag_ecx,
        g_ap_flag_save_base, g_ap_flag_caller);
}

extern "C" int ApAssignUiNotify() {
    return grandia_ap::TryInterceptChestAssignUi(g_ap_assign_return_addr, g_ap_assign_stack_pointer);
}

extern "C" int ApShouldSuppressFieldGold() {
    return grandia_ap::TrySuppressVanillaFieldGold();
}
