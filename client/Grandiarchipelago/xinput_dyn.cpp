#include "xinput_dyn.h"

namespace grandia_ap {
namespace {

using XInputGetState_t = DWORD(WINAPI*)(DWORD, XINPUT_STATE*);

XInputGetState_t g_xinput_get_state = nullptr;
bool g_xinput_resolved = false;

void ResolveXInput() {
    if (g_xinput_resolved) {
        return;
    }
    g_xinput_resolved = true;

    const wchar_t* candidates[] = {
        L"xinput1_4.dll",
        L"xinput1_3.dll",
        L"xinput9_1_0.dll",
    };
    for (const wchar_t* name : candidates) {
        HMODULE mod = LoadLibraryW(name);
        if (!mod) {
            continue;
        }
        auto* fn = reinterpret_cast<XInputGetState_t>(GetProcAddress(mod, "XInputGetState"));
        if (fn) {
            g_xinput_get_state = fn;
            return;
        }
    }
}

}  // namespace

DWORD XInputGetStateDyn(DWORD user_index, XINPUT_STATE* state) {
    ResolveXInput();
    if (!g_xinput_get_state || !state) {
        return ERROR_DEVICE_NOT_CONNECTED;
    }
    return g_xinput_get_state(user_index, state);
}

}  // namespace grandia_ap
