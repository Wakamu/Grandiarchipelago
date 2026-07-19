#pragma once

#include <cstdint>

namespace grandia_ap {

// Field chest / story event-flag hook (grandia.exe+0x70505).
// Chest loot checks: caller +0x53C45. Story checks: +0x6F03F, +0x7CB4F.

bool IsChestPickupHookInstalled();
bool IsAssignUiHookInstalled();

// True while kLogAllEventFlagWrites is enabled in chest_pickup.cpp (RE mode).
bool IsEventFlagTraceEnabled();

void QueueChestEventPickup(unsigned event_id, unsigned flag_offset, unsigned flag_value, unsigned mask,
                           unsigned ecx_index, std::uintptr_t save_base, std::uintptr_t caller);
void ProcessChestPickupQueue();

// Returns 1 to skip assign UI, 0 to run vanilla handler.
int TryInterceptChestAssignUi(std::uintptr_t return_addr, std::uintptr_t stack_pointer);

// Returns 1 to zero the field-chest gold add at +0x7612E (AP gold chests).
int TrySuppressVanillaFieldGold();

// Runtime options from slot_data (CONFIG …). Defaults match DefaultOnToggle.
void SetIncludeGoldChests(bool include);
bool IncludeGoldChests();
void SetIncludeSoldiersGraveyard(bool include);
bool IncludeSoldiersGraveyard();
void SetIncludeCastleOfDreams(bool include);
bool IncludeCastleOfDreams();
void SetIncludeTowerOfTemptation(bool include);
bool IncludeTowerOfTemptation();
bool IsExcludedOptionalDungeonEvent(uint16_t event_id);

}  // namespace grandia_ap
