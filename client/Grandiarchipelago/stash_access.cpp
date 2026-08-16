#include "stash_access.h"

#include "d3d_overlay.h"
#include "game_memory.h"
#include "log.h"
#include "movie_skip.h"
#include "xinput_dyn.h"

#include <Windows.h>
#include <Xinput.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>

#if defined(_M_IX86)
extern "C" void ApFieldTickStashDetour();
extern "C" void* g_ap_field_tick_stash_tramp = nullptr;
#endif

namespace grandia_ap {
namespace {

// Field tick (+55F40) already calls menu FSM +66E90 each frame. We run just before
// that call so a pending +670C0 stash request is consumed on the game thread.
constexpr std::uintptr_t kFieldTickMenuCallRva = 0x55F4Cu;  // call +66E90
constexpr std::uintptr_t kOpenMenuFnRva = 0x670C0u;
// +670C0 cl = top-level UI: 0=FWIN, 1=SHOP.BIN, 2=ITEM.BIN
// For SHOP.BIN, dl selects mode. Stock stubs:
//   +6FD4C dl=3 (Stash Item / deposit), +6FD61 dl=4 (Get Item / withdraw).
constexpr uint8_t kMenuTypeShop = 1u;
constexpr uint8_t kShopModeStashItem = 3u;  // deposit to stash
constexpr uint8_t kShopModeGetItem = 4u;    // withdraw from stash

// Soft gates mirrored from +670C0 (so we can toast instead of silent no-op).
constexpr std::uintptr_t kMenuPhaseWordRva = 0x319428u;   // VA 0x719428
constexpr std::uintptr_t kMenuReadyDwordRva = 0x31942Cu;  // VA 0x71942C (==1 when idle)
constexpr std::uintptr_t kMenuBusyFlagRva = 0x23FA5Au;    // VA 0x63FA5A
constexpr std::uintptr_t kFieldBusyFlagRva = 0x31CD38u;   // VA 0x71CD38
constexpr std::uintptr_t kInputMaskWordRva = 0x319464u;   // VA 0x719464 bit 0x8000 blocks

// DualShock → XInput: Square=X, Circle=B, Select=Back.
constexpr WORD kFaceSquare = XINPUT_GAMEPAD_X;
constexpr WORD kFaceCircle = XINPUT_GAMEPAD_B;

void* g_call_site = nullptr;
uint8_t g_call_original[5]{};
bool g_installed = false;

// 0 = none; otherwise SHOP.BIN dl mode to open on the next field tick.
std::atomic<uint8_t> g_pending_shop_mode{0};
bool g_square_was_down = false;
bool g_circle_was_down = false;

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

bool GamepadSelectFaceDown(WORD face_button) {
    for (DWORD i = 0; i < XUSER_MAX_COUNT; ++i) {
        XINPUT_STATE state{};
        if (XInputGetStateDyn(i, &state) != ERROR_SUCCESS) {
            continue;
        }
        const WORD buttons = state.Gamepad.wButtons;
        if ((buttons & XINPUT_GAMEPAD_BACK) && (buttons & face_button)) {
            return true;
        }
    }
    return false;
}

const char* SoftGateReason(std::uintptr_t base) {
    __try {
        if (*reinterpret_cast<uint32_t*>(base + kMenuReadyDwordRva) != 1u) {
            return "not on field";
        }
        if (*reinterpret_cast<uint16_t*>(base + kMenuPhaseWordRva) != 0) {
            return "menu busy";
        }
        if (*reinterpret_cast<uint8_t*>(base + kMenuBusyFlagRva) != 0) {
            return "menu busy";
        }
        if (*reinterpret_cast<uint8_t*>(base + kFieldBusyFlagRva) != 0) {
            return "field busy";
        }
        if ((*reinterpret_cast<uint16_t*>(base + kInputMaskWordRva) & 0x8000u) != 0) {
            return "input locked";
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return "unavailable";
    }
    return nullptr;
}

#if defined(_M_IX86)
// +670C0: cl = menu type, dl = shop/sub mode, [esp+4] = third byte; cdecl (caller cleans 4).
void CallOpenMenuType(std::uintptr_t base, uint8_t menu_type, uint8_t shop_mode) {
    void* fn = reinterpret_cast<void*>(base + kOpenMenuFnRva);
    const uint32_t type32 = menu_type;
    const uint32_t mode32 = shop_mode;
    __asm {
        push 0
        mov edx, mode32
        mov ecx, type32
        call fn
        add esp, 4
    }
}
#endif

}  // namespace

#if defined(_M_IX86)
extern "C" void ApApplyPendingStashOpen() {
    const uint8_t mode = g_pending_shop_mode.exchange(0);
    if (mode == 0) {
        return;
    }
    const std::uintptr_t base = GetGrandiaModuleBase();
    if (base == 0) {
        return;
    }
    if (const char* why = SoftGateReason(base)) {
        char msg[96];
        std::snprintf(msg, sizeof(msg), "Stash unavailable (%s)", why);
        ShowD3dOverlayToast(msg, 1600, 0xFFAA55u);
        LogInfo("Stash access: blocked — %s", why);
        return;
    }
    CallOpenMenuType(base, kMenuTypeShop, mode);
    const char* label = (mode == kShopModeStashItem) ? "Stash Item" : "Get Item";
    char toast[48];
    std::snprintf(toast, sizeof(toast), "Stash (%s)", label);
    ShowD3dOverlayToast(toast, 1000, 0x7CFC00u);
    LogInfo("Stash access: requested SHOP %s via +0x%X type=%u mode=%u", label,
            static_cast<unsigned>(kOpenMenuFnRva), static_cast<unsigned>(kMenuTypeShop),
            static_cast<unsigned>(mode));
}

extern "C" void __declspec(naked) ApFieldTickStashDetour() {
    __asm {
        pushad
        call ApApplyPendingStashOpen
        popad
        jmp dword ptr [g_ap_field_tick_stash_tramp]
    }
}
#endif

bool InstallStashAccessHook() {
#if !defined(_M_IX86)
    return false;
#else
    if (g_installed) {
        return true;
    }
    const std::uintptr_t base = GetGrandiaModuleBase();
    if (base == 0) {
        LogWarn("Stash access: grandia base unknown");
        return false;
    }

    auto* site = reinterpret_cast<uint8_t*>(base + kFieldTickMenuCallRva);
    if (site[0] != 0xE8) {
        LogWarn("Stash access: call site mismatch at +0x%X (got %02X)",
                static_cast<unsigned>(kFieldTickMenuCallRva), site[0]);
        return false;
    }

    const auto old_rel = *reinterpret_cast<int32_t*>(site + 1);
    g_ap_field_tick_stash_tramp = reinterpret_cast<void*>(site + 5 + old_rel);

    if (!WriteCall(site, reinterpret_cast<void*>(&ApFieldTickStashDetour), g_call_original)) {
        g_ap_field_tick_stash_tramp = nullptr;
        LogWarn("Stash access: failed to patch field tick call");
        return false;
    }

    g_call_site = site;
    g_installed = true;
    g_pending_shop_mode.store(0);
    LogInfo("Stash access active — Select+Square=Get Item / Select+Circle=Stash Item");
    return true;
#endif
}

void RemoveStashAccessHook() {
    if (g_call_site) {
        RestoreBytes(g_call_site, g_call_original, 5);
        g_call_site = nullptr;
    }
#if defined(_M_IX86)
    g_ap_field_tick_stash_tramp = nullptr;
#endif
    g_pending_shop_mode.store(0);
    g_installed = false;
}

void PollStashAccessHotkey() {
    if (!g_installed) {
        return;
    }

    const bool square_down = GamepadSelectFaceDown(kFaceSquare);
    const bool circle_down = GamepadSelectFaceDown(kFaceCircle);

    if (IsMoviePlaying()) {
        g_square_was_down = square_down;
        g_circle_was_down = circle_down;
        return;
    }

    const bool square_edge = square_down && !g_square_was_down;
    const bool circle_edge = circle_down && !g_circle_was_down;
    g_square_was_down = square_down;
    g_circle_was_down = circle_down;

    // Prefer Get Item if both edges somehow fire together.
    if (square_edge) {
        g_pending_shop_mode.store(kShopModeGetItem);
    } else if (circle_edge) {
        g_pending_shop_mode.store(kShopModeStashItem);
    }
}

}  // namespace grandia_ap
