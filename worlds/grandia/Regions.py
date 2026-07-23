from typing import TYPE_CHECKING

from BaseClasses import CollectionState, Region

from .Keys_data import (
    FINISH_LOCATION_NAME,
    KEY_FOR_MAP,
    KEY_ITEM_DATA,
    KEY_REQUIREMENTS,
    LOGIC_MAP_HEXES,
    START_MAP_HEXES,
    STORY_CHECK_DATA,
)
from .Locations import (
    GrandiaLocation,
    location_table,
    lockout_event_item_name,
    lockout_location_name,
)
from .Locations_data import CHEST_LOCATION_DATA
from .OptionalDungeons_data import excluded_optional_dungeon_hexes

if TYPE_CHECKING:
    from . import GrandiaWorld


def blocking_story_entries() -> list[dict]:
    return [e for e in STORY_CHECK_DATA if e.get("blocks")]


def _key_for(map_hex: str) -> str | None:
    if map_hex in KEY_FOR_MAP:
        return KEY_FOR_MAP[map_hex]
    for k, v in KEY_FOR_MAP.items():
        if k.upper() == map_hex:
            return v
    return None


def _map_hex_from_id(map_id: int) -> str:
    return f"{map_id:04X}"


def sealed_by_lockout_events() -> dict[str, list[str]]:
    """map_hex → lockout event item names that close this map."""
    sealed: dict[str, list[str]] = {}
    for entry in blocking_story_entries():
        event = lockout_event_item_name(entry["ap_name"])
        for map_id in entry["blocks"]:
            hx = _map_hex_from_id(int(map_id))
            sealed.setdefault(hx, []).append(event)
    return sealed


def create_regions(world: "GrandiaWorld") -> None:
    """Only maps listed in progressions.json are in logic (partial progression OK)."""
    player = world.player
    menu = Region("Menu", player, world.multiworld)
    overworld = Region("Overworld", player, world.multiworld)

    start_hexes = {h.upper() for h in START_MAP_HEXES}
    logic_hexes = {h.upper() for h in LOGIC_MAP_HEXES}
    sealed_by = sealed_by_lockout_events()

    map_regions: dict[str, Region] = {}

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

    def has_keys(state: CollectionState, required: list[str]) -> bool:
        return all(state.has(name, player) for name in required)

    def not_sealed(state: CollectionState, lockout_events: list[str]) -> bool:
        return not any(state.has(name, player) for name in lockout_events)

    def make_enter_rule(required: list[str] | None, lockout_events: list[str]):
        req = list(required) if required else []
        seals = list(lockout_events)

        def rule(state: CollectionState) -> bool:
            if req and not has_keys(state, req):
                return False
            if seals and not not_sealed(state, seals):
                return False
            return True

        return rule

    for mid, region in map_regions.items():
        key_name = _key_for(mid)
        seals = sealed_by.get(mid, [])
        if mid in start_hexes:
            if seals:
                overworld.connect(region, f"Enter {region.name}", make_enter_rule(None, seals))
            else:
                overworld.connect(region)
        elif key_name:
            required = KEY_REQUIREMENTS[key_name]
            overworld.connect(
                region,
                f"Enter {region.name}",
                make_enter_rule(required, seals),
            )
        elif seals:
            overworld.connect(region, f"Enter {region.name}", make_enter_rule(None, seals))
        else:
            overworld.connect(region)

    include_gold = bool(world.options.include_gold_chests)
    excluded_dungeons = excluded_optional_dungeon_hexes(world.options)

    locs_by_map: dict[str, dict[str, int]] = {}
    for entry in CHEST_LOCATION_DATA:
        mid = entry["map_hex"].upper()
        if mid not in logic_hexes:
            continue
        if mid in excluded_dungeons:
            continue
        if entry.get("vanilla_kind") == "gold" and not include_gold:
            continue
        locs_by_map.setdefault(mid, {})[entry["ap_name"]] = location_table[entry["ap_name"]]

    for mid, loc_map in locs_by_map.items():
        map_regions[mid].add_locations(loc_map, GrandiaLocation)

    for entry in STORY_CHECK_DATA:
        mid = entry["map_hex"].upper()
        if mid in excluded_dungeons:
            continue
        name = entry["ap_name"]
        map_regions[mid].add_locations({name: location_table[name]}, GrandiaLocation)
        if entry.get("blocks"):
            lo_name = lockout_location_name(name)
            map_regions[mid].add_locations({lo_name: location_table[lo_name]}, GrandiaLocation)

    if not (FINISH_LOCATION_NAME and FINISH_LOCATION_NAME in location_table):
        overworld.add_locations({"Victory": location_table["Victory"]}, GrandiaLocation)

    menu.connect(overworld)
    world.multiworld.regions += [menu, overworld, *map_regions.values()]


def place_lockout_events(world: "GrandiaWorld") -> None:
    """Lock progression tokens onto companion '(Area Lockout)' locations.

    The original story check stays shuffled. The client completes both IDs when
    the story flag fires so Universal Tracker receives the lockout item.
    Tokens use real item IDs (required by MultiServer LocationStore).
    """
    player = world.player
    excluded = excluded_optional_dungeon_hexes(world.options)
    for entry in blocking_story_entries():
        mid = entry["map_hex"].upper()
        if mid in excluded:
            continue
        lo_name = lockout_location_name(entry["ap_name"])
        loc = world.multiworld.get_location(lo_name, player)
        loc.place_locked_item(world.create_item(lockout_event_item_name(entry["ap_name"])))
