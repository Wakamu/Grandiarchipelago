from typing import TYPE_CHECKING

from .Regions import place_lockout_events

if TYPE_CHECKING:
    from . import GrandiaWorld


def set_rules(world: "GrandiaWorld") -> None:
    # Map access: Keys_data + Regions.create_regions (keys + lockout events).
    # Blocking story checks get locked lockout events in place_lockout_events.
    place_lockout_events(world)

    # Victory is locked on GameOver (map E01C), gated by Key to Luzet Mountains
    # (and every earlier key via KEY_REQUIREMENTS). Without this, Universal Tracker
    # treats the slot as always in Go Mode.
    player = world.player
    world.multiworld.completion_condition[player] = (
        lambda state: state.has("Victory", player)
    )
