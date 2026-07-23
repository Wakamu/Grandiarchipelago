from typing import TYPE_CHECKING

from .Regions import place_lockout_events

if TYPE_CHECKING:
    from . import GrandiaWorld


def set_rules(world: "GrandiaWorld") -> None:
    # Map access: Keys_data + Regions.create_regions (keys + lockout events).
    # Blocking story checks get locked lockout events in place_lockout_events.
    place_lockout_events(world)
