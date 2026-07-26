#include "speed_turbo.h"

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
#include <mutex>
#include <vector>

namespace grandia_ap {

namespace {

constexpr int kHoldVk = VK_RCONTROL;
constexpr int kMaxSpeedLevel = 5;
constexpr int kHoldSpeedLevel = 2;  // RCtrl temporary boost when latched OFF

using QueryPerformanceCounter_t = BOOL(WINAPI*)(LARGE_INTEGER*);
using GetTickCount_t = DWORD(WINAPI*)();
using GetTickCount64_t = ULONGLONG(WINAPI*)();
using TimeGetTime_t = DWORD(WINAPI*)();
using Sleep_t = void(WINAPI*)(DWORD);
using SleepEx_t = DWORD(WINAPI*)(DWORD, BOOL);

QueryPerformanceCounter_t g_orig_qpc = nullptr;
GetTickCount_t g_orig_tick = nullptr;
GetTickCount64_t g_orig_tick64 = nullptr;
TimeGetTime_t g_orig_time_get_time = nullptr;
Sleep_t g_orig_sleep = nullptr;
SleepEx_t g_orig_sleep_ex = nullptr;

std::mutex g_warp_mu;
double g_speed = 1.0;
bool g_qpc_init = false;
bool g_tick_init = false;
bool g_tick64_init = false;
bool g_tgt_init = false;
LONGLONG g_last_real_qpc = 0;
LONGLONG g_last_fake_qpc = 0;
DWORD g_last_real_tick = 0;
DWORD g_last_fake_tick = 0;
ULONGLONG g_last_real_tick64 = 0;
ULONGLONG g_last_fake_tick64 = 0;
DWORD g_last_real_tgt = 0;
DWORD g_last_fake_tgt = 0;

std::atomic<int> g_effective_level{0};
int g_speed_level = 0;  // 0 = OFF, else 2..5
bool g_r1_was_down = false;
bool g_l1_was_down = false;
bool g_installed = false;

int ClampSpeedLevel(int level) {
    if (level <= 0) {
        return 0;
    }
    if (level < 2) {
        return 2;
    }
    if (level > kMaxSpeedLevel) {
        return kMaxSpeedLevel;
    }
    return level;
}

int RaiseSpeedLevel(int level) {
    if (level <= 0) {
        return 2;
    }
    return ClampSpeedLevel(level + 1);
}

int LowerSpeedLevel(int level) {
    if (level <= 2) {
        return 0;
    }
    return ClampSpeedLevel(level - 1);
}

void ToastSpeedLevel(int level, bool from_hold) {
    if (level <= 0) {
        ShowD3dOverlayToast("Turbo OFF", 1200, 0xFFE528u);
        return;
    }
    char buf[48];
    if (from_hold) {
        snprintf(buf, sizeof(buf), "Turbo %dx (RCtrl)", level);
    } else {
        snprintf(buf, sizeof(buf), "Turbo %dx", level);
    }
    ShowD3dOverlayToast(buf, 1500, 0x7CFC00u);
}

struct IatPatch {
    void** slot = nullptr;
    void* original = nullptr;
};

std::vector<IatPatch> g_patches;

LONGLONG AdvanceQpc(LONGLONG real_now) {
    std::lock_guard<std::mutex> lock(g_warp_mu);
    if (!g_qpc_init) {
        g_last_real_qpc = real_now;
        g_last_fake_qpc = real_now;
        g_qpc_init = true;
        return real_now;
    }
    const LONGLONG delta = real_now - g_last_real_qpc;
    g_last_real_qpc = real_now;
    g_last_fake_qpc += static_cast<LONGLONG>(static_cast<double>(delta) * g_speed);
    return g_last_fake_qpc;
}

DWORD AdvanceMs(DWORD real_now, bool* inited, DWORD* last_real, DWORD* last_fake) {
    std::lock_guard<std::mutex> lock(g_warp_mu);
    if (!*inited) {
        *last_real = real_now;
        *last_fake = real_now;
        *inited = true;
        return real_now;
    }
    const DWORD delta = real_now - *last_real;
    *last_real = real_now;
    *last_fake += static_cast<DWORD>(static_cast<double>(delta) * g_speed);
    return *last_fake;
}

ULONGLONG AdvanceMs64(ULONGLONG real_now) {
    std::lock_guard<std::mutex> lock(g_warp_mu);
    if (!g_tick64_init) {
        g_last_real_tick64 = real_now;
        g_last_fake_tick64 = real_now;
        g_tick64_init = true;
        return real_now;
    }
    const ULONGLONG delta = real_now - g_last_real_tick64;
    g_last_real_tick64 = real_now;
    g_last_fake_tick64 += static_cast<ULONGLONG>(static_cast<double>(delta) * g_speed);
    return g_last_fake_tick64;
}

void SetSpeedUnlocked(double speed) {
    if (speed < 0.1) {
        speed = 0.1;
    }
    if (speed > 10.0) {
        speed = 10.0;
    }
    g_speed = speed;
}

// Frame pacers use short Sleep; leave long waits alone (load/audio).
constexpr DWORD kMaxScaledSleepMs = 50;

DWORD ScaleSleepMs(DWORD ms) {
    if (ms == 0 || ms > kMaxScaledSleepMs) {
        return ms;
    }
    const int level = g_effective_level.load(std::memory_order_relaxed);
    if (level <= 1) {
        return ms;
    }
    const double scaled = static_cast<double>(ms) / static_cast<double>(level);
    if (scaled < 1.0) {
        return 0;
    }
    return static_cast<DWORD>(scaled + 0.5);
}

BOOL WINAPI HookQueryPerformanceCounter(LARGE_INTEGER* counter) {
    LARGE_INTEGER real{};
    if (!g_orig_qpc(&real)) {
        return FALSE;
    }
    if (counter) {
        counter->QuadPart = AdvanceQpc(real.QuadPart);
    }
    return TRUE;
}

DWORD WINAPI HookGetTickCount() {
    return AdvanceMs(g_orig_tick(), &g_tick_init, &g_last_real_tick, &g_last_fake_tick);
}

ULONGLONG WINAPI HookGetTickCount64() {
    return AdvanceMs64(g_orig_tick64());
}

DWORD WINAPI HookTimeGetTime() {
    return AdvanceMs(g_orig_time_get_time(), &g_tgt_init, &g_last_real_tgt, &g_last_fake_tgt);
}

void WINAPI HookSleep(DWORD ms) {
    if (g_orig_sleep) {
        g_orig_sleep(ScaleSleepMs(ms));
    }
}

DWORD WINAPI HookSleepEx(DWORD ms, BOOL alertable) {
    if (!g_orig_sleep_ex) {
        return 0;
    }
    return g_orig_sleep_ex(ScaleSleepMs(ms), alertable);
}

bool EqualsIgnoreCase(const char* a, const char* b) {
    if (!a || !b) {
        return false;
    }
    while (*a && *b) {
        const char ca = (*a >= 'A' && *a <= 'Z') ? static_cast<char>(*a - 'A' + 'a') : *a;
        const char cb = (*b >= 'A' && *b <= 'Z') ? static_cast<char>(*b - 'A' + 'a') : *b;
        if (ca != cb) {
            return false;
        }
        ++a;
        ++b;
    }
    return *a == *b;
}

bool IsTimeDll(const char* name) {
    return EqualsIgnoreCase(name, "kernel32.dll") || EqualsIgnoreCase(name, "kernelbase.dll") ||
           EqualsIgnoreCase(name, "winmm.dll");
}

void* HookForImport(const char* func_name) {
    if (EqualsIgnoreCase(func_name, "QueryPerformanceCounter")) {
        return reinterpret_cast<void*>(&HookQueryPerformanceCounter);
    }
    if (EqualsIgnoreCase(func_name, "GetTickCount")) {
        return reinterpret_cast<void*>(&HookGetTickCount);
    }
    if (EqualsIgnoreCase(func_name, "GetTickCount64")) {
        return reinterpret_cast<void*>(&HookGetTickCount64);
    }
    if (EqualsIgnoreCase(func_name, "timeGetTime")) {
        return reinterpret_cast<void*>(&HookTimeGetTime);
    }
    // Scale short sleeps with turbo so frame pacers don't pin effective speed near 2x.
    if (EqualsIgnoreCase(func_name, "Sleep")) {
        return reinterpret_cast<void*>(&HookSleep);
    }
    if (EqualsIgnoreCase(func_name, "SleepEx")) {
        return reinterpret_cast<void*>(&HookSleepEx);
    }
    return nullptr;
}

void CaptureOriginal(const char* func_name, void* original) {
    if (!original) {
        return;
    }
    if (EqualsIgnoreCase(func_name, "QueryPerformanceCounter") && !g_orig_qpc) {
        g_orig_qpc = reinterpret_cast<QueryPerformanceCounter_t>(original);
    } else if (EqualsIgnoreCase(func_name, "GetTickCount") && !g_orig_tick) {
        g_orig_tick = reinterpret_cast<GetTickCount_t>(original);
    } else if (EqualsIgnoreCase(func_name, "GetTickCount64") && !g_orig_tick64) {
        g_orig_tick64 = reinterpret_cast<GetTickCount64_t>(original);
    } else if (EqualsIgnoreCase(func_name, "timeGetTime") && !g_orig_time_get_time) {
        g_orig_time_get_time = reinterpret_cast<TimeGetTime_t>(original);
    } else if (EqualsIgnoreCase(func_name, "Sleep") && !g_orig_sleep) {
        g_orig_sleep = reinterpret_cast<Sleep_t>(original);
    } else if (EqualsIgnoreCase(func_name, "SleepEx") && !g_orig_sleep_ex) {
        g_orig_sleep_ex = reinterpret_cast<SleepEx_t>(original);
    }
}

int PatchModuleIat(HMODULE module) {
    if (!module) {
        return 0;
    }

    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(module);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) {
        return 0;
    }
    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(reinterpret_cast<uint8_t*>(module) + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) {
        return 0;
    }

