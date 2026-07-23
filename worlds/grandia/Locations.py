from BaseClasses import Location

from .Keys_data import FINISH_EVENT_ID, FINISH_LOCATION_NAME, STORY_CHECK_DATA
from .Locations_data import CHEST_LOCATION_DATA

# Must match client/Grandiarchipelago/location_ids.h
GRANDIA_CHEST_LOCATION_BASE = 0x4752_2000
# Companion locations for story checks that block maps (locked lockout events).
GRANDIA_AREA_LOCKOUT_LOCATION_BASE = 0x4752_4000


def lockout_location_name(story_ap_name: str) -> str:
    return f"{story_ap_name} (Area Lockout)"


def lockout_event_item_name(story_ap_name: str) -> str:
    return f"Lockout - {story_ap_name}"


class GrandiaLocation(Location):
    game: str = "Grandia"


location_table: dict[str, int] = {}

for entry in CHEST_LOCATION_DATA:
    location_table[entry["ap_name"]] = GRANDIA_CHEST_LOCATION_BASE + entry["event_id"]

for entry in STORY_CHECK_DATA:
    location_table[entry["ap_name"]] = GRANDIA_CHEST_LOCATION_BASE + entry["event_id"]

for entry in STORY_CHECK_DATA:
    if not entry.get("blocks"):
        continue
    loc_name = lockout_location_name(entry["ap_name"])
    location_table[loc_name] = GRANDIA_AREA_LOCKOUT_LOCATION_BASE + int(entry["event_id"])

# Alias used by options / locked Victory item placement.
if FINISH_LOCATION_NAME:
    location_table["Victory"] = GRANDIA_CHEST_LOCATION_BASE + int(FINISH_EVENT_ID)
else:
    location_table["Victory"] = GRANDIA_CHEST_LOCATION_BASE + 0x7FFF

event_id_to_location_name: dict[int, str] = {
    entry["event_id"]: entry["ap_name"] for entry in CHEST_LOCATION_DATA
}
for entry in STORY_CHECK_DATA:
    event_id_to_location_name[entry["event_id"]] = entry["ap_name"]
