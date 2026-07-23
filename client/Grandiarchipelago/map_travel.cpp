#include "map_travel.h"

#include "d3d_overlay.h"
#include "game_memory.h"
#include "log.h"
#include "progressions_generated.h"

#include <Windows.h>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <unordered_set>

namespace {

// World-map confirm FSM (+0x58400): when [0x641197]==1, body at +0x58491 …
constexpr std::uintptr_t kWorldMapConfirmRva = 0x58491u;
constexpr std::uintptr_t kWorldMapConfirmResumeRva = 0x58496u;
constexpr std::uintptr_t kWorldMapConfirmSkipRva = 0x584FBu;
constexpr std::uintptr_t kWorldMapDestMapRva = 0x2C2990u;
constexpr std::uintptr_t kWorldMapConfirmStateRva = 0x241197u;

constexpr size_t kPatchSize = 5;

void* g_hook_site = nullptr;
uint8_t g_hook_original[8]{};

std::mutex g_lock;
std::unordered_set<uint16_t> g_unlocked;
std::unordered_set<uint16_t> g_owned_key_primaries;
std::atomic<unsigned> g_trace_hits_left{48};
std::atomic<uint16_t> g_last_deny_logged{0};
std::atomic<uint16_t> g_last_deny_toast_map{0};
std::atomic<DWORD> g_last_deny_toast_tick{0};

bool WriteJump(void* site, void* destination, uint8_t* original_out, size_t patch_size) {
    DWORD old_protect = 0;
    if (!VirtualProtect(site, patch_size, PAGE_EXECUTE_READWRITE, &old_protect)) {
        return false;
    }
    std::memcpy(original_out, site, patch_size);
    const auto rel = static_cast<int32_t>(reinterpret_cast<uint8_t*>(destination) -
                                          (reinterpret_cast<uint8_t*>(site) + 5));
    auto* bytes = reinterpret_cast<uint8_t*>(site);
    bytes[0] = 0xE9;
    std::memcpy(bytes + 1, &rel, sizeof(rel));
    for (size_t i = 5; i < patch_size; ++i) {
        bytes[i] = 0x90;
    }
    VirtualProtect(site, patch_size, old_protect, &old_protect);
    FlushInstructionCache(GetCurrentProcess(), site, patch_size);
    return true;
}

void RestoreBytes(void* site, const uint8_t* original, size_t size) {
    DWORD old_protect = 0;
    if (!VirtualProtect(site, size, PAGE_EXECUTE_READWRITE, &old_protect)) {
        return;
    }
    std::memcpy(site, original, size);
    VirtualProtect(site, size, old_protect, &old_protect);
    FlushInstructionCache(GetCurrentProcess(), site, size);
}

bool GatedSetContains(uint16_t map_id) {
    for (std::size_t i = 0; i < grandia_ap::progressions::kGatedMapCount; ++i) {
        if (grandia_ap::progressions::kGatedMaps[i] == map_id) {
            return true;
        }
    }
    return false;
}

}  // namespace

extern "C" {

#if defined(_M_IX86)

void* g_ap_wm_confirm_resume = nullptr;
void* g_ap_wm_confirm_skip = nullptr;
uint8_t g_ap_wm_confirm_orig_mov_eax[5]{};
uint16_t* g_ap_world_map_dest = nullptr;
uint8_t* g_ap_world_map_confirm_state = nullptr;
volatile unsigned g_ap_map_travel_pending_id = 0;
volatile int g_ap_map_travel_allow = 1;

void ApOnWorldMapConfirm() {
    const uint16_t map_id = static_cast<uint16_t>(g_ap_map_travel_pending_id);
    const bool allow = grandia_ap::AllowMapTravel(map_id);
    g_ap_map_travel_allow = allow ? 1 : 0;
    if (!allow && g_ap_world_map_confirm_state) {
        *g_ap_world_map_confirm_state = 0;
    }
}

__declspec(naked) void ApWorldMapConfirmDetour() {
    __asm {
        pushad

        mov eax, dword ptr [g_ap_world_map_dest]
        movzx eax, word ptr [eax]
        mov dword ptr [g_ap_map_travel_pending_id], eax

        mov eax, esp
        and esp, 0FFFFFFF0h
        sub esp, 16
        mov dword ptr [esp], eax
        call ApOnWorldMapConfirm
        mov esp, dword ptr [esp]

        popad

        cmp dword ptr [g_ap_map_travel_allow], 0
        je deny

        mov eax, dword ptr [g_ap_wm_confirm_orig_mov_eax + 1]
        mov eax, dword ptr [eax]
        jmp dword ptr [g_ap_wm_confirm_resume]

    deny:
        jmp dword ptr [g_ap_wm_confirm_skip]
    }
}

#endif

}  // extern "C"