    const auto& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (dir.VirtualAddress == 0 || dir.Size == 0) {
        return 0;
    }

    auto* imp = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(reinterpret_cast<uint8_t*>(module) +
                                                           dir.VirtualAddress);
    int patched = 0;

    for (; imp->Name != 0; ++imp) {
        const char* dll_name =
            reinterpret_cast<const char*>(reinterpret_cast<uint8_t*>(module) + imp->Name);
        if (!IsTimeDll(dll_name)) {
            continue;
        }

        auto* thunk = reinterpret_cast<IMAGE_THUNK_DATA*>(
            reinterpret_cast<uint8_t*>(module) +
            (imp->OriginalFirstThunk ? imp->OriginalFirstThunk : imp->FirstThunk));
        auto* iat = reinterpret_cast<IMAGE_THUNK_DATA*>(reinterpret_cast<uint8_t*>(module) +
                                                        imp->FirstThunk);

        for (; thunk->u1.AddressOfData != 0; ++thunk, ++iat) {
            if (thunk->u1.Ordinal & IMAGE_ORDINAL_FLAG) {
                continue;
            }
            auto* ibn = reinterpret_cast<IMAGE_IMPORT_BY_NAME*>(reinterpret_cast<uint8_t*>(module) +
                                                               thunk->u1.AddressOfData);
            const char* func_name = reinterpret_cast<const char*>(ibn->Name);
            void* hook = HookForImport(func_name);
            if (!hook) {
                continue;
            }

            void** slot = reinterpret_cast<void**>(&iat->u1.Function);
            void* current = *slot;
            if (current == hook) {
                continue;
            }

            CaptureOriginal(func_name, current);

            DWORD old_protect = 0;
            if (!VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &old_protect)) {
                continue;
            }
            *slot = hook;
            VirtualProtect(slot, sizeof(void*), old_protect, &old_protect);

            g_patches.push_back(IatPatch{slot, current});
            ++patched;
            LogInfo("Speed turbo IAT: %s!%s", dll_name, func_name);
        }
    }
    return patched;
}

