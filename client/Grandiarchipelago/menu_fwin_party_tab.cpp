#include "menu_fwin_party_tab.h"

#include "game_memory.h"
#include "log.h"

#include <Windows.h>

#include <array>
#include <cstdint>
#include <cstring>
#include <vector>

namespace grandia_ap {
namespace {

// FWIN category-menu tab count widen. See docs/fwin_party_tab_checklist.md.
//
// Stock behavior:
//   - Normal: push 5 (Items..Status) at +0x1C6A07 / +0x1C6A2A
//   - Debug flag "4000" @ 0x63F00E: other branch already push 6 (6th tab = Debug)
//
// +0x1C695B is also GetGoldPtrAOB — gold JMP owns the live site; patch trampoline.
constexpr std::uintptr_t kCtorPushCountRva = 0x1C695Bu;
constexpr std::uintptr_t kSetupPushCountARva = 0x1C6A07u;
constexpr std::uintptr_t kSetupPushCountBRva = 0x1C6A2Au;
constexpr std::uintptr_t kDebugBranchPushCountRva = 0x1C69D2u;  // stock push 6

struct AppliedPatch {
    void* site = nullptr;
    size_t size = 0;
    std::array<uint8_t, 8> original{};
};

bool g_installed = false;
bool g_patched_gold_trampoline = false;
std::vector<AppliedPatch> g_applied;

bool WriteBytes(void* site, const void* bytes, size_t size) {
    DWORD old_protect = 0;
    if (!VirtualProtect(site, size, PAGE_EXECUTE_READWRITE, &old_protect)) {
        return false;
    }
    std::memcpy(site, bytes, size);
    VirtualProtect(site, size, old_protect, &old_protect);
    FlushInstructionCache(GetCurrentProcess(), site, size);
    return true;
}

void RememberPatch(void* site, size_t size) {
    AppliedPatch entry;
    entry.site = site;
    entry.size = size;
    std::memcpy(entry.original.data(), site, size);
    g_applied.push_back(entry);
}

bool PatchPushImm8(std::uintptr_t base, std::uintptr_t rva, uint8_t stock_imm, uint8_t patched_imm,
                   const char* label) {
    auto* site = reinterpret_cast<uint8_t*>(base + rva);
    if (site[0] != 0x6A) {
        LogWarn("FWIN party tab: opcode mismatch for %s at +0x%X (got %02X)", label,
                static_cast<unsigned>(rva), site[0]);
        return false;
    }

    const uint8_t current = site[1];
    if (current == patched_imm) {
        return true;
    }
    if (current != stock_imm) {
        LogWarn("FWIN party tab: imm mismatch for %s at +0x%X (got %u want %u or %u)", label,
                static_cast<unsigned>(rva), current, stock_imm, patched_imm);
        return false;
    }

    RememberPatch(site + 1, 1);
    const uint8_t next = patched_imm;
    if (!WriteBytes(site + 1, &next, 1)) {
        LogWarn("FWIN party tab: write failed for %s at +0x%X", label, static_cast<unsigned>(rva));
        g_applied.pop_back();
        return false;
    }
    LogInfo("FWIN party tab: %s +0x%X push %u -> %u", label, static_cast<unsigned>(rva), stock_imm,
            patched_imm);
    return true;
}

// Ctor push is secondary (slot clear loop). Gold hook often owns the site.
bool TryPatchCtorPushCount(std::uintptr_t base) {
    auto* site = reinterpret_cast<uint8_t*>(base + kCtorPushCountRva);
    if (site[0] == 0x6A) {
        return PatchPushImm8(base, kCtorPushCountRva, 5, 6, "ctor slot clear");
    }
    if (site[0] == 0xE9) {
        if (!PatchGoldHookStolenImm8(1, 5, 6)) {
            LogWarn(
                "FWIN party tab: +0x%X is JMP (GetGoldPtrAOB) but gold trampoline push patch "
                "failed — continuing with setup count sites only",
                static_cast<unsigned>(kCtorPushCountRva));
            return false;
        }
        g_patched_gold_trampoline = true;
        LogInfo("FWIN party tab: ctor slot clear via gold trampoline (+0x%X)",
                static_cast<unsigned>(kCtorPushCountRva));
        return true;
    }
    LogWarn("FWIN party tab: unexpected opcode at ctor +0x%X (got %02X) — skipping",
            static_cast<unsigned>(kCtorPushCountRva), site[0]);
    return false;
}

void RestoreAppliedPatches() {
    if (g_patched_gold_trampoline) {
        PatchGoldHookStolenImm8(1, 6, 5);
        g_patched_gold_trampoline = false;
    }
    for (auto it = g_applied.rbegin(); it != g_applied.rend(); ++it) {
        if (it->site && it->size > 0) {
            WriteBytes(it->site, it->original.data(), it->size);
        }
    }
    g_applied.clear();
}

}  // namespace

bool InstallMenuFwinPartyTabHook() {
    // Parked: Party UI is moving to the Load menu (+0x64F20, ecx=1), not FWIN tabs.
    // Phase-1 5→6 only unlocked the stock Debug "List" page — kept for reference.
    constexpr bool kEnabled = false;
    if (!kEnabled) {
        return false;
    }

    if (g_installed) {
        return true;
    }

    const std::uintptr_t base = GetGrandiaModuleBase();
    if (base == 0) {
        LogWarn("FWIN party tab: grandia base unknown");
        return false;
    }

    g_applied.clear();
    g_applied.reserve(4);
    g_patched_gold_trampoline = false;

    // Non-fatal: gold JMP may own this site.
    TryPatchCtorPushCount(base);

    // These set word [0x701160] via +0x1D03B0 — required for L1/R1 page 5.
    const bool ok = PatchPushImm8(base, kSetupPushCountARva, 5, 6, "setup count A") &&
                    PatchPushImm8(base, kSetupPushCountBRva, 5, 6, "setup count B");

    if (!ok) {
        RestoreAppliedPatches();
        LogWarn("FWIN party tab: phase-1 install failed (setup count sites)");
        return false;
    }

    // Debug branch already pushes 6 (vanilla Debug tab). Leave at 6 for now;
    // Party+Debug together needs 6→7 here in a later phase.
    const auto* dbg = reinterpret_cast<const uint8_t*>(base + kDebugBranchPushCountRva);
    if (dbg[0] == 0x6A) {
        LogInfo("FWIN party tab: debug-branch push at +0x%X is %u (stock Debug tab when flag ON)",
                static_cast<unsigned>(kDebugBranchPushCountRva), dbg[1]);
    }

    g_installed = true;
    LogInfo(
        "FWIN party tab phase 1 active (normal count 5->6). "
        "Tip: turn debug flag OFF (²) to test empty 6th slot — ON shows stock Debug tab");
    return true;
}

void RemoveMenuFwinPartyTabHook() {
    if (!g_installed) {
        return;
    }
    RestoreAppliedPatches();
    g_installed = false;
    LogInfo("FWIN party tab patches removed");
}

bool IsMenuFwinPartyTabHookInstalled() {
    return g_installed;
}

}  // namespace grandia_ap
