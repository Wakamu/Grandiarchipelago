from typing import TYPE_CHECKING

if TYPE_CHECKING:
    from . import GrandiaWorld


def set_rules(world: "GrandiaWorld") -> None:
    # Access comes from region entrances (Keys_data / Regions.create_regions).
    # Victory is locked in GrandiaWorld.set_rules via place_locked_item.
    _ = world
