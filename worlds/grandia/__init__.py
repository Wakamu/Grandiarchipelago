from BaseClasses import ItemClassification, Tutorial
from worlds.AutoWorld import WebWorld, World
from worlds.LauncherComponents import Component, Type, components, launch

from .Items import (
    GrandiaItem,
    classification_table,
    item_name_groups,
    item_table,
)
from .Keys_data import FINISH_LOCATION_NAME, KEY_ITEM_DATA, LOGIC_MAP_HEXES
from .Locations import GrandiaLocation, location_table
from .Locations_data import CHEST_LOCATION_DATA
from .OptionalDungeons_data import excluded_optional_dungeon_hexes
from .Options import GrandiaOptions
from .Regions import create_regions
from .Rules import set_rules


def _grandia_client(*args: str) -> None:
    from .GrandiaClient import launch as launch_grandia_client

    launch_grandia_client(*args)


def run_grandia_client(*args: str) -> None:
    """Launch the Grandia client from the Archipelago Launcher."""
    launch(_grandia_client, name="Grandia Client", args=args)


try:
    components.append(
        Component(
            "Grandia Client",
            func=run_grandia_client,
            component_type=Type.CLIENT,
            game_name="Grandia",
            supports_uri=True,
            description="Connect Archipelago to Grandia HD Remaster (injects native DLL).",
        )
    )
except TypeError:
    # Older Archipelago builds may not accept game_name / supports_uri / description.
    components.append(
        Component(
            "Grandia Client",
            func=run_grandia_client,
            component_type=Type.CLIENT,
        )
    )


class GrandiaWebWorld(WebWorld):
    theme = "ocean"
    tutorials = [
        Tutorial(
            "Multiworld Setup Guide",
            "A guide to playing Grandia HD Remaster with Archipelago.",
            "English",
            "setup_en.md",
            "setup/en",
            ["Grandiarchipelago"],
        ),
    ]


class GrandiaWorld(World):
    """Grandia HD Remaster (PC) — live multiworld via native Win32 client.

    v1 location model: MDP chest events + early story checks
    (AP loc id = 0x47522000 + event_id). World-map keys use
    0x47523000 + map_id (e.g. Key to Sult Ruins → 0x47525400).
    """

    game = "Grandia"
    web = GrandiaWebWorld()

    options_dataclass = GrandiaOptions
    options: GrandiaOptions
    topology_present = False

    item_name_to_id = item_table
    location_name_to_id = location_table
    item_name_groups = item_name_groups

    required_client_version = (0, 5, 0)

    data_version = 8
    required_data_version = 8

    def create_regions(self) -> None:
        create_regions(self)

    def create_item(self, name: str) -> GrandiaItem:
        classification = classification_table.get(name, ItemClassification.filler)
        return GrandiaItem(name, classification, item_table[name], self.player)

    def create_location(self, name: str) -> GrandiaLocation:
        return GrandiaLocation(self.player, name, location_table[name])

    def get_filler_item_name(self) -> str:
        if "Herbs" in item_table:
            return "Herbs"
        return next(iter(item_table))

    def create_items(self) -> None:
        """Shuffle vanilla chest loot + world-map keys into the included locations.

        Each included chest contributes its vanilla item. Keys are always added.
        Story checks add locations without chest loot, so a small amount of filler
        pads the pool to match location count. Gold items follow include_gold_chests.
        """
        locked_names = {"Victory"}
        if FINISH_LOCATION_NAME:
            locked_names.add(FINISH_LOCATION_NAME)

        location_count = sum(
            1
            for location in self.multiworld.get_locations(self.player)
            if not getattr(location, "locked", False) and location.name not in locked_names
        )

        include_gold = bool(self.options.include_gold_chests.value)
        excluded = excluded_optional_dungeon_hexes(self.options)
        logic_hexes = {h.upper() for h in LOGIC_MAP_HEXES}
        key_names = {entry["item_name"] for entry in KEY_ITEM_DATA}

        pool: list[GrandiaItem] = []

        for entry in CHEST_LOCATION_DATA:
            mid = entry["map_hex"].upper()
            if mid not in logic_hexes or mid in excluded:
                continue
            if entry.get("vanilla_kind") == "gold" and not include_gold:
                continue
            name = entry["vanilla_name"]
            if name not in item_table:
                continue
            pool.append(self.create_item(name))

        for entry in KEY_ITEM_DATA:
            pool.append(self.create_item(entry["item_name"]))

        filler_cycle = [self.get_filler_item_name()]
        if include_gold:
            for name in ("Gold (10)", "Gold (30)", "Gold (60)", "Gold (90)"):
                if name in item_table:
                    filler_cycle.append(name)
        else:
            for name in (
                "Would Salve",
                "Poison Antidote",
                "Blue Medicine",
                "Yellow Medicine",
                "Hand Grenade",
            ):
                if name in item_table and name not in filler_cycle:
                    filler_cycle.append(name)

        i = 0
        while len(pool) < location_count:
            pool.append(self.create_item(filler_cycle[i % len(filler_cycle)]))
            i += 1

        if len(pool) > location_count:
            keys = [item for item in pool if item.name in key_names]
            rest = [item for item in pool if item.name not in key_names]

            def keep_priority(item: GrandiaItem) -> int:
                # Lower = keep preferentially when trimming.
                if item.classification == ItemClassification.progression:
                    return 0
                if item.classification == ItemClassification.useful:
                    return 1
                if item.name.startswith("Gold ("):
                    return 3
                return 2

            rest.sort(key=keep_priority)
            need = location_count - len(keys)
            pool = keys + rest[: max(0, need)]

        self.multiworld.itempool += pool

    def set_rules(self) -> None:
        set_rules(self)
        # Key placement: Archipelago spheres + region access rules (start_maps /
        # unlocks_maps). No per-location item_rule / accessible_maps list.
        victory_loc = FINISH_LOCATION_NAME or "Victory"
        victory = self.multiworld.get_location(victory_loc, self.player)
        victory.place_locked_item(self.create_item("Victory"))

    def fill_slot_data(self) -> dict:
        return {
            "include_gold_chests": bool(self.options.include_gold_chests.value),
            "include_soldiers_graveyard": bool(self.options.include_soldiers_graveyard.value),
            "include_castle_of_dreams": bool(self.options.include_castle_of_dreams.value),
            "include_tower_of_temptation": bool(self.options.include_tower_of_temptation.value),
            "magic_xp_multiplier": int(self.options.magic_xp_multiplier.value),
            "skill_xp_multiplier": int(self.options.skill_xp_multiplier.value),
            "level_xp_multiplier": int(self.options.level_xp_multiplier.value),
            "gameplay_balance": int(self.options.gameplay_balance.value),
            "data_version": self.data_version,
        }
