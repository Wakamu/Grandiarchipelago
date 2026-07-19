from dataclasses import dataclass

from Options import DefaultOnToggle, PerGameCommonOptions


class IncludeGoldChests(DefaultOnToggle):
    """Include gold chest / gold bag pickups as Archipelago locations.

    When disabled, those pickups stay vanilla (you receive the gold in-game)
    and are not shuffled into the multiworld.
    """

    display_name = "Include Gold Chests"


class IncludeSoldiersGraveyard(DefaultOnToggle):
    """Include Soldier's Graveyard chest pickups as Archipelago locations.

    When disabled, that optional dungeon keeps vanilla loot and is omitted
    from the multiworld location pool.
    """

    display_name = "Include Soldier's Graveyard"


class IncludeCastleOfDreams(DefaultOnToggle):
    """Include Castle of Dreams chest pickups as Archipelago locations.

    When disabled, that optional dungeon keeps vanilla loot and is omitted
    from the multiworld location pool.
    """

    display_name = "Include Castle of Dreams"


class IncludeTowerOfTemptation(DefaultOnToggle):
    """Include Tower of Temptation chest pickups as Archipelago locations.

    When disabled, that optional dungeon keeps vanilla loot and is omitted
    from the multiworld location pool.
    """

    display_name = "Include Tower of Temptation"


@dataclass
class GrandiaOptions(PerGameCommonOptions):
    include_gold_chests: IncludeGoldChests
    include_soldiers_graveyard: IncludeSoldiersGraveyard
    include_castle_of_dreams: IncludeCastleOfDreams
    include_tower_of_temptation: IncludeTowerOfTemptation
