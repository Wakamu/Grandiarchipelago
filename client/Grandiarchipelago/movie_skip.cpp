#include "movie_skip.h"

#include "d3d_overlay.h"
#include "game_memory.h"
#include "log.h"

#include <Windows.h>
#include <Xinput.h>

#include <atomic>
#include <cstdint>
#include <cstring>

#pragma comment(lib, "Xinput9_1_0.lib")

#if defined(_M_IX86)
extern "C" void ApMovieInputDetour();
#endif

namespace grandia_ap {
namespace {

// Movie_Play wait loop: call MovieInputPoll @ +0x1DBA30 → +0x1DBBE0
constexpr std::uintptr_t kMovieInputCallRva = 0x1DBA30u;
// After input: test esi,esi / je / test eax,0x800  @ +0x1DBA3A
constexpr std::uintptr_t kMovieSkipAllowJeRva = 0x1DBA3Cu;
constexpr std::uintptr_t kMovieInputFlagsRva = 0x301514u;
constexpr std::uintptr_t kMoviePlayerPtrRva = 0x2C2414u;
// Normal skip button bit (NOT 0x90F — that abort path returns to title)
constexpr unsigned kMovieSkipButtonBit = 0x800u;
constexpr uint8_t kSkipAllowJeOriginal[2] = {0x74, 0x07};  // je +0x07

using MovieInputFn = void (*)();

MovieInputFn g_orig_movie_input = nullptr;
void* g_call_site = nullptr;
void* g_allow_je_site = nullptr;
uint8_t g_call_original[5]{};
uint8_t g_allow_je_original[2]{};
bool g_installed = false;

std::atomic<bool> g_skip_requested{false};
bool g_select_was_down = false;
bool g_key_was_down = false;

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

void ApplyPendingSkip() {
    if (!g_skip_requested.exchange(false)) {
        return;
    }
    const std::uintptr_t base = GetGrandiaModuleBase();
    if (base == 0) {
        return;
    }
    auto* flags = reinterpret_cast<unsigned*>(base + kMovieInputFlagsRva);
    *flags |= kMovieSkipButtonBit;
}

bool GamepadSelectDown() {
    for (DWORD i = 0; i < XUSER_MAX_COUNT; ++i) {
        XINPUT_STATE state{};
        if (XInputGetState(i, &state) != ERROR_SUCCESS) {
            continue;
        }
        if (state.Gamepad.wButtons & XINPUT_GAMEPAD_BACK) {
            return true;
        }
    }
    return false;
}

}  // namespace

#if defined(_M_IX86)
extern "C" void ApMovieInputDetour() {
    if (g_orig_movie_input) {
        g_orig_movie_input();
    }
    ApplyPendingSkip();
}
#endif

bool IsMoviePlaying() {
    const std::uintptr_t base = GetGrandiaModuleBase();
    if (base == 0) {
        return false;
    }
    const auto* player = reinterpret_cast<void* const*>(base + kMoviePlayerPtrRva);
    return *player != nullptr;
}

bool InstallMovieSkipHook() {
#if !defined(_M_IX86)
    return false;
#else
    if (g_installed) {
        return true;
    }
    const std::uintptr_t base = GetGrandiaModuleBase();
    if (base == 0) {
        LogWarn("Movie skip: grandia base unknown");
        return false;
    }

    auto* site = reinterpret_cast<uint8_t*>(base + kMovieInputCallRva);
    if (site[0] != 0xE8) {
        LogWarn("Movie skip: call site mismatch at +0x%X (got %02X)",
                static_cast<unsigned>(kMovieInputCallRva), site[0]);
        return false;
    }

    auto* allow_je = reinterpret_cast<uint8_t*>(base + kMovieSkipAllowJeRva);
    if (allow_je[0] != kSkipAllowJeOriginal[0] || allow_je[1] != kSkipAllowJeOriginal[1]) {
        LogWarn("Movie skip: allow-check JE mismatch at +0x%X",
                static_cast<unsigned>(kMovieSkipAllowJeRva));
        return false;
    }

    const auto old_rel = *reinterpret_cast<int32_t*>(site + 1);
    g_orig_movie_input = reinterpret_cast<MovieInputFn>(site + 5 + old_rel);

    // Allow the normal 0x800 skip path on every movie (not only boot/debug-allowed ones).
    const uint8_t nops[2] = {0x90, 0x90};
    if (!WriteBytes(allow_je, nops, 2, g_allow_je_original)) {
        LogWarn("Movie skip: failed to patch allow-check");
        g_orig_movie_input = nullptr;
        return false;
    }
    g_allow_je_site = allow_je;

    if (!WriteCall(site, reinterpret_cast<void*>(&ApMovieInputDetour), g_call_original)) {
        RestoreBytes(g_allow_je_site, g_allow_je_original, 2);
        g_allow_je_site = nullptr;
        g_orig_movie_input = nullptr;
        LogWarn("Movie skip: failed to patch call site");
        return false;
    }

    g_call_site = site;
    g_installed = true;
    g_skip_requested.store(false);
    LogInfo("Movie skip active (Select/Backspace → normal skip bit 0x%X)", kMovieSkipButtonBit);
    return true;
#endif
}

void RemoveMovieSkipHook() {
    if (g_call_site) {
        RestoreBytes(g_call_site, g_call_original, 5);
        g_call_site = nullptr;
    }
    if (g_allow_je_site) {
        RestoreBytes(g_allow_je_site, g_allow_je_original, 2);
        g_allow_je_site = nullptr;
    }
    g_orig_movie_input = nullptr;
    g_skip_requested.store(false);
    g_installed = false;
}

bool IsMovieSkipHookInstalled() {
    return g_installed;
}

void PollMovieSkipHotkey() {
    if (!g_installed) {
        return;
    }
    if (!IsMoviePlaying()) {
        g_select_was_down = GamepadSelectDown();
        g_key_was_down = (GetAsyncKeyState(VK_BACK) & 0x8000) != 0;
        return;
    }

    const bool select_down = GamepadSelectDown();
    const bool key_down = (GetAsyncKeyState(VK_BACK) & 0x8000) != 0;
    const bool select_edge = select_down && !g_select_was_down;
    const bool key_edge = key_down && !g_key_was_down;
    g_select_was_down = select_down;
    g_key_was_down = key_down;

    if (!select_edge && !key_edge) {
        return;
    }

    g_skip_requested.store(true);
    ShowD3dOverlayToast("Skipping cinematic…", 1200, 0x7CFC00u);
}

}  // namespace grandia_ap
