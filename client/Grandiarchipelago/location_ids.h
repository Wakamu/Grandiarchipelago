#pragma once

#include "known_slots.h"

namespace grandia_ap {

// Item-slot locations (v0 APWorld). Must match worlds/grandia/Locations.py GRANDIA_LOCATION_BASE.
constexpr unsigned kLocationBase = 0x47521000u;

// Per-chest / event-flag locations (RE: EDI at grandia.exe+0x70505, caller +0x53C45).
constexpr unsigned kChestEventLocationBase = 0x47522000u;

inline bool LocationIdForItemSlot(int slot_id, unsigned* out_location_id) {
    if (!IsKnownSlot(slot_id)) {
        return false;
    }
    *out_location_id = kLocationBase + static_cast<unsigned>(slot_id);
    return true;
}

inline unsigned LocationIdForChestEvent(unsigned event_id) {
    return kChestEventLocationBase + event_id;
}

}  // namespace grandia_ap
