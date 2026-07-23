#pragma once

// gameplay_balance CONFIG (0=vanilla, 1=redux):
//   - BATLE/M_DAT.BIN + FIELD/WINDT.BIN via fopen Redux overlays
//   - SHOP/MDP/BBG/SCN/MCHAR/TEXT overlays via same fopen hook
// Legacy in-memory M_DAT call-site patch retained in .cpp for rollback only.

namespace grandia_ap {

void SetGameplayBalance(unsigned pack);
unsigned GetGameplayBalance();

bool InstallMdatBalanceHook();
void RemoveMdatBalanceHook();
bool IsMdatBalanceHookInstalled();

// Called from asm detour after ranged read (only if call-site hooks re-enabled).
void HandleMdatRangeLoaded(const char* path, void* dest, unsigned file_offset, unsigned size);

}  // namespace grandia_ap
