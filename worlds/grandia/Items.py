from BaseClasses import Item, ItemClassification

from .Items_data import CHEST_ITEM_DATA, CHEST_ITEM_POOL_COUNTS
from .Keys_data import GRANDIA_KEY_ITEM_BASE, KEY_ITEM_DATA, STORY_CHECK_DATA
from .Locations import lockout_event_item_name

GRANDIA_ITEM_BASE = 0x4752_0000  # client/item_tracker.cpp — inventory item_id
# Logic-only progression tokens for area seals (not inventory).
# Must sit above map-key ids (0x47523000 + map_id, map_id up to ~0xE01C).
GRANDIA_LOCKOUT_ITEM_BASE = 0x4754_0000


class GrandiaItem(Item):
    game: str = "Grandia"


def _classification(name: str) -> ItemClassification:
    if name == "Victory":
        return ItemClassification.progression
    if name == "Progressive Party Member":
        return ItemClassification.progression
    if name.startswith("Lockout - "):
        return ItemClassification.progression
    if any(e["item_name"] == name for e in KEY_ITEM_DATA):
        return ItemClassification.progression
    meta = next((e for e in CHEST_ITEM_DATA if e["name"] == name), None)
    if meta is None:
        return ItemClassification.filler
    kind = meta["classification"]
    if kind == "useful":
        return ItemClassification.useful
    if kind == "progression":
        return ItemClassification.progression
    return ItemClassification.filler


item_table: dict[str, int] = {
    "Victory": GRANDIA_ITEM_BASE,
    "Progressive Party Member": GRANDIA_ITEM_BASE + 1,
}

for entry in CHEST_ITEM_DATA:
    item_table[entry["name"]] = GRANDIA_ITEM_BASE + entry["item_id"]

for entry in KEY_ITEM_DATA:
    # ITEM id = KEY_BASE + unlocks_maps[0] (primary); DLL unlocks the whole group.
    item_table[entry["item_name"]] = GRANDIA_KEY_ITEM_BASE + entry["primary_map_id"]

for entry in STORY_CHECK_DATA:
    if not entry.get("blocks"):
        continue
    item_table[lockout_event_item_name(entry["ap_name"])] = (
        GRANDIA_LOCKOUT_ITEM_BASE + int(entry["event_id"])
    )

item_name_groups = {
    "Progression": {
        "Victory",
        "Progressive Party Member",
        *(e["item_name"] for e in KEY_ITEM_DATA),
        *(
            lockout_event_item_name(e["ap_name"])
            for e in STORY_CHECK_DATA
            if e.get("blocks")
        ),
    },
    "World Map Keys": {e["item_name"] for e in KEY_ITEM_DATA},
    "Area Lockouts": {
        lockout_event_item_name(e["ap_name"])
        for e in STORY_CHECK_DATA
        if e.get("blocks")
    },
    "Useful": {
        e["name"] for e in CHEST_ITEM_DATA if e["classification"] == "useful"
    },
    "Filler": {
        e["name"] for e in CHEST_ITEM_DATA if e["classification"] == "filler"
    },
    "Gold": {e["name"] for e in CHEST_ITEM_DATA if e["kind"] == "gold"},
}

classification_table = {name: _classification(name) for name in item_table}

item_pool_counts = dict(CHEST_ITEM_POOL_COUNTS)
for entry in KEY_ITEM_DATA:
    item_pool_counts[entry["item_name"]] = item_pool_counts.get(entry["item_name"], 0) + 1
