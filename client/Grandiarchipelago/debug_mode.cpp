#include "debug_mode.h"

#include "d3d_overlay.h"
#include "game_memory.h"
#include "log.h"
#include "movie_skip.h"

#include <Windows.h>
#include <Xinput.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>

#pragma comment(lib, "Xinput9_1_0.lib")

#if defined(_M_IX86)
extern "C" {
void* g_ap_orig_pad_refresh_dbg = nullptr;
void ApDebugPadAfterRefresh();
void ApDebugPadRefreshDetour();
}
#endif

namespace grandia_ap {
namespace {

// Parked: F4 field debug overlay (Camera/Flags/Object/Map/Save/Sheet). Re-enable later.
constexpr bool kDebugOverlayEnabled = false;

// Debug overlay (field tick +0x5F0A0): needs latch 0x4000 + ASCII flag "4000".
// Pages @ 0x64122B: 1=Camera 3=Scenario 4=Object 5=Map/Enc 6=LoadSave 7=DebugSheet
// Cursor gaps: Camera reads 0x719452 bit0; Map needs 0x719446 bit0; Object needs held bit0.

constexpr std::uintptr_t kDebugFlagRva = 0x23F00Eu;
constexpr std::uintptr_t kPadRefreshCallRva = 0x55FC7u;
constexpr std::uintptr_t kPadHeldRva = 0x319440u;
constexpr std::uintptr_t kPadTrigRva = 0x319446u;
constexpr std::uintptr_t kPadCamRva = 0x319452u;
constexpr std::uintptr_t kPadLatchRva = 0x319464u;
constexpr std::uintptr_t kDebugPageRva = 0x24122Bu;
// Scenario-flag blob; ENCOUNT OFF = bit 0x04 at +0x11F (flag id 0x8FD via +0x70400).
constexpr std::uintptr_t kFlagBlobPtrRva = 0x318BD8u;
constexpr std::uintptr_t kEncounterOffByteOff = 0x11Fu;
constexpr uint8_t kEncounterOffBit = 0x04u;

constexpr unsigned kDebugFlagOff = 0x30303030u;  // "0000"
constexpr unsigned kDebugFlagOn = 0x30303034u;   // "4000"
constexpr uint16_t kLatchOverlay = 0x4000u;

constexpr UINT kDebugHotkeyScan = 0x29u;  // ² / tilde — flag only (legacy)
constexpr int kOverlayToggleVk = VK_F4;
constexpr int kOverlayCycleVk = VK_F5;
constexpr int kCamGroupVk = VK_F6;
constexpr int kMapModeVk = VK_F7;
constexpr int kEncounterToggleVk = VK_F8;

// Skip empty page 2.
constexpr uint8_t kPages[] = {1, 3, 4, 5, 6, 7};
constexpr int kPageCount = sizeof(kPages) / sizeof(kPages[0]);

bool g_flag_hotkey_was_down = false;
bool g_overlay_toggle_was_down = false;
bool g_overlay_cycle_was_down = false;
bool g_cam_group_was_down = false;
bool g_map_mode_was_down = false;
bool g_encounter_was_down = false;
bool g_encounter_pad_was_down = false;

void* g_call_site = nullptr;
uint8_t g_call_original[5]{};
bool g_pad_hook_installed = false;
std::atomic<bool> g_overlay_active{false};
std::atomic<bool> g_cam_view_group{false};  // false=ZOOM/SHADOW, true=VIEW X/Y (P2 bit0)
std::atomic<bool> g_map_party_mode{false};  // Map page: trig bit0 → party/enc vs map digits

const char* PageName(uint8_t page) {
    switch (page) {
        case 1:
            return "Camera";
        case 3:
            return "Scenario Flags";
        case 4:
            return "Object/Parts";
        case 5:
            return "Map/Party/Enc";
        case 6:
            return "Load & Save";
        case 7:
            return "Debug Sheet";
        default:
            return "Debug";
    }
}

UINT FlagHotkeyVk() {
    const UINT vk = MapVirtualKeyA(kDebugHotkeyScan, MAPVK_VSC_TO_VK);
    return vk != 0 ? vk : static_cast<UINT>(VK_OEM_7);
}

bool EdgePress(int vk, bool& was_down) {
    const bool down = (GetAsyncKeyState(vk) & 0x8000) != 0;
    if (!down) {
        was_down = false;
        return false;
    }
    if (was_down) {
        return false;
    }
    was_down = true;
    return true;
}

bool GamepadSelectR1Down() {
    for (DWORD i = 0; i < XUSER_MAX_COUNT; ++i) {
        XINPUT_STATE state{};
        if (XInputGetState(i, &state) != ERROR_SUCCESS) {
            continue;
        }
        const WORD buttons = state.Gamepad.wButtons;
        if ((buttons & XINPUT_GAMEPAD_BACK) && (buttons & XINPUT_GAMEPAD_RIGHT_SHOULDER)) {
            return true;
        }
    }
    return false;
}

bool WriteBytes(void* site, const void* bytes, size_t size, uint8_t* original_out) {
    DWORD old_protect = 0;
    if (!VirtualProtect(site, size, PAGE_EXECUTE_READWRITE, &old_protect)) {
        return false;
    }
    if (original_out) {
        std::memcpy(original_out, site, size);
    }
    std::memcpy(site, bytes, size);
    VirtualProtect(site, size, old_protect, &old_protect);
    FlushInstructionCache(GetCurrentProcess(), site, size);
    return true;
}

bool WriteCall(void* site, void* destination, uint8_t* original_out) {
    auto* bytes = reinterpret_cast<uint8_t*>(site);
    if (bytes[0] != 0xE8) {
        return false;
    }
    uint8_t patch[5] = {0xE8};
    const auto rel = static_cast<int32_t>(reinterpret_cast<uint8_t*>(destination) -
                                          (bytes + 5));
    std::memcpy(patch + 1, &rel, sizeof(rel));
    return WriteBytes(site, patch, 5, original_out);
}

void RestoreBytes(void* site, const uint8_t* original, size_t size) {
    if (!site || !original) {
        return;
    }
    WriteBytes(site, original, size, nullptr);
}

void SetDebugFlag(bool on) {
    const std::uintptr_t base = GetGrandiaModuleBase();
    if (base == 0) {
        return;
    }
    *reinterpret_cast<volatile unsigned*>(base + kDebugFlagRva) =
        on ? kDebugFlagOn : kDebugFlagOff;
}

void SetOverlayLatch(bool on) {
    const std::uintptr_t base = GetGrandiaModuleBase();
    if (base == 0) {
        return;
    }
    auto* latch = reinterpret_cast<uint16_t*>(base + kPadLatchRva);
    if (on) {
        *latch = static_cast<uint16_t>(*latch | kLatchOverlay);
    } else {
        *latch = static_cast<uint16_t>(*latch & ~kLatchOverlay);
    }
}

uint8_t GetDebugPage() {
    const std::uintptr_t base = GetGrandiaModuleBase();
    if (base == 0) {
        return 0;
    }
    return *reinterpret_cast<uint8_t*>(base + kDebugPageRva);
}

void SetDebugPage(uint8_t page) {
    const std::uintptr_t base = GetGrandiaModuleBase();
    if (base == 0) {
        return;
    }
    *reinterpret_cast<uint8_t*>(base + kDebugPageRva) = page;
}

// Same bit the Map debug page shows as "ENCOUNT OFF = ON/OFF".
bool ToggleEncounterOff() {
    const std::uintptr_t base = GetGrandiaModuleBase();
    if (base == 0) {
        return false;
    }
    bool encounters_off = false;
    __try {
        auto* blob = *reinterpret_cast<uint8_t**>(base + kFlagBlobPtrRva);
        if (!blob) {
            return false;
        }
        blob[kEncounterOffByteOff] =
            static_cast<uint8_t>(blob[kEncounterOffByteOff] ^ kEncounterOffBit);
        encounters_off = (blob[kEncounterOffByteOff] & kEncounterOffBit) != 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    LogInfo("Encounter off %s (F8)", encounters_off ? "ON" : "OFF");
    ShowD3dOverlayToast(encounters_off ? "Encounters OFF" : "Encounters ON", 2500,
                        encounters_off ? 0x7CFC00u : 0xFFE528u);
    return true;
}

void CycleDebugPage() {
    const uint8_t cur = GetDebugPage();
    int idx = 0;
    for (; idx < kPageCount; ++idx) {
        if (kPages[idx] == cur) {
            break;
        }
    }
    if (idx >= kPageCount) {
        idx = 0;
    } else {
        idx = (idx + 1) % kPageCount;
    }
    const uint8_t next = kPages[idx];
    SetDebugPage(next);
    LogInfo("Debug menu page %u (%s)", static_cast<unsigned>(next), PageName(next));
    char toast[64];
    std::snprintf(toast, sizeof(toast), "Debug: %s (F5 next)", PageName(next));
    ShowD3dOverlayToast(toast, 2000, 0x7CFC00u);
}

void EnterDebugOverlay() {
    SetDebugFlag(true);
    SetOverlayLatch(true);
    SetDebugPage(kPages[0]);
    g_overlay_active.store(true);
    g_cam_view_group.store(false);
    g_map_party_mode.store(false);
    LogInfo("Debug overlay ON (F4 toggle, F5 page, F6 cam group, F7 map mode)");
    ShowD3dOverlayToast("Debug ON — F5 page / F6 cam / F7 map mode", 3500, 0x7CFC00u);
}

void ExitDebugOverlay() {
    SetOverlayLatch(false);
    SetDebugPage(0);
    // Leave ASCII debug flag alone if user enabled it via ²; only clear latch/page.
    g_overlay_active.store(false);
    LogInfo("Debug overlay OFF");
    ShowD3dOverlayToast("Debug overlay OFF", 2000, 0xFFE528u);
}

void InjectCursorAssist() {
    const std::uintptr_t base = GetGrandiaModuleBase();
    if (base == 0) {
        return;
    }
    auto* held = reinterpret_cast<uint16_t*>(base + kPadHeldRva);
    auto* trig = reinterpret_cast<uint16_t*>(base + kPadTrigRva);
    auto* pad_p2 = reinterpret_cast<uint16_t*>(base + kPadCamRva);  // +0x12 = player-2 held

    // Hub: held 0x100 forces page back to 0 — never leave that sticky.
    *held = static_cast<uint16_t>(*held & ~static_cast<uint16_t>(0x0100u));

    const uint8_t page = GetDebugPage();
    switch (page) {
        case 1:
            // Camera Control reads player-2 pad @ 719452, not P1. Mirror P1 held.
            *pad_p2 = *held;
            if (g_cam_view_group.load()) {
                *pad_p2 = static_cast<uint16_t>(*pad_p2 & ~static_cast<uint16_t>(0x0001u));
            } else {
                *pad_p2 = static_cast<uint16_t>(*pad_p2 | 0x0001u);
            }
            break;
        case 3:
            // Scenario Flags: gate is held bit 0x08 on the copied pad object.
            *held = static_cast<uint16_t>(*held | 0x0008u);
            break;
        case 4:
            // Object/Parts: gate is held bit 0x01.
            *held = static_cast<uint16_t>(*held | 0x0001u);
            break;
        case 5:
            // Map: trig bit0 selects party/encounter branch; clear = map# digits.
            if (g_map_party_mode.load()) {
                *trig = static_cast<uint16_t>(*trig | 0x0001u);
            } else {
                *trig = static_cast<uint16_t>(*trig & ~static_cast<uint16_t>(0x0001u));
            }
            break;
        case 6:
            // Load & Save: needs held bit0; nav uses 719444 (filled by pad SM).
            *held = static_cast<uint16_t>(*held | 0x0001u);
            break;
        default:
            break;
    }
}

}  // namespace

void ApplyDebugPadAssist() {
    if (!g_overlay_active.load()) {
        return;
    }
    // Keep latch sticky while overlay is on (game may clear it).
    SetOverlayLatch(true);
    InjectCursorAssist();
}

bool IsDebugModeEnabled() {
    const std::uintptr_t base = GetGrandiaModuleBase();
    if (base == 0) {
        return false;
    }
    const auto* flag = reinterpret_cast<const volatile unsigned*>(base + kDebugFlagRva);
    return *flag == kDebugFlagOn;
}

bool IsDebugOverlayActive() { return g_overlay_active.load(); }

bool InstallDebugOverlayHook() {
    if (!kDebugOverlayEnabled) {
        LogInfo("Debug overlay: parked (F4 menu deferred — F8/Select+R1 encounter still active)");
        return true;
    }
#if !defined(_M_IX86)
    return false;
#else
    if (g_pad_hook_installed) {
        return true;
    }
    const std::uintptr_t base = GetGrandiaModuleBase();
    if (base == 0) {
        LogWarn("Debug overlay: grandia base unknown");
        return false;
    }

    auto* site = reinterpret_cast<uint8_t*>(base + kPadRefreshCallRva);
    if (site[0] != 0xE8) {
        LogWarn("Debug overlay: pad-refresh call mismatch at +0x%X",
                static_cast<unsigned>(kPadRefreshCallRva));
        return false;
    }

    // If map_overview already hooked this call, don't double-hook.
    // We own this site now — map_overview is disabled.
    const auto old_rel = *reinterpret_cast<int32_t*>(site + 1);
    g_ap_orig_pad_refresh_dbg = reinterpret_cast<void*>(site + 5 + old_rel);

    if (!WriteCall(site, reinterpret_cast<void*>(&ApDebugPadRefreshDetour), g_call_original)) {
        g_ap_orig_pad_refresh_dbg = nullptr;
        LogWarn("Debug overlay: failed to hook pad refresh");
        return false;
    }

    g_call_site = site;
    g_pad_hook_installed = true;
    LogInfo("Debug overlay hook active (F4 on/off, F5 page, F6 camera group)");
    return true;
#endif
}

void RemoveDebugOverlayHook() {
    if (g_overlay_active.load()) {
        ExitDebugOverlay();
    }
    if (g_call_site) {
        RestoreBytes(g_call_site, g_call_original, 5);
        g_call_site = nullptr;
    }
#if defined(_M_IX86)
    g_ap_orig_pad_refresh_dbg = nullptr;
#endif
    g_pad_hook_installed = false;
}

void PollDebugModeHotkey() {
    const std::uintptr_t base = GetGrandiaModuleBase();
    if (base == 0) {
        return;
    }

    // Legacy ² : toggle ASCII debug flag only (9999 dmg etc.) without overlay.
    if (EdgePress(static_cast<int>(FlagHotkeyVk()), g_flag_hotkey_was_down)) {
        const bool enable = !IsDebugModeEnabled();
        SetDebugFlag(enable);
        LogInfo("Debug flag %s", enable ? "ON" : "OFF");
        ShowD3dOverlayToast(enable ? "Debug flag ON" : "Debug flag OFF", 2500,
                            enable ? 0x7CFC00u : 0xFFE528u);
    }

    // F8 or Select+R1: encounter off (same bit as Map debug "ENCOUNT OFF").
    bool encounter_edge = EdgePress(kEncounterToggleVk, g_encounter_was_down);
    const bool enc_pad = GamepadSelectR1Down();
    if (!IsMoviePlaying() && enc_pad && !g_encounter_pad_was_down) {
        encounter_edge = true;
    }
    g_encounter_pad_was_down = enc_pad;
    if (encounter_edge) {
        if (!ToggleEncounterOff()) {
            ShowD3dOverlayToast("Encounter toggle unavailable (load a save)", 2500, 0xFA8072u);
        }
    }

    if constexpr (!kDebugOverlayEnabled) {
        return;
    }
    if (!g_pad_hook_installed) {
        return;
    }

    if (EdgePress(kOverlayToggleVk, g_overlay_toggle_was_down)) {
        if (g_overlay_active.load()) {
            ExitDebugOverlay();
        } else {
            EnterDebugOverlay();
        }
    }

    if (g_overlay_active.load() && EdgePress(kOverlayCycleVk, g_overlay_cycle_was_down)) {
        CycleDebugPage();
    }

    if (g_overlay_active.load() && EdgePress(kCamGroupVk, g_cam_group_was_down)) {
        const bool view = !g_cam_view_group.load();
        g_cam_view_group.store(view);
        ShowD3dOverlayToast(view ? "Camera: VIEW X/Y group" : "Camera: ZOOM/SHADOW group", 2000,
                            0x7CFC00u);
    }

    if (g_overlay_active.load() && EdgePress(kMapModeVk, g_map_mode_was_down)) {
        const bool party = !g_map_party_mode.load();
        g_map_party_mode.store(party);
        ShowD3dOverlayToast(party ? "Map: Party/Encounter mode" : "Map: Map# digit mode", 2000,
                            0x7CFC00u);
    }
}

}  // namespace grandia_ap

#if defined(_M_IX86)
extern "C" {

void ApDebugPadAfterRefresh() { grandia_ap::ApplyDebugPadAssist(); }

void __declspec(naked) ApDebugPadRefreshDetour() {
    __asm {
        call dword ptr [g_ap_orig_pad_refresh_dbg]
        pushad
        call ApDebugPadAfterRefresh
        popad
        ret
    }
}

}  // extern "C"
#endif
