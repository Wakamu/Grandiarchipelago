from typing import TYPE_CHECKING

from BaseClasses import CollectionState, Region

from .Keys_data import (
    FINISH_LOCATION_NAME,
    KEY_FOR_MAP,
    KEY_ITEM_DATA,
    LOGIC_MAP_HEXES,
    START_MAP_HEXES,
    STORY_CHECK_DATA,
)
from .Locations import GrandiaLocation, location_table
from .Locations_data import CHEST_LOCATION_DATA

if TYPE_CHECKING:
    from . import GrandiaWorld


def _key_for(map_hex: str) -> str | None:
    if map_hex in KEY_FOR_MAP:
        return KEY_FOR_MAP[map_hex]
    for k, v in KEY_FOR_MAP.items():
        if k.upper() == map_hex:
            return v
    return None


def create_regions(world: "GrandiaWorld") -> None:
    """Only maps listed in progressions.json are in logic (partial progression OK)."""
    player = world.player
    menu = Region("Menu", player, world.multiworld)
    overworld = Region("Overworld", player, world.multiworld)

    start_hexes = {h.upper() for h in START_MAP_HEXES}
    logic_hexes = {h.upper() for h in LOGIC_MAP_HEXES}

    map_regions: dict[str, Region] = {}

    # Chest maps — only if in progression scope.
    for entry in CHEST_LOCATION_DATA:
        mid = entry["map_hex"].upper()
        if mid not in logic_hexes or mid in map_regions:
            continue
        area = entry["area_name"] or f"Map {mid}"
        map_regions[mid] = Region(f"{area} ({mid})", player, world.multiworld)

    for entry in KEY_ITEM_DATA:
        for mid in entry["unlocks_map_hexes"]:
            mid_u = mid.upper()
            if mid_u not in map_regions:
                map_regions[mid_u] = Region(f"Map {mid_u}", player, world.multiworld)

    for entry in STORY_CHECK_DATA:
        mid = entry["map_hex"].upper()
        if mid not in map_regions:
            map_regions[mid] = Region(f"Map {mid}", player, world.multiworld)

    def has_key(state: CollectionState, item_name: str) -> bool:
        return state.has(item_name, player)

    for mid, region in map_regions.items():
        key_name = _key_for(mid)
        if mid in start_hexes:
            overworld.connect(region)
        elif key_name:
            overworld.connect(
                region,
                f"Enter {region.name}",
                lambda state, name=key_name: has_key(state, name),
            )
        else:
            # In progressions (event / accessible / blocks) but no Key yet —
            # treat as open for the partial logic tree (story routing TBD).
            overworld.connect(region)

    locs_by_map: dict[str, dict[str, int]] = {}
    for entry in CHEST_LOCATION_DATA:
        mid = entry["map_hex"].upper()
        if mid not in logic_hexes:
            continue
        locs_by_map.setdefault(mid, {})[entry["ap_name"]] = location_table[entry["ap_name"]]

    for mid, loc_map in locs_by_map.items():
        map_regions[mid].add_locations(loc_map, GrandiaLocation)

    for entry in STORY_CHECK_DATA:
        mid = entry["map_hex"].upper()
        name = entry["ap_name"]
        map_regions[mid].add_locations({name: location_table[name]}, GrandiaLocation)

    if not (FINISH_LOCATION_NAME and FINISH_LOCATION_NAME in location_table):
        overworld.add_locations({"Victory": location_table["Victory"]}, GrandiaLocation)

    menu.connect(overworld)
    world.multiworld.regions += [menu, overworld, *map_regions.values()]
