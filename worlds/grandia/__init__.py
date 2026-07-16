from BaseClasses import ItemClassification, Tutorial
from worlds.AutoWorld import WebWorld, World
from worlds.LauncherComponents import Component, Type, components, launch

from .Items import (
    GrandiaItem,
    classification_table,
    item_name_groups,
    item_pool_counts,
    item_table,
)
from .Keys_data import FINISH_LOCATION_NAME
from .Locations import GrandiaLocation, location_table
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

    data_version = 7
    required_data_version = 7

    def create_regions(self) -> None:
        create_regions(self)

    def create_item(self, name: str) -> GrandiaItem:
        classification = classification_table.get(name, ItemClassification.filler)
        return GrandiaItem(name, classification, item_table[name], self.player)

    def create_location(self, name: str) -> GrandiaLocation:
        return GrandiaLocation(self.player, name, location_table[name])

    def create_items(self) -> None:
        # Partial progression: location count << full chest catalog.
        # Always keep world-map Keys in the pool; fill the rest with filler.
        # (Truncating the old full catalog pool was dropping Keys and breaking
        # accessibility.)
        from .Keys_data import KEY_ITEM_DATA

        locked_names = {"Victory"}
        if FINISH_LOCATION_NAME:
            locked_names.add(FINISH_LOCATION_NAME)

        location_count = sum(
            1
            for location in self.multiworld.get_locations(self.player)
            if not getattr(location, "locked", False) and location.name not in locked_names
        )

        pool: list[GrandiaItem] = [
            self.create_item(entry["item_name"]) for entry in KEY_ITEM_DATA
        ]

        filler_cycle = [
            name
            for name in ("Herbs", "Gold (10)", "Gold (30)", "Gold (90)", "Gold (60)")
            if name in item_table
        ]
        if not filler_cycle:
            filler_cycle = ["Herbs"] if "Herbs" in item_table else list(item_table)[:1]

        i = 0
        while len(pool) < location_count:
            pool.append(self.create_item(filler_cycle[i % len(filler_cycle)]))
            i += 1

        if len(pool) > location_count:
            # Never drop Keys — trim filler only.
            keys = {e["item_name"] for e in KEY_ITEM_DATA}
            kept_keys = [it for it in pool if it.name in keys]
            filler = [it for it in pool if it.name not in keys]
            need = location_count - len(kept_keys)
            pool = kept_keys + filler[: max(0, need)]

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
            "death_link": self.options.death_link.value,
            "victory_location": self.options.victory_condition.value,
            "data_version": self.data_version,
        }
