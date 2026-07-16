from dataclasses import dataclass

from Options import Choice, DeathLink, DefaultOnToggle, Toggle, PerGameCommonOptions


class VictoryCondition(Choice):
    """When the Grandia player is considered to have won the multiworld."""

    display_name = "Victory Condition"
    option_final_boss = 0
    option_credits = 1
    default = 0


class StartingParty(Toggle):
    """If enabled, starting character equipment slots may be checked locations."""

    display_name = "Randomize Starting Equipment"
    default = False


@dataclass
class GrandiaOptions(PerGameCommonOptions):
    death_link: DeathLink
    victory_condition: VictoryCondition
    starting_party: StartingParty
