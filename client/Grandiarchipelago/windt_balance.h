#pragma once

// Redux WINDT: prefer FIELD/WINDT.BIN via fopen overlay when gameplay_balance=redux.
// Finalize hooks spot-check live sec3; sell UI mid-hooks still force cost/2 from the
// live record (needed if shop UI caches prices).

namespace grandia_ap {

// Spot-check that live WINDT sec3 looks Redux (no in-place writes in overlay mode).
bool TryApplyWindtSec3Redux();

bool IsWindtSec3ReduxApplied();

void ResetWindtSec3ReduxApplied();

// Hook WINDT finalize + sell UI so overlay Redux prices are used before bake / sell.
bool InstallWindtSec3Hooks();
void RemoveWindtSec3Hooks();
bool IsWindtSec3HookInstalled();

}  // namespace grandia_ap
