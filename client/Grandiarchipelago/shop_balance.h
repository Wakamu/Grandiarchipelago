#pragma once

// Redux content: redirect fopen of FIELD/BIN/BATLE/TEXT assets to redux_content/
// when gameplay_balance=redux (SHOP.BIN, MDPs, MCHAR.DAT, BBG, SCN, …).

namespace grandia_ap {

bool InstallShopBalanceHooks();
void RemoveShopBalanceHooks();
bool IsShopBalanceHookInstalled();

}  // namespace grandia_ap
