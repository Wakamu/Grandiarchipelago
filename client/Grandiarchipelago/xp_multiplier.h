#pragma once

#include <cstdint>

namespace grandia_ap {

// Install battle XP multiply hooks:
//   magic +0xA4E96, skill +0xA4FA7,
//   level in-fight kill credit +0x1387C3 (add [battle+0x88], enemy_xp)
bool InstallXpMultiplierHooks();
void RemoveXpMultiplierHooks();
bool IsXpMultiplierHookInstalled();

// Runtime CONFIG from slot_data. Values are integer multipliers (1 = vanilla).
void SetMagicXpMultiplier(unsigned multiplier);
void SetSkillXpMultiplier(unsigned multiplier);
void SetLevelXpMultiplier(unsigned multiplier);
unsigned GetMagicXpMultiplier();
unsigned GetSkillXpMultiplier();
unsigned GetLevelXpMultiplier();

}  // namespace grandia_ap
