#include "map_travel.h"

#include "game_memory.h"
#include "log.h"
#include "progressions_generated.h"

#include <Windows.h>

#include <atomic>
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
std::atomic<unsigned> g_trace_hits_left{48};
std::atomic<uint16_t> g_last_deny_logged{0};

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
        g_unlocked.insert(map_id);
    }
    LogInfo("Map unlocked: 0x%04X", static_cast<unsigned>(map_id));
}

void UnlockKeyGroup(uint16_t primary_or_any_map) {
    for (std::size_t g = 0; g < progressions::kKeyGroupCount; ++g) {
        const auto& group = progressions::kKeyGroups[g];
        bool match = group.primary_map == primary_or_any_map;
        if (!match) {
            for (std::size_t i = 0; i < group.count; ++i) {
                if (group.maps[i] == primary_or_any_map) {
                    match = true;
                    break;
                }
            }
        }
        if (!match) {
            continue;
        }
        for (std::size_t i = 0; i < group.count; ++i) {
            UnlockMap(group.maps[i]);
        }
        LogInfo("Key group unlocked (primary=0x%04X, %u maps)", static_cast<unsigned>(group.primary_map),
                static_cast<unsigned>(group.count));
        return;
    }
    // Unknown primary: still unlock the single map id.
    UnlockMap(primary_or_any_map);
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
    return false;
}

bool TryHandleMapKeyItem(unsigned ap_item_id) {
    if (ap_item_id < kMapKeyItemBase) {
        return false;
    }
    const unsigned map_id = ap_item_id - kMapKeyItemBase;
    if (map_id == 0 || map_id > 0xFFFFu) {
        return false;
    }
    // Item id = KEY_BASE + primary unlocks_maps[0]; unlock entire group.
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
