#pragma once

#include <cstdint>

namespace grandia_ap {

// Pattern scan and hooks derived from grandia.CT (DrummerIX).
// See data/cheat_engine_re.json and docs/cheat_engine_re.md.

bool InitializeGameMemory();
void ShutdownGameMemory();

bool HasStashBase();
std::uintptr_t GetStashBase();
// Resolve stash base from deposit global and/or stash UI hook capture.
bool EnsureStashBaseResolved();
// Lock in the global stash array pointer (called once from watcher).
bool AdoptStashBase(std::uintptr_t candidate, const char* reason);
unsigned GetStashHookHitCount();
std::uintptr_t GetStashHookLastEax();

// Quantity byte at stash_base + item_id - 1 (CE: [StashPtr] + selected - 1).
bool SafeReadByte(std::uintptr_t address, uint8_t* out_byte);
bool ReadStashByteAtOffset(int byte_offset, uint8_t* out_byte);
bool ReadStashQuantity(int item_id, uint8_t* out_quantity);
bool WriteStashQuantity(int item_id, uint8_t quantity);
// Prefer the game's stash deposit path (RE: grandia+0x44C3E); falls back to direct writes.
bool CallGameAddStashItem(int item_id, uint8_t quantity);
bool HasStashUiManager();
bool AddStashQuantity(int item_id, uint8_t delta);

// CE: GoldPtr from GetGoldPtrAOB (ESI); party gold dword at GoldPtr+4. Cap 9999999.
bool HasGoldBase();
std::uintptr_t GetGoldBase();
std::uintptr_t GetCharacterStatsBase();
bool AddGoldAmount(unsigned amount);
void FlushPendingGold();

bool IsPartyInventoryWriteHookInstalled();
bool IsChestFlagHookInstalled();
bool IsAssignUiEntryHookInstalled();
bool IsFieldGoldAddHookInstalled();
std::uintptr_t GetGrandiaModuleBase();

}  // namespace grandia_ap
