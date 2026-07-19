#include "pipe_bridge.h"

#include "chest_pickup.h"
#include "d3d_overlay.h"
#include "item_tracker.h"
#include "log.h"
#include "save_sync.h"
#include "xp_multiplier.h"

#include <Windows.h>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <queue>
#include <string>
#include <thread>

namespace grandia_ap {

namespace {

constexpr wchar_t kPipeName[] = L"\\\\.\\pipe\\Grandiarchipelago";

std::atomic<bool> g_running{false};
std::thread g_pipe_thread;
HANDLE g_pipe = INVALID_HANDLE_VALUE;
std::mutex g_io_mutex;
std::queue<unsigned> g_pending_checks;
std::queue<std::string> g_pending_lockouts;
std::mutex g_check_mutex;
std::mutex g_sync_mutex;
bool g_have_sync = false;
unsigned g_pending_sync_index = 0;
bool g_sync_dirty = false;
bool g_sync_sent = false;
unsigned g_last_sent_sync_index = 0;

bool WriteLineLocked(HANDLE pipe, const std::string& line) {
    std::string payload = line;
    payload.push_back('\n');
    DWORD written = 0;
    const BOOL ok = WriteFile(pipe, payload.data(), static_cast<DWORD>(payload.size()), &written, nullptr);
    if (!ok) {
        LogWarn("Pipe WriteFile failed: %lu", GetLastError());
    }
    return ok != FALSE;
}

bool ReadLineLocked(HANDLE pipe, std::string& out_line) {
    out_line.clear();
    char ch = 0;
    DWORD read = 0;
    while (ReadFile(pipe, &ch, 1, &read, nullptr) && read == 1) {
        if (ch == '\n') {
            return true;
        }
        if (ch != '\r') {
            out_line.push_back(ch);
        }
    }

    if (read == 0) {
        const DWORD err = GetLastError();
        if (err != ERROR_SUCCESS && err != ERROR_NO_DATA) {
            LogWarn("Pipe ReadFile ended: %lu", err);
        }
    }
    return !out_line.empty();
}

bool ParseItemLine(const std::string& line, unsigned* out_item_id, unsigned* out_index,
                   bool* out_have_index) {
    if (!out_item_id || !out_have_index) {
        return false;
    }
    *out_have_index = false;
    if (out_index) {
        *out_index = 0;
    }

    const std::string prefix = "ITEM ";
    if (line.rfind(prefix, 0) != 0) {
        return false;
    }

    const char* token = line.c_str() + prefix.size();
    while (*token == ' ') {
        ++token;
    }

    char* end = nullptr;
    const unsigned long item_id = strtoul(token, &end, 0);
    if (end == token) {
        return false;
    }
    *out_item_id = static_cast<unsigned>(item_id);

    while (*end == ' ') {
        ++end;
    }
    if (_strnicmp(end, "INDEX", 5) == 0) {
        end += 5;
        while (*end == ' ') {
            ++end;
        }
        char* index_end = nullptr;
        const unsigned long index = strtoul(end, &index_end, 0);
        if (index_end != end && out_index) {
            *out_index = static_cast<unsigned>(index);
            *out_have_index = true;
        }
    }
    return true;
}

void HandleBridgeLine(const std::string& line) {
    if (line.rfind("ITEM ", 0) == 0) {
        unsigned item_id = 0;
        unsigned index = 0;
        bool have_index = false;
        if (ParseItemLine(line, &item_id, &index, &have_index) && item_id != 0) {
            if (have_index) {
                LogInfo("Bridge delivered AP item 0x%08X (Index=%u)", item_id, index);
                SetSaveSyncReceivedIndex(index);
            } else {
                LogInfo("Bridge delivered AP item 0x%08X", item_id);
            }
            GetItemTracker().EnqueueReceivedItem(item_id, nullptr);
        } else {
            LogWarn("Could not parse ITEM line: %s", line.c_str());
        }
        return;
    }

    if (line.rfind("TOAST ", 0) == 0) {
        const char* rest = line.c_str() + 6;
        while (*rest == ' ') {
            ++rest;
        }
        unsigned rgb = 0xFFE528;
        if (rest[0] == '#' && std::strlen(rest) >= 7) {
            char hex[7] = {};
            std::memcpy(hex, rest + 1, 6);
            char* end = nullptr;
            const unsigned long parsed = std::strtoul(hex, &end, 16);
            if (end && end == hex + 6) {
                rgb = static_cast<unsigned>(parsed);
                rest += 7;
                while (*rest == ' ') {
                    ++rest;
                }
            }
        }
        if (*rest) {
            ShowD3dOverlayToast(rest, 6000, rgb);
        }
        return;
    }

    if (line == "TOAST_CLEAR") {
        ClearD3dOverlayToast();
        return;
    }

    if (line == "CONNECTED") {
        LogInfo("Archipelago bridge connected to server");
        return;
    }

    if (line.rfind("CONFIG include_gold_chests ", 0) == 0) {
        const char* rest = line.c_str() + std::strlen("CONFIG include_gold_chests ");
        while (*rest == ' ') {
            ++rest;
        }
        const unsigned value = static_cast<unsigned>(std::strtoul(rest, nullptr, 10));
        SetIncludeGoldChests(value != 0);
        return;
    }

    if (line.rfind("CONFIG include_soldiers_graveyard ", 0) == 0) {
        const char* rest = line.c_str() + std::strlen("CONFIG include_soldiers_graveyard ");
        while (*rest == ' ') {
            ++rest;
        }
        const unsigned value = static_cast<unsigned>(std::strtoul(rest, nullptr, 10));
        SetIncludeSoldiersGraveyard(value != 0);
        return;
    }

    if (line.rfind("CONFIG include_castle_of_dreams ", 0) == 0) {
        const char* rest = line.c_str() + std::strlen("CONFIG include_castle_of_dreams ");
        while (*rest == ' ') {
            ++rest;
        }
        const unsigned value = static_cast<unsigned>(std::strtoul(rest, nullptr, 10));
        SetIncludeCastleOfDreams(value != 0);
        return;
    }

    if (line.rfind("CONFIG include_tower_of_temptation ", 0) == 0) {
        const char* rest = line.c_str() + std::strlen("CONFIG include_tower_of_temptation ");
        while (*rest == ' ') {
            ++rest;
        }
        const unsigned value = static_cast<unsigned>(std::strtoul(rest, nullptr, 10));
        SetIncludeTowerOfTemptation(value != 0);
        return;
    }

    if (line.rfind("CONFIG magic_xp_multiplier ", 0) == 0) {
        const char* rest = line.c_str() + std::strlen("CONFIG magic_xp_multiplier ");
        while (*rest == ' ') {
            ++rest;
        }
        const unsigned value = static_cast<unsigned>(std::strtoul(rest, nullptr, 10));
        SetMagicXpMultiplier(value);
        return;
    }

    if (line.rfind("CONFIG skill_xp_multiplier ", 0) == 0) {
        const char* rest = line.c_str() + std::strlen("CONFIG skill_xp_multiplier ");
        while (*rest == ' ') {
            ++rest;
        }
        const unsigned value = static_cast<unsigned>(std::strtoul(rest, nullptr, 10));
        SetSkillXpMultiplier(value);
        return;
    }

    if (line.rfind("CONFIG level_xp_multiplier ", 0) == 0) {
        const char* rest = line.c_str() + std::strlen("CONFIG level_xp_multiplier ");
        while (*rest == ' ') {
            ++rest;
        }
        const unsigned value = static_cast<unsigned>(std::strtoul(rest, nullptr, 10));
        SetLevelXpMultiplier(value);
        return;
    }

    if (line.rfind("CONFIG ", 0) == 0) {
        LogWarn("Unrecognized CONFIG line: %s", line.c_str());
        return;
    }

    if (line.rfind("DISCONNECTED", 0) == 0) {
        LogWarn("Archipelago bridge disconnected: %s", line.c_str());
        return;
    }

    if (line.rfind("LOG ", 0) == 0) {
        LogInfo("[Bridge] %s", line.c_str() + 4);
    }
}

void FlushPendingSyncLocked(HANDLE pipe) {
    unsigned index = 0;
    bool send = false;
    {
        std::lock_guard<std::mutex> lock(g_sync_mutex);
        if (g_have_sync && g_sync_dirty) {
            index = g_pending_sync_index;
            send = true;
            g_sync_dirty = false;
        }
    }
    if (!send) {
        return;
    }

    char buffer[64];
    snprintf(buffer, sizeof(buffer), "SYNC %u", index);
    if (!WriteLineLocked(pipe, buffer)) {
        std::lock_guard<std::mutex> lock(g_sync_mutex);
        g_sync_dirty = true;
        LogWarn("Failed to send SYNC %u", index);
        return;
    }
    LogInfo("Sent SYNC to bridge: received_index=%u", index);
    {
        std::lock_guard<std::mutex> lock(g_sync_mutex);
        g_sync_sent = true;
        g_last_sent_sync_index = index;
    }
}

void FlushPendingChecksLocked(HANDLE pipe) {
    FlushPendingSyncLocked(pipe);

    std::queue<unsigned> local_checks;
    std::queue<std::string> local_lockouts;
    {
        std::lock_guard<std::mutex> lock(g_check_mutex);
        local_checks.swap(g_pending_checks);
        local_lockouts.swap(g_pending_lockouts);
    }

    while (!local_checks.empty()) {
        const unsigned location_id = local_checks.front();
        local_checks.pop();
        char buffer[64];
        snprintf(buffer, sizeof(buffer), "CHECK %08X", location_id);
        if (!WriteLineLocked(pipe, buffer)) {
            LogWarn("Failed to send pending check 0x%08X", location_id);
            std::lock_guard<std::mutex> requeue_lock(g_check_mutex);
            g_pending_checks.push(location_id);
            while (!local_checks.empty()) {
                g_pending_checks.push(local_checks.front());
                local_checks.pop();
            }
            while (!local_lockouts.empty()) {
                g_pending_lockouts.push(local_lockouts.front());
                local_lockouts.pop();
            }
            return;
        }
        LogInfo("Sent check to bridge: 0x%08X", location_id);
    }

    while (!local_lockouts.empty()) {
        const std::string line = local_lockouts.front();
        local_lockouts.pop();
        if (!WriteLineLocked(pipe, line)) {
            LogWarn("Failed to send pending lockout line");
            std::lock_guard<std::mutex> requeue_lock(g_check_mutex);
            g_pending_lockouts.push(line);
            while (!local_lockouts.empty()) {
                g_pending_lockouts.push(local_lockouts.front());
                local_lockouts.pop();
            }
            return;
        }
        LogInfo("Sent lockout sweep request to bridge (%u chars)", static_cast<unsigned>(line.size()));
    }
}

void PipeThreadMain() {
    LogInfo("Pipe bridge waiting for Grandiarchipelago.Bridge on \\\\.\\pipe\\Grandiarchipelago");

    while (g_running.load()) {
        HANDLE pipe = CreateNamedPipeW(kPipeName, PIPE_ACCESS_DUPLEX,
                                       PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT, 1, 65536, 65536, 0,
                                       nullptr);
        if (pipe == INVALID_HANDLE_VALUE) {
            LogWarn("CreateNamedPipe failed: %lu", GetLastError());
            Sleep(1000);
            continue;
        }

        const BOOL connected = ConnectNamedPipe(pipe, nullptr) ? TRUE : (GetLastError() == ERROR_PIPE_CONNECTED);
        if (!connected) {
            CloseHandle(pipe);
            Sleep(500);
            continue;
        }

        {
            std::lock_guard<std::mutex> lock(g_io_mutex);
            g_pipe = pipe;
        }

        LogInfo("Bridge process connected to game pipe");
        {
            std::lock_guard<std::mutex> lock(g_sync_mutex);
            if (g_have_sync) {
                g_sync_dirty = true;  // re-announce for reconnecting bridge
            }
        }
        WriteLineLocked(pipe, "HELLO game");
        FlushPendingChecksLocked(pipe);

        while (g_running.load()) {
            FlushPendingChecksLocked(pipe);

            std::string line;
            bool have_line = false;
            {
                std::lock_guard<std::mutex> lock(g_io_mutex);
                if (g_pipe == INVALID_HANDLE_VALUE) {
                    break;
                }

                DWORD available = 0;
                if (!PeekNamedPipe(g_pipe, nullptr, 0, nullptr, &available, nullptr)) {
                    LogWarn("PeekNamedPipe failed: %lu", GetLastError());
                    break;
                }

                if (available > 0) {
                    have_line = ReadLineLocked(g_pipe, line);
                    if (!have_line && available > 0) {
                        LogWarn("Pipe read failed with %lu bytes pending", available);
                        break;
                    }
                }
            }

            if (have_line) {
                HandleBridgeLine(line);
            } else {
                Sleep(25);
            }
        }

        LogInfo("Bridge disconnected from game pipe");
        {
            std::lock_guard<std::mutex> lock(g_io_mutex);
            if (g_pipe != INVALID_HANDLE_VALUE) {
                DisconnectNamedPipe(g_pipe);
                CloseHandle(g_pipe);
                g_pipe = INVALID_HANDLE_VALUE;
            }
        }
    }
}

}  // namespace

void StartPipeBridge() {
    if (g_running.exchange(true)) {
        return;
    }
    g_pipe_thread = std::thread(PipeThreadMain);
}

void StopPipeBridge() {
    if (!g_running.exchange(false)) {
        return;
    }

    {
        std::lock_guard<std::mutex> lock(g_io_mutex);
        if (g_pipe != INVALID_HANDLE_VALUE) {
            CancelIoEx(g_pipe, nullptr);
            DisconnectNamedPipe(g_pipe);
            CloseHandle(g_pipe);
            g_pipe = INVALID_HANDLE_VALUE;
        }
    }

    if (g_pipe_thread.joinable()) {
        g_pipe_thread.join();
    }
}

void PipeEnqueueLocationCheck(unsigned location_id) {
    std::lock_guard<std::mutex> lock(g_check_mutex);
    g_pending_checks.push(location_id);
}

void PipeEnqueueLockoutMessage(const std::string& message) {
    if (message.empty()) {
        return;
    }
    std::lock_guard<std::mutex> lock(g_check_mutex);
    g_pending_lockouts.push(message);
}

void PipeEnqueueSync(unsigned received_index, bool force) {
    std::lock_guard<std::mutex> lock(g_sync_mutex);
    if (!force && g_sync_sent && g_last_sent_sync_index == received_index && !g_sync_dirty) {
        return;
    }
    if (!force && g_have_sync && g_pending_sync_index == received_index && g_sync_dirty) {
        return;
    }
    g_have_sync = true;
    g_pending_sync_index = received_index;
    g_sync_dirty = true;
}

void PipePoll() {
    // Reads happen on the pipe thread.
}

}  // namespace grandia_ap
