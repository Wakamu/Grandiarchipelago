"""Optional dungeons whose chest pickups can be excluded from the multiworld."""

from __future__ import annotations

from typing import TYPE_CHECKING

if TYPE_CHECKING:
    from .Options import GrandiaOptions


def _map_hexes(map_ids: list[int]) -> frozenset[str]:
    return frozenset(f"{m:04X}" for m in map_ids)


# Soldier's Graveyard
SOLDIERS_GRAVEYARD_MAP_IDS: list[int] = [44036, 44040, 44044, 44048, 44052, 44056]
SOLDIERS_GRAVEYARD_MAP_HEXES: frozenset[str] = _map_hexes(SOLDIERS_GRAVEYARD_MAP_IDS)

# Castle of Dreams
CASTLE_OF_DREAMS_MAP_IDS: list[int] = [41988, 41996, 42000, 42004, 42008, 42012, 42016]
CASTLE_OF_DREAMS_MAP_HEXES: frozenset[str] = _map_hexes(CASTLE_OF_DREAMS_MAP_IDS)

# Tower of Temptation
TOWER_OF_TEMPTATION_MAP_IDS: list[int] = [
    46080,
    46084,
    46088,
    46092,
    46096,
    46104,
    46108,
    46112,
    46116,
    46120,
    46124,
]
TOWER_OF_TEMPTATION_MAP_HEXES: frozenset[str] = _map_hexes(TOWER_OF_TEMPTATION_MAP_IDS)


def excluded_optional_dungeon_hexes(options: "GrandiaOptions") -> set[str]:
    """Map hexes whose chest pickups are omitted when the matching option is off."""
    out: set[str] = set()
    if not options.include_soldiers_graveyard:
        out |= SOLDIERS_GRAVEYARD_MAP_HEXES
    if not options.include_castle_of_dreams:
        out |= CASTLE_OF_DREAMS_MAP_HEXES
    if not options.include_tower_of_temptation:
        out |= TOWER_OF_TEMPTATION_MAP_HEXES
    return out
