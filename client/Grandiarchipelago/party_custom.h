#pragma once

#include <cstdint>
#include <cstddef>

namespace grandia_ap {

// Menu/battle roster override (Save MC Party tab in unlocks mode).
// On apply: roster()+save IDs, seed char blocks, rebuild menu cache.
// Field PGR remap is OFF — PGR does not drive battle party (swap test).
// See kPartyCustomEnabled / kPartyAssetRemapEnabled in party_custom.cpp.
bool InstallPartyCustomHook();
void RemovePartyCustomHook();
bool IsPartyCustomHookInstalled();

// Keeps battle mode polling; hotkey edit removed (use Save Party overlay).
void PollPartyCustomHotkey();

bool IsPartyCustomEnabled();
void SetPartyCustomEnabled(bool enabled);

// custom_party CONFIG: 0=vanilla, 1=roulette, 2=unlocks.
constexpr unsigned kCustomPartyVanilla = 0;
constexpr unsigned kCustomPartyRoulette = 1;
constexpr unsigned kCustomPartyUnlocks = 2;

void ApplyCustomPartyConfig(unsigned mode);
// Roulette roster: four native char ids (1..8). Call after ApplyCustomPartyConfig(roulette).
void ApplyCustomPartyRoster(const uint8_t* ids, uint8_t count);

unsigned GetCustomPartyMode();
bool IsPartyUnlocksMode();
bool IsPartyRouletteMode();
// Save MC Party overlay — unlocks mode only.
bool IsPartyOverlayEnabled();

// Character unlocks: AP item id = 0x47525000 + native char id (2..8). Justin (1) always on.
constexpr unsigned kCharacterItemBase = 0x47525000u;
bool TryHandleCharacterUnlockItem(unsigned ap_item_id);
void ClearCharacterUnlockState();
bool IsCharacterUnlocked(uint8_t char_id);
uint8_t UnlockedCharacterCount();
// Max party slots: min(4, unlocked) in unlocks mode, else 4.
uint8_t PartyMaxSlots();

// Restore roster from GAP1 on load (marks AP party initialized so CONFIG won't reset).
void ApplySavedCustomParty(const uint8_t* ids, uint8_t count);

// ids[0..count-1] are character ids (1=Justin, 2=Feena, 3=Sue, …). count 1..4.
bool SetCustomParty(const uint8_t* ids, uint8_t count);

// Save-menu Party tab helpers (edit buffer + apply).
void FormatPartyRosterLine(char* buf, size_t buf_size);
const char* PartyCharName(uint8_t id);
uint8_t PartyEditCount();
uint8_t PartyEditIdAt(uint8_t slot);
uint8_t PartyEditSelectedSlot();
void PartyEditSetSelectedSlot(uint8_t slot);
void PartyUiEnsureEnabled();
void PartyUiCycleSlot();
void PartyUiCycleChar(int dir = 1);
void PartyUiAddMember();
void PartyUiRemoveMember();
bool PartyUiApply();
// Quiet variants (no toast) for Save Party center panel.
void PartyUiCycleCharQuiet(int dir);
void PartyUiAddMemberQuiet();
void PartyUiRemoveMemberQuiet();
bool PartyUiApplyQuiet();

// Called from the shared fopen IAT hook. If a party asset remap applies, writes
// the replacement path into out_path and returns true.
bool TryPartyAssetOverlay(const char* original_path, char* out_path, size_t out_size);

// Called from D3D Present: pack custom-party bags only (no +1DD6B0 — unsafe in field pickup UI).
void PollPartyInventoryUiFix();

}  // namespace grandia_ap
