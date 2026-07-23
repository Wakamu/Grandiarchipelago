#include "chest_pickup.h"
#include "debug_mode.h"
#include "map_overview.h"
#include "game_memory.h"
#include "log.h"
#include "m_dat_balance.h"
#include "movie_skip.h"
#include "party_custom.h"
#include "save_sync.h"
#include "speed_turbo.h"
#include "windt_balance.h"

#include <Windows.h>

#include <array>
#include <atomic>
#include <cstdio>

namespace grandia_ap {

namespace {

constexpr int kStashByteCount = 512;
constexpr int kStashFirstOffset = 8;

std::atomic<bool> g_running{false};
HANDLE g_thread = nullptr;
std::array<uint8_t, kStashByteCount> g_last_bytes{};
bool g_have_snapshot = false;

void ResnapshotStash(bool log_summary) {
    const std::uintptr_t base = GetStashBase();
    if (base == 0) {
        return;
    }

    unsigned nonzero = 0;
    for (int offset = 0; offset < kStashByteCount; ++offset) {
        uint8_t value = 0;
        if (ReadStashByteAtOffset(offset, &value)) {
            g_last_bytes[static_cast<size_t>(offset)] = value;
            if (value != 0) {
                ++nonzero;
            }
        } else {
            g_last_bytes[static_cast<size_t>(offset)] = 0;
        }
    }
    g_have_snapshot = true;

    if (log_summary) {
        LogInfo("Stash snapshot at 0x%08X (%u non-zero bytes)", static_cast<unsigned>(base), nonzero);
    }
}

void PollStashChanges() {
    if (!HasStashBase()) {
        return;
    }

    if (!g_have_snapshot) {
        ResnapshotStash(false);
        return;
    }

    for (int offset = kStashFirstOffset; offset < kStashByteCount; ++offset) {
        uint8_t value = 0;
        if (!ReadStashByteAtOffset(offset, &value)) {
            continue;
        }

        const uint8_t previous = g_last_bytes[static_cast<size_t>(offset)];
        if (value == previous) {
            continue;
        }

        g_last_bytes[static_cast<size_t>(offset)] = value;
    }
}

void OnStashHookHit(unsigned /*hits*/) {
    const std::uintptr_t eax = GetStashHookLastEax();

    if (AdoptStashBase(eax, "stash UI hook") && !g_have_snapshot) {
        ResnapshotStash(true);
    }
}

DWORD WINAPI WatcherThread(LPVOID) {
    LogInfo("Stash watcher started — tracking stash quantities when base is resolved");

    unsigned last_hook_hits = 0;
    unsigned heartbeat = 0;

    while (g_running.load()) {
        ProcessChestPickupQueue();
        FlushPendingGold();
        PollDebugModeHotkey();
        PollMapOverviewHotkey();
        PollMovieSkipHotkey();
        PollSpeedTurboHotkey();
        PollPartyCustomHotkey();

        // File-overlay Redux: WINDT reloads already come from redux_content via fopen.
        // Only poll until first healthy spot-check (finalize/sell hooks re-check on use).
        if (GetGameplayBalance() == 1 && !IsWindtSec3ReduxApplied()) {
            TryApplyWindtSec3Redux();
        }

        const unsigned hits = GetStashHookHitCount();
        if (hits != last_hook_hits) {
            OnStashHookHit(hits);
            last_hook_hits = hits;
        }

        if (!HasStashBase()) {
            EnsureStashBaseResolved();
        }

        if (HasStashBase()) {
            if (!g_have_snapshot) {
                ResnapshotStash(true);
            }
            PollStashChanges();
        } else if ((heartbeat++ % 40) == 0) {
            LogInfo("Waiting for stash base (need in-game save loaded; hook hits: %u)...", hits);
        }

        Sleep(100);
    }

    return 0;
}

}  // namespace

void StartStashWatcher() {
    if (g_running.exchange(true)) {
        return;
    }
    g_thread = CreateThread(nullptr, 0, WatcherThread, nullptr, 0, nullptr);
}

void StopStashWatcher() {
    if (!g_running.exchange(false)) {
        return;
    }
    if (g_thread) {
        WaitForSingleObject(g_thread, 2000);
        CloseHandle(g_thread);
        g_thread = nullptr;
    }
}

}  // namespace grandia_ap
