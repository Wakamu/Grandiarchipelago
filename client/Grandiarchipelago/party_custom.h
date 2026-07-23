#pragma once

#include <cstdint>
#include <cstddef>

namespace grandia_ap {

// Custom party override (F9/F10/F11 + PGR asset remap): parked for now.
// See kPartyCustomEnabled in party_custom.cpp.
bool InstallPartyCustomHook();
void RemovePartyCustomHook();
bool IsPartyCustomHookInstalled();

void PollPartyCustomHotkey();

bool IsPartyCustomEnabled();
void SetPartyCustomEnabled(bool enabled);

// ids[0..count-1] are character ids (1=Justin, 2=Feena, 3=Sue, …). count 1..4.
bool SetCustomParty(const uint8_t* ids, uint8_t count);

// Called from the shared fopen IAT hook. If a party asset remap applies, writes
// the replacement path into out_path and returns true.
bool TryPartyAssetOverlay(const char* original_path, char* out_path, size_t out_size);

}  // namespace grandia_ap