namespace grandia_ap {

bool IsMapGated(uint16_t map_id) { return GatedSetContains(map_id); }

bool IsMapUnlocked(uint16_t map_id) {
    std::lock_guard<std::mutex> lock(g_lock);
    return g_unlocked.find(map_id) != g_unlocked.end();
}

void UnlockMap(uint16_t map_id) {
    if (map_id == 0) {
        return;
    }
    {
        std::lock_guard<std::mutex> lock(g_lock);
        if (!g_unlocked.insert(map_id).second) {
            return;
        }
    }
    LogInfo("Map unlocked: 0x%04X", static_cast<unsigned>(map_id));
}

void ClearMapKeyState() {
    std::lock_guard<std::mutex> lock(g_lock);
    const auto n_maps = g_unlocked.size();
    const auto n_keys = g_owned_key_primaries.size();
    g_unlocked.clear();
    g_owned_key_primaries.clear();
    if (n_maps != 0 || n_keys != 0) {
        LogInfo("Cleared map-key state (%zu unlocks, %zu key primaries) for save SYNC", n_maps,
                n_keys);
    }
}

bool OwnsAllKeysUpToLocked(unsigned value) {
    for (std::size_t g = 0; g < progressions::kKeyGroupCount; ++g) {
        const auto& group = progressions::kKeyGroups[g];
        if (group.value > value) {
            continue;
        }
        if (g_owned_key_primaries.find(group.primary_map) == g_owned_key_primaries.end()) {
            return false;
        }
    }
    return true;
}

void UnlockMapsForSatisfiedKeys() {
    struct PendingGroup {
        uint16_t primary = 0;
        unsigned value = 0;
        unsigned count = 0;
        unsigned newly = 0;
    };
    PendingGroup pending[64]{};
    std::size_t pending_n = 0;

    {
        std::lock_guard<std::mutex> lock(g_lock);
        for (std::size_t g = 0; g < progressions::kKeyGroupCount; ++g) {
            const auto& group = progressions::kKeyGroups[g];
            if (g_owned_key_primaries.find(group.primary_map) == g_owned_key_primaries.end()) {
                continue;
            }
            if (!OwnsAllKeysUpToLocked(group.value)) {
                continue;
            }
            unsigned newly = 0;
            for (std::size_t i = 0; i < group.count; ++i) {
                if (g_unlocked.insert(group.maps[i]).second) {
                    ++newly;
                }
            }
            if (newly > 0 && pending_n < 64) {
                pending[pending_n++] = {group.primary_map, group.value,
                                       static_cast<unsigned>(group.count), newly};
            }
        }
    }

    for (std::size_t i = 0; i < pending_n; ++i) {
        LogInfo("Key group unlocked (primary=0x%04X value=%u, %u maps)",
                static_cast<unsigned>(pending[i].primary), pending[i].value, pending[i].count);
    }
}

const progressions::KeyUnlockGroup* FindKeyGroup(uint16_t primary_or_any_map) {
    for (std::size_t g = 0; g < progressions::kKeyGroupCount; ++g) {
        const auto& group = progressions::kKeyGroups[g];
        if (group.primary_map == primary_or_any_map) {
            return &group;
        }
        for (std::size_t i = 0; i < group.count; ++i) {
            if (group.maps[i] == primary_or_any_map) {
                return &group;
            }
        }
    }
    return nullptr;
}

void UnlockKeyGroup(uint16_t primary_or_any_map) {
    const auto* group = FindKeyGroup(primary_or_any_map);
    if (!group) {
        UnlockMap(primary_or_any_map);
        return;
    }
    {
        std::lock_guard<std::mutex> lock(g_lock);
        g_owned_key_primaries.insert(group->primary_map);
    }
    UnlockMapsForSatisfiedKeys();
}

const progressions::KeyUnlockGroup* FindKeyGroup(uint16_t primary_or_any_map);

void ToastMissingKeyForMap(uint16_t map_id) {
    const char* missing_name = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_lock);
        const auto* dest = FindKeyGroup(map_id);
        const unsigned need_value = dest ? dest->value : 0xFFFFFFFFu;
        for (std::size_t g = 0; g < progressions::kKeyGroupCount; ++g) {
            const auto& group = progressions::kKeyGroups[g];
            if (group.value > need_value) {
                continue;
            }
            if (g_owned_key_primaries.find(group.primary_map) == g_owned_key_primaries.end()) {
                missing_name = group.name;
                break;  // lowest-value missing prerequisite
            }
        }
        if (!missing_name && dest) {
            missing_name = dest->name;
        }
    }

    char buf[160];
    if (missing_name && missing_name[0]) {
        snprintf(buf, sizeof(buf), "Need %s to travel here", missing_name);
    } else {
        snprintf(buf, sizeof(buf), "Need a world map key to travel here (0x%04X)",
                 static_cast<unsigned>(map_id));
    }
    // Plum — same as progression item toasts.
    ShowD3dOverlayToast(buf, 5000, 0xDDA0DD);
}

