#pragma once

#include <cstdint>

namespace grandia_ap {

// Destination-commit gate (grandia.exe+0x61614). SI = full destination map id.
// See docs/world_map_key_gating.md.

constexpr unsigned kMapKeyItemBase = 0x47523000u;

bool InstallMapTravelHook();
void RemoveMapTravelHook();
bool IsMapTravelHookInstalled();

void UnlockMap(uint16_t map_id);
void UnlockKeyGroup(uint16_t primary_or_any_map);
bool IsMapUnlocked(uint16_t map_id);
bool IsMapGated(uint16_t map_id);
bool AllowMapTravel(uint16_t map_id);

// Drop runtime unlocks (call on save SYNC before bridge re-applies keys).
void ClearMapKeyState();

// True if ap_item_id is a Key-to-Map progression item (not a stash row).
bool TryHandleMapKeyItem(unsigned ap_item_id);

}  // namespace grandia_ap