bool ResolveFallbacks() {
    HMODULE kernel32 = GetModuleHandleW(L"kernel32.dll");
    HMODULE winmm = LoadLibraryW(L"winmm.dll");
    if (!kernel32) {
        return false;
    }
    if (!g_orig_qpc) {
        g_orig_qpc = reinterpret_cast<QueryPerformanceCounter_t>(
            GetProcAddress(kernel32, "QueryPerformanceCounter"));
    }
    if (!g_orig_tick) {
        g_orig_tick =
            reinterpret_cast<GetTickCount_t>(GetProcAddress(kernel32, "GetTickCount"));
    }
    if (!g_orig_tick64) {
        g_orig_tick64 =
            reinterpret_cast<GetTickCount64_t>(GetProcAddress(kernel32, "GetTickCount64"));
    }
    if (!g_orig_sleep) {
        g_orig_sleep = reinterpret_cast<Sleep_t>(GetProcAddress(kernel32, "Sleep"));
    }
    if (!g_orig_sleep_ex) {
        g_orig_sleep_ex = reinterpret_cast<SleepEx_t>(GetProcAddress(kernel32, "SleepEx"));
    }
    if (!g_orig_time_get_time && winmm) {
        g_orig_time_get_time =
            reinterpret_cast<TimeGetTime_t>(GetProcAddress(winmm, "timeGetTime"));
    }
    return g_orig_qpc != nullptr;
}

}  // namespace

