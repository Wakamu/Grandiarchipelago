#include "debug_mode.h"

#include "d3d_overlay.h"
#include "game_memory.h"
#include "log.h"

#include <Windows.h>

#include <cstdint>

namespace grandia_ap {

namespace {

constexpr std::uintptr_t kDebugFlagRva = 0x23F00Eu;
constexpr unsigned kDebugFlagOff = 0x30303030u;
constexpr unsigned kDebugFlagOn = 0x30303034u;
constexpr UINT kDebugHotkeyScan = 0x29u;

bool g_debug_hotkey_was_down = false;

UINT DebugHotkeyVk() {
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

}  // namespace

bool IsDebugModeEnabled() {
    const std::uintptr_t base = GetGrandiaModuleBase();
    if (base == 0) {
        return false;
    }
    const auto* flag = reinterpret_cast<const volatile unsigned*>(base + kDebugFlagRva);
    return *flag == kDebugFlagOn;
}

void PollDebugModeHotkey() {
    if (!EdgePress(static_cast<int>(DebugHotkeyVk()), g_debug_hotkey_was_down)) {
        return;
    }

    const std::uintptr_t base = GetGrandiaModuleBase();
    if (base == 0) {
        LogWarn("Debug hotkey: grandia module base not ready");
        return;
    }

    volatile unsigned* flag = reinterpret_cast<volatile unsigned*>(base + kDebugFlagRva);
    const bool enable = *flag != kDebugFlagOn;
    *flag = enable ? kDebugFlagOn : kDebugFlagOff;
    LogInfo("Debug mode %s", enable ? "ON" : "OFF");
    ShowD3dOverlayToast(enable ? "Debug mode ON" : "Debug mode OFF", 4000,
                        enable ? 0x7CFC00u : 0xFFE528u);
}

}  // namespace grandia_ap
