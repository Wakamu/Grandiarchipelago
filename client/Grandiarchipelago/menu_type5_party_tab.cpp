#include "menu_type5_party_tab.h"

#include "game_memory.h"
#include "log.h"

#include <Windows.h>

#include <array>
#include <cstdint>
#include <cstring>
#include <vector>

namespace grandia_ap {
namespace {

constexpr std::uintptr_t kTabLoopStartMovRva = 0x62597u;  // mov ecx, tab_slot_base
constexpr std::uintptr_t kTabLoopEndCmpRva = 0x625ABu;    // cmp ecx, tab_slot_end
constexpr uint32_t kTabSlotStride = 0x0Eu;
constexpr uint32_t kStockTabSlots = 4u;
constexpr uint32_t kPatchedTabSlots = 5u;

struct AppliedPatch {
    void* site = nullptr;
    size_t size = 0;
    std::array<uint8_t, 8> original{};
};

bool g_installed = false;
std::vector<AppliedPatch> g_applied;

bool WriteBytes(void* site, const void* bytes, size_t size, uint8_t* original_out) {
    DWORD old_protect = 0;
    if (!VirtualProtect(site, size, PAGE_EXECUTE_READWRITE, &old_protect)) {
        return false;
    }
    if (original_out) {
        std::memcpy(original_out, site, size);
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

bool PatchImm8(std::uintptr_t base, std::uintptr_t rva, const uint8_t* prefix, size_t prefix_size,
               size_t imm_offset, uint8_t stock_imm, uint8_t patched_imm, const char* label) {
    auto* site = reinterpret_cast<uint8_t*>(base + rva);
    if (std::memcmp(site, prefix, prefix_size) != 0) {
        LogWarn("Menu type 5 tab patch: opcode mismatch for %s at +0x%X", label,
                static_cast<unsigned>(rva));
        return false;
    }

    const uint8_t current = site[imm_offset];
    if (current == patched_imm) {
        return true;
    }
    if (current != stock_imm) {
        LogWarn("Menu type 5 tab patch: imm mismatch for %s at +0x%X (got %u want %u or %u)", label,
                static_cast<unsigned>(rva), current, stock_imm, patched_imm);
        return false;
    }

    const uint8_t next = patched_imm;
    if (!WriteBytes(site + imm_offset, &next, 1, nullptr)) {
        LogWarn("Menu type 5 tab patch: write failed for %s at +0x%X", label,
                static_cast<unsigned>(rva));
        return false;
    }
    RememberPatch(site + imm_offset, 1);
    return true;
}

bool PatchSelectorLoopEnd(std::uintptr_t base) {
    const auto* mov_site = reinterpret_cast<const uint8_t*>(base + kTabLoopStartMovRva);
    if (mov_site[0] != 0xB9) {
        LogWarn("Menu type 5 tab patch: selector start mov mismatch at +0x%X",
                static_cast<unsigned>(kTabLoopStartMovRva));
        return false;
    }

    const auto* cmp_site = reinterpret_cast<const uint8_t*>(base + kTabLoopEndCmpRva);
    if (cmp_site[0] != 0x81 || cmp_site[1] != 0xF9) {
        LogWarn("Menu type 5 tab patch: selector end cmp mismatch at +0x%X",
                static_cast<unsigned>(kTabLoopEndCmpRva));
        return false;
    }

    const uint32_t start = *reinterpret_cast<const uint32_t*>(mov_site + 1);
    const uint32_t end = *reinterpret_cast<const uint32_t*>(cmp_site + 2);
    const uint32_t end_stock = start + kStockTabSlots * kTabSlotStride;
    const uint32_t end_patched = start + kPatchedTabSlots * kTabSlotStride;

    if (end == end_patched) {
        return true;
    }
    if (end != end_stock) {
        LogWarn("Menu type 5 tab patch: selector end 0x%08X (expected 0x%08X or 0x%08X)",
                end, end_stock, end_patched);
        return false;
    }

    auto* end_site = reinterpret_cast<uint8_t*>(base + kTabLoopEndCmpRva + 2);
    if (!WriteBytes(end_site, &end_patched, sizeof(end_patched), nullptr)) {
        LogWarn("Menu type 5 tab patch: selector end write failed at +0x%X",
                static_cast<unsigned>(kTabLoopEndCmpRva + 2));
        return false;
    }
    RememberPatch(end_site, sizeof(end_patched));
    LogInfo("Menu type 5 tab patch: selector end 0x%08X -> 0x%08X", end, end_patched);
    return true;
}

void RestoreAppliedPatches() {
    for (auto it = g_applied.rbegin(); it != g_applied.rend(); ++it) {
        if (it->site && it->size > 0) {
            WriteBytes(it->site, it->original.data(), it->size, nullptr);
        }
    }
    g_applied.clear();
}

}  // namespace

bool InstallMenuType5PartyTabHook() {
    // Disabled: +0x62350 is the Options menu (4 tabs), not Items/Equip/Magic/Moves/Status.
    // Real FWIN widen lives in menu_fwin_party_tab.cpp.
    constexpr bool kEnabled = false;
    if (!kEnabled) {
        return false;
    }

    if (g_installed) {
        return true;
    }

    const std::uintptr_t base = GetGrandiaModuleBase();
    if (base == 0) {
        LogWarn("Menu type 5 tab patch: grandia base unknown");
        return false;
    }

    g_applied.clear();
    g_applied.reserve(12);

    const uint8_t kCmpEsi4[] = {0x83, 0xFE, 0x04};
    const uint8_t kCmpEbx4[] = {0x83, 0xFB, 0x04};
    const uint8_t kCmpEdx4[] = {0x83, 0xFA, 0x04};
    const uint8_t kCmpCl3[] = {0x80, 0xF9, 0x03};
    const uint8_t kMovAbs[] = {0xC6, 0x05};
    const uint8_t kCmpAbs[] = {0x80, 0x3D};

    const bool ok =
        PatchImm8(base, 0x62586u, kCmpEsi4, 3, 2, 4, 5, "tab build loop") &&
        PatchImm8(base, 0x626D6u, kCmpEbx4, 3, 2, 4, 5, "tab cursor loop") &&
        PatchImm8(base, 0x62BA5u, kCmpEdx4, 3, 2, 4, 5, "tab clear loop") &&
        PatchImm8(base, 0x62C49u, kCmpEdx4, 3, 2, 4, 5, "tab init clear loop") &&
        PatchImm8(base, 0x62730u, kCmpCl3, 3, 2, 3, 4, "left-wrap cmp") &&
        PatchImm8(base, 0x627ADu, kMovAbs, 2, 6, 3, 4, "left-wrap mov") &&
        PatchImm8(base, 0x62800u, kCmpAbs, 2, 6, 3, 4, "right-wrap cmp A") &&
        PatchImm8(base, 0x62869u, kCmpAbs, 2, 6, 3, 4, "right-wrap cmp B") &&
        PatchImm8(base, 0x62BBCu, kCmpAbs, 2, 6, 3, 4, "close cmp") &&
        PatchSelectorLoopEnd(base);

    if (!ok) {
        RestoreAppliedPatches();
        LogWarn("Menu type 5 tab patch: install failed");
        return false;
    }

    g_installed = true;
    LogInfo("Menu type 5 tab patch active (phases 1-3: loops 5, wrap last=4, selector end=slot4)");
    return true;
}

void RemoveMenuType5PartyTabHook() {
    if (!g_installed) {
        return;
    }

    RestoreAppliedPatches();
    g_installed = false;
}

bool IsMenuType5PartyTabHookInstalled() {
    return g_installed;
}

}  // namespace grandia_ap