bool InstallSpeedTurbo() {
#if !defined(_M_IX86)
    return false;
#else
    if (g_installed) {
        return true;
    }

    g_patches.clear();

    // Timing: grandia QPC + MSVCP _Query_perf_* (via msvcp QPC) + SDL2 timers.
    // Do not touch D3D/DXGI/XAudio — that crashed earlier.
    const wchar_t* targets[] = {L"grandia.exe", L"SDL2.dll", L"msvcp140.dll"};
    int total = 0;
    for (const wchar_t* name : targets) {
        HMODULE mod = GetModuleHandleW(name);
        if (!mod && _wcsicmp(name, L"grandia.exe") == 0) {
            const std::uintptr_t base = GetGrandiaModuleBase();
            mod = base ? reinterpret_cast<HMODULE>(base) : nullptr;
        }
        if (!mod) {
            LogWarn("Speed turbo: module %ls not loaded yet", name);
            continue;
        }
        const int n = PatchModuleIat(mod);
        total += n;
        LogInfo("Speed turbo: %ls patched %d import(s)", name, n);
    }

    if (!ResolveFallbacks()) {
        RemoveSpeedTurbo();
        LogWarn("Speed turbo: failed to resolve original QPC");
        return false;
    }

    if (total == 0) {
        LogWarn("Speed turbo: no IAT slots patched");
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(g_warp_mu);
        SetSpeedUnlocked(1.0);
        g_qpc_init = false;
        g_tick_init = false;
        g_tick64_init = false;
        g_tgt_init = false;
    }

    g_installed = true;
    g_speed_level = 0;
    g_effective_level.store(0);
    g_r1_was_down = false;
    g_l1_was_down = false;
    LogInfo("Speed turbo ready — Select+R1/L1 step 2x..5x / RCtrl holds %dx (%d IAT hooks)",
            kHoldSpeedLevel, total);
    return true;
#endif
}

void RemoveSpeedTurbo() {
    {
        std::lock_guard<std::mutex> lock(g_warp_mu);
        SetSpeedUnlocked(1.0);
    }
    g_effective_level.store(0);
    g_speed_level = 0;
    g_r1_was_down = false;
    g_l1_was_down = false;

    for (auto it = g_patches.rbegin(); it != g_patches.rend(); ++it) {
        if (!it->slot) {
            continue;
        }
        DWORD old_protect = 0;
        if (VirtualProtect(it->slot, sizeof(void*), PAGE_READWRITE, &old_protect)) {
            *it->slot = it->original;
            VirtualProtect(it->slot, sizeof(void*), old_protect, &old_protect);
        }
    }
    g_patches.clear();

    g_orig_qpc = nullptr;
    g_orig_tick = nullptr;
    g_orig_tick64 = nullptr;
    g_orig_time_get_time = nullptr;
    g_orig_sleep = nullptr;
    g_orig_sleep_ex = nullptr;
    g_installed = false;
}

