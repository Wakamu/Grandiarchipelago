from dataclasses import dataclass

from Options import Choice, DefaultOnToggle, PerGameCommonOptions, Range


class IncludeGoldChests(DefaultOnToggle):
    """Include gold chest / gold bag pickups as Archipelago locations and items.

    When disabled, those pickups stay vanilla (you receive the gold in-game),
    they are omitted from the location pool, and Gold filler items are not
    placed in other chests.
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


class MagicXpMultiplier(Range):
    """Multiply magic XP gained in battle (Fire/Wind/Water/Earth).

    1 is vanilla. Applies when a character earns magic XP from casting.
    """

    display_name = "Magic XP Multiplier"
    range_start = 1
    range_end = 10
    default = 1


class SkillXpMultiplier(Range):
    """Multiply weapon/skill XP gained in battle (Sword, Mace, Bow, etc.).

    1 is vanilla. Applies when a character earns weapon-skill XP from attacking.
    """

    display_name = "Skill XP Multiplier"
    range_start = 1
    range_end = 10
    default = 1


class LevelXpMultiplier(Range):
    """Multiply character level XP gained from battle.

    1 is vanilla. Applies to the EXP added to each party member after combat.
    """

    display_name = "Level XP Multiplier"
    range_start = 1
    range_end = 10
    default = 1


class GameplayBalance(Choice):
    """Grandia Remastered Redux gameplay tables (enemies, items, shops, names).

    Vanilla uses stock HD Remaster data. Redux applies bundled content overlays
    at runtime (M_DAT enemies, WINDT prices/stats, shop stock, item names, etc.).
    Requires vanilla game files on disk — do not also install the Redux file pack.
    """

    display_name = "Gameplay Balance"
    option_vanilla = 0
    option_redux = 1
    default = 0


@dataclass
class GrandiaOptions(PerGameCommonOptions):
    include_gold_chests: IncludeGoldChests
    include_soldiers_graveyard: IncludeSoldiersGraveyard
    include_castle_of_dreams: IncludeCastleOfDreams
    include_tower_of_temptation: IncludeTowerOfTemptation
    magic_xp_multiplier: MagicXpMultiplier
    skill_xp_multiplier: SkillXpMultiplier
    level_xp_multiplier: LevelXpMultiplier
    gameplay_balance: GameplayBalance
