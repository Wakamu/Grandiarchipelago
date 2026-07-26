#pragma once

#include <Windows.h>
#include <Xinput.h>

namespace grandia_ap {

// Resolve XInputGetState at runtime (xinput1_4 → 1_3 → 9_1_0).
// Avoids a hard import of XINPUT9_1_0.dll, which makes LoadLibrary fail on
// machines that lack the legacy DirectX XInput redistributable.
DWORD XInputGetStateDyn(DWORD user_index, XINPUT_STATE* state);

}  // namespace grandia_ap