bool IsSpeedTurboInstalled() {
    return g_installed;
}

int GetSpeedTurboLevel() {
    return g_effective_level.load(std::memory_order_relaxed);
}

void PaceSpeedTurboFrame() {
    const int level = GetSpeedTurboLevel();
    if (level < 2) {
        return;
    }

    static LONGLONG freq = 0;
    static LONGLONG next_deadline = 0;
    LARGE_INTEGER qpc{};
    if (freq == 0) {
        LARGE_INTEGER f{};
        if (!QueryPerformanceFrequency(&f) || f.QuadPart <= 0) {
            return;
        }
        freq = f.QuadPart;
    }
    if (!QueryPerformanceCounter(&qpc)) {
        return;
    }

    const LONGLONG period = freq / (60LL * level);
    if (period <= 0) {
        return;
    }

    if (next_deadline == 0) {
        next_deadline = qpc.QuadPart + period;
        return;
    }

    // Fell too far behind — resync instead of spiral-sleeping.
    if (qpc.QuadPart > next_deadline + period * 2) {
        next_deadline = qpc.QuadPart + period;
        return;
    }

    while (qpc.QuadPart < next_deadline) {
        const LONGLONG remain = next_deadline - qpc.QuadPart;
        const DWORD sleep_ms = static_cast<DWORD>((remain * 1000) / freq);
        if (sleep_ms > 1) {
            Sleep(sleep_ms - 1);
        }
        if (!QueryPerformanceCounter(&qpc)) {
            break;
        }
    }
    next_deadline += period;
}

bool GamepadSelectShoulderDown(WORD shoulder) {
    for (DWORD i = 0; i < XUSER_MAX_COUNT; ++i) {
        XINPUT_STATE state{};
        if (XInputGetStateDyn(i, &state) != ERROR_SUCCESS) {
            continue;
        }
        const WORD buttons = state.Gamepad.wButtons;
        if ((buttons & XINPUT_GAMEPAD_BACK) && (buttons & shoulder)) {
            return true;
        }
    }
    return false;
}

void PollSpeedTurboHotkey() {
    if (!g_installed) {
        return;
    }

    // Select+R1 raises / Select+L1 lowers; skipped while a movie is up (Select alone skips movies).
    const bool r1_down = GamepadSelectShoulderDown(XINPUT_GAMEPAD_RIGHT_SHOULDER);
    const bool l1_down = GamepadSelectShoulderDown(XINPUT_GAMEPAD_LEFT_SHOULDER);
    bool pad_stepped = false;
    if (!IsMoviePlaying()) {
        if (r1_down && !g_r1_was_down) {
            g_speed_level = RaiseSpeedLevel(g_speed_level);
            ToastSpeedLevel(g_speed_level, false);
            pad_stepped = true;
        }
        if (l1_down && !g_l1_was_down) {
            g_speed_level = LowerSpeedLevel(g_speed_level);
            ToastSpeedLevel(g_speed_level, false);
            pad_stepped = true;
        }
    }
    g_r1_was_down = r1_down;
    g_l1_was_down = l1_down;

    const bool held = (GetAsyncKeyState(kHoldVk) & 0x8000) != 0;
    int effective = g_speed_level;
    bool from_hold = false;
    if (effective <= 0 && held) {
        effective = kHoldSpeedLevel;
        from_hold = true;
    }

    {
        std::lock_guard<std::mutex> lock(g_warp_mu);
        SetSpeedUnlocked(effective > 0 ? static_cast<double>(effective) : 1.0);
    }

    const int was = g_effective_level.exchange(effective);
    if (effective == was || pad_stepped) {
        return;
    }
    // Announce RCtrl hold on/off only (pad steps already toasted).
    ToastSpeedLevel(effective, from_hold);
}

}  // namespace grandia_ap