bool AllowMapTravel(uint16_t map_id) {
    const bool gated = IsMapGated(map_id);
    unsigned left = g_trace_hits_left.load();
    if (left > 0 && g_trace_hits_left.compare_exchange_strong(left, left - 1)) {
        LogInfo("World-map confirm SI=0x%04X gated=%d", static_cast<unsigned>(map_id), gated ? 1 : 0);
    }
    if (!gated) {
        return true;
    }
    if (IsMapUnlocked(map_id)) {
        LogInfo("World-map allow (key) 0x%04X", static_cast<unsigned>(map_id));
        return true;
    }
    if (g_last_deny_logged.exchange(map_id) != map_id) {
        LogWarn("World-map DENY (need key) 0x%04X", static_cast<unsigned>(map_id));
    }
    const DWORD now = GetTickCount();
    const uint16_t prev_map = g_last_deny_toast_map.load();
    const DWORD prev_tick = g_last_deny_toast_tick.load();
    // Avoid stacking identical toasts if confirm re-fires; allow repeat after 2.5s.
    if (prev_map != map_id || now - prev_tick >= 2500) {
        g_last_deny_toast_map.store(map_id);
        g_last_deny_toast_tick.store(now);
        ToastMissingKeyForMap(map_id);
    }
    return false;
}

bool TryHandleMapKeyItem(unsigned ap_item_id) {
    if (ap_item_id < kMapKeyItemBase) {
        return false;
    }
    // Lockout tokens live at 0x47540000+; do not treat as map keys.
    constexpr unsigned kLockoutItemBase = 0x47540000u;
    if (ap_item_id >= kLockoutItemBase) {
        return false;
    }
    const unsigned map_id = ap_item_id - kMapKeyItemBase;
    if (map_id == 0 || map_id > 0xFFFFu) {
        return false;
    }
    // Item id = KEY_BASE + primary unlocks_maps[0].
    // Maps unlock only once every key with value 1..N is owned (N = this key's value).
    UnlockKeyGroup(static_cast<uint16_t>(map_id));
    return true;
}

bool InstallMapTravelHook() {
#if !defined(_M_IX86)
    LogWarn("Map travel hook requires 32-bit build");
    return false;
#else
    const std::uintptr_t base = GetGrandiaModuleBase();
    if (base == 0) {
        LogWarn("Map travel: grandia base unknown");
        return false;
    }

    auto* site = reinterpret_cast<uint8_t*>(base + kWorldMapConfirmRva);
    if (site[0] != 0xA1) {
        LogWarn("Map travel: world-map confirm bytes mismatch at +0x%X (expected A1, got %02X)",
                static_cast<unsigned>(kWorldMapConfirmRva), site[0]);
        return false;
    }

    g_ap_world_map_dest = reinterpret_cast<uint16_t*>(base + kWorldMapDestMapRva);
    g_ap_world_map_confirm_state = reinterpret_cast<uint8_t*>(base + kWorldMapConfirmStateRva);
    g_ap_wm_confirm_resume = reinterpret_cast<void*>(base + kWorldMapConfirmResumeRva);
    g_ap_wm_confirm_skip = reinterpret_cast<void*>(base + kWorldMapConfirmSkipRva);

    if (!WriteJump(site, reinterpret_cast<void*>(ApWorldMapConfirmDetour), g_hook_original, kPatchSize)) {
        LogWarn("Map travel: failed to install jump at +0x%X", static_cast<unsigned>(kWorldMapConfirmRva));
        return false;
    }
    std::memcpy(g_ap_wm_confirm_orig_mov_eax, g_hook_original, 5);
    g_hook_site = site;

    LogInfo("Installed world-map gate at grandia.exe+0x%X (%u gated maps from progressions.json)",
            static_cast<unsigned>(kWorldMapConfirmRva), static_cast<unsigned>(progressions::kGatedMapCount));
    return true;
#endif
}

void RemoveMapTravelHook() {
#if defined(_M_IX86)
    if (g_hook_site) {
        RestoreBytes(g_hook_site, g_hook_original, kPatchSize);
        g_hook_site = nullptr;
    }
    g_ap_world_map_dest = nullptr;
    g_ap_world_map_confirm_state = nullptr;
#endif
}

bool IsMapTravelHookInstalled() { return g_hook_site != nullptr; }

}  // namespace grandia_ap
