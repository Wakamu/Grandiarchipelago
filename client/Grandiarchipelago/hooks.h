#pragma once

namespace grandia_ap {

bool InstallHooks();
void RemoveHooks();

// Call from RE once the item-give function is identified in grandia.exe.
void NotifyItemAcquired(int item_slot_id, const char* context);

}  // namespace grandia_ap
