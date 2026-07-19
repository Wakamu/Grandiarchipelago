#include "ap_session.h"
#include "d3d_overlay.h"
#include "game_memory.h"
#include "hooks.h"
#include "log.h"
#include "pipe_bridge.h"
#include "stash_watcher.h"

#include <Windows.h>

namespace {

DWORD WINAPI MainThread(LPVOID) {
    __try {
        grandia_ap::LogInfo("MainThread started (pid=%lu)", GetCurrentProcessId());
        Sleep(500);

        if (!grandia_ap::InitializeGameMemory()) {
            grandia_ap::LogWarn("No CE AOB patterns matched — hooks not installed");
        }

        grandia_ap::InstallHooks();
        if (!grandia_ap::InstallD3dOverlay()) {
            grandia_ap::LogWarn("D3D11 overlay test not installed");
        }
        grandia_ap::StartStashWatcher();
        grandia_ap::StartPipeBridge();
        grandia_ap::LogInfo("Grandiarchipelago v0.0.44 ready (optional dungeon options)");
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        grandia_ap::LogWarn("MainThread crashed during init (exception=0x%08X)", GetExceptionCode());
    }
    return 0;
}

}  // namespace

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID) {
    switch (reason) {
        case DLL_PROCESS_ATTACH:
            DisableThreadLibraryCalls(module);
            grandia_ap::InitializeLogging();
            grandia_ap::LogInfo("DllMain PROCESS_ATTACH (module=0x%p)", module);
            if (!CreateThread(nullptr, 0, MainThread, nullptr, 0, nullptr)) {
                grandia_ap::LogWarn("CreateThread failed: %lu", GetLastError());
            }
            break;
        case DLL_PROCESS_DETACH:
            grandia_ap::StopPipeBridge();
            grandia_ap::StopStashWatcher();
            grandia_ap::ShutdownD3dOverlay();
            grandia_ap::RemoveHooks();
            grandia_ap::ShutdownGameMemory();
            grandia_ap::LogInfo("DllMain PROCESS_DETACH");
            grandia_ap::ShutdownLogging();
            break;
        default:
            break;
    }
    return TRUE;
}
