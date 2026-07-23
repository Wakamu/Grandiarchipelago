#include "windt_balance.h"

#include "game_memory.h"
#include "log.h"
#include "m_dat_balance.h"
#include "windt_sec3_redux_patch_data.h"

#include <Windows.h>

#include <atomic>
#include <cstdint>
#include <cstring>
#include <vector>

#if defined(_M_IX86)
extern "C" {
void* g_ap_windt_fin_m1 = nullptr;
void* g_ap_windt_fin_m2 = nullptr;
void* g_ap_windt_fin_m3 = nullptr;
void* g_ap_sell_half_list_cont = nullptr;  // +0x1ECDE5
void* g_ap_sell_half_sel_cont = nullptr;   // +0x1ED00A
void* g_ap_sell_list_orig = nullptr;       // trampoline → +0x1ECC40+6
void* g_ap_sell_sel_orig = nullptr;        // trampoline → +0x1ECF60+6
void ApWindtFinalizeM1();
void ApWindtFinalizeM2();
void ApWindtFinalizeM3();
void ApAfterWindtFinalize();
void ApSellHalfList();
void ApSellHalfSelect();
void ApSellListEntry();
void ApSellSelectEntry();
uint32_t __cdecl ApLookupSellCost(uint32_t item_id, uint8_t* rec);
}
#endif

namespace grandia_ap {

namespace {

// Field heap: WINDT file image at *(base+0x240E68) + 0x30000 (all three loaders).
constexpr std::uintptr_t kFieldHeapRva = 0x240E68u;
constexpr std::uintptr_t kWindtFromHeap = 0x30000u;

// Loader section-3 slots + runtime aliases used by item-resolve (shop/inv).
// Resolve: base + (id-1)*28 via [alias] — see +0x1C8A23 / +0x1E0DC3 / +0x1EAF63.
constexpr std::uintptr_t kSec3PtrRvas[] = {
    0x300240u,  // loader mode1 → [0x700240]
    0x3015A0u,  // loader mode2 → [0x7015A0]
    0x302558u,  // loader mode3 → [0x702558]
    0x30B364u,  // runtime mode1 → [0x70B364]  (shop/item resolve)
    0x308E24u,  // runtime mode2 → [0x708E24]
    0x307FD8u,  // runtime mode3 → [0x707FD8]
};

constexpr unsigned kRecordSize = 28;
constexpr unsigned kMaxItemId = 511;
constexpr uint16_t kManaEggId = 395;
constexpr uint16_t kManaEggReduxCost = 8000;
constexpr uint16_t kLorenzoId = 96;
constexpr uint16_t kLorenzoReduxCost = 25000;

struct ItemPatch {
    uint16_t item_id = 0;
    const uint8_t* vanilla = nullptr;
    const uint8_t* redux = nullptr;
};

std::vector<ItemPatch> g_patches;
bool g_patches_ready = false;
std::atomic<bool> g_healthy{false};  // active UI table has Redux Mana Egg cost

// call sites after each mode publishes runtime sec3 aliases (shop bakes prices after these).
constexpr std::uintptr_t kFinalizeCallRvas[] = {
    0x1C3E6Cu,  // mode1 → call +0x1C6940
    0x1DCDBEu,  // mode2 → call +0x1DCE70
    0x1E9467u,  // mode3 → call +0x1EA550
};

// Mode3 sell UI: list bake cost/2 (+0x1ECDDF) and select/hover cost/2 (+0x1ED002).
// Buy list was fixed by finalize hooks; sell display can still show a stale half-cost.
constexpr std::uintptr_t kSellHalfListRva = 0x1ECDDFu;
constexpr std::uintptr_t kSellHalfSelectRva = 0x1ED002u;
constexpr std::uintptr_t kSellListEntryRva = 0x1ECC40u;
constexpr std::uintptr_t kSellSelectEntryRva = 0x1ECF60u;
constexpr uint8_t kSellHalfListExpect[] = {0x0F, 0xB7, 0x40, 0x04, 0xD1, 0xE8};
constexpr uint8_t kSellHalfSelectExpect[] = {0x0F, 0xB7, 0x40, 0x04, 0x8B, 0xD0, 0xD1, 0xEA};

struct FinalizeSite {
    void* address = nullptr;
    uint8_t original[5]{};
};
FinalizeSite g_fin_sites[3]{};
unsigned g_fin_count = 0;

struct JumpSite {
    void* address = nullptr;
    uint8_t original[8]{};
    size_t size = 0;
    void* trampoline = nullptr;
};
JumpSite g_sell_sites[4]{};
unsigned g_sell_count = 0;

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

bool WriteCall(void* site, void* destination, uint8_t* original_out) {
    auto* bytes = reinterpret_cast<uint8_t*>(site);
    if (bytes[0] != 0xE8) {
        return false;
    }
    uint8_t patch[5] = {0xE8};
    const auto rel = static_cast<int32_t>(reinterpret_cast<uint8_t*>(destination) - (bytes + 5));
    std::memcpy(patch + 1, &rel, sizeof(rel));
    return WriteBytes(site, patch, 5, original_out);
}

bool WriteJump(void* site, void* destination, uint8_t* original_out, size_t patch_size) {
    if (patch_size < 5 || patch_size > 8) {
        return false;
    }
    uint8_t patch[8]{};
    if (original_out) {
        std::memcpy(original_out, site, patch_size);
    }
    patch[0] = 0xE9;
    const auto rel = static_cast<int32_t>(reinterpret_cast<uint8_t*>(destination) -
                                          (reinterpret_cast<uint8_t*>(site) + 5));
    std::memcpy(patch + 1, &rel, sizeof(rel));
    for (size_t i = 5; i < patch_size; ++i) {
        patch[i] = 0x90;
    }
    return WriteBytes(site, patch, patch_size, nullptr);
}

void* MakeTrampoline(const uint8_t* stolen, size_t stolen_size, void* continue_at) {
    void* mem = VirtualAlloc(nullptr, 32, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!mem) {
        return nullptr;
    }
    auto* tramp = reinterpret_cast<uint8_t*>(mem);
    std::memcpy(tramp, stolen, stolen_size);
    tramp[stolen_size] = 0xE9;
    const auto rel =
        static_cast<int32_t>(reinterpret_cast<uint8_t*>(continue_at) - (tramp + stolen_size + 5));
    std::memcpy(tramp + stolen_size + 1, &rel, sizeof(rel));
    FlushInstructionCache(GetCurrentProcess(), mem, 32);
    return mem;
}

void* CallTarget(void* site) {
    auto* bytes = reinterpret_cast<uint8_t*>(site);
    const int32_t rel = *reinterpret_cast<const int32_t*>(bytes + 1);
    return bytes + 5 + rel;
}

bool BytesMatch(const uint8_t* a, const uint8_t* b, size_t n) {
    return std::memcmp(a, b, n) == 0;
}

bool LoadEmbeddedPatch() {
    if (g_patches_ready) {
        return true;
    }
    const uint8_t* blob = kWindtSec3ReduxPatchData;
    if (std::memcmp(blob, "GAPW", 4) != 0) {
        LogWarn("WINDT sec3 patch: bad magic");
        return false;
    }
    const uint32_t version = *reinterpret_cast<const uint32_t*>(blob + 4);
    if (version != 1) {
        LogWarn("WINDT sec3 patch: unsupported version %u", version);
        return false;
    }
    const uint32_t count = *reinterpret_cast<const uint32_t*>(blob + 8);
    g_patches.clear();
    g_patches.reserve(count);
    std::size_t pos = 12;
    for (uint32_t i = 0; i < count; ++i) {
        if (pos + 2 + kRecordSize * 2 > kWindtSec3ReduxPatchSize) {
            LogWarn("WINDT sec3 patch: truncated at item %u", i);
            g_patches.clear();
            return false;
        }
        ItemPatch p;
        p.item_id = *reinterpret_cast<const uint16_t*>(blob + pos);
        pos += 2;
        p.vanilla = blob + pos;
        pos += kRecordSize;
        p.redux = blob + pos;
        pos += kRecordSize;
        if (p.item_id == 0 || p.item_id > kMaxItemId) {
            continue;
        }
        g_patches.push_back(p);
    }
    g_patches_ready = true;
    return true;
}

bool LooksLikeSec3(const uint8_t* sec3) {
    if (!sec3) {
        return false;
    }
    uint16_t id = 0;
    __try {
        id = *reinterpret_cast<const uint16_t*>(sec3);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    return id == 1;
}

uint8_t* ReadPtrRva(std::uintptr_t base, std::uintptr_t rva) {
    uint8_t* p = nullptr;
    __try {
        p = *reinterpret_cast<uint8_t**>(base + rva);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        p = nullptr;
    }
    return p;
}

uint8_t* Sec3FromHeap(std::uintptr_t base) {
    uint8_t* heap = ReadPtrRva(base, kFieldHeapRva);
    if (!heap) {
        return nullptr;
    }
    uint8_t* windt = heap + kWindtFromHeap;
    uint32_t sec0 = 0;
    uint32_t sec3_off = 0;
    __try {
        sec0 = *reinterpret_cast<uint32_t*>(windt);
        sec3_off = *reinterpret_cast<uint32_t*>(windt + 12);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
    if (sec0 < 16 || sec0 > 0x1000 || sec3_off < sec0 || sec3_off > 0x40000) {
        return nullptr;
    }
    uint8_t* sec3 = windt + sec3_off;
    return LooksLikeSec3(sec3) ? sec3 : nullptr;
}

void AddUnique(std::vector<uint8_t*>& out, uint8_t* p) {
    if (!p || !LooksLikeSec3(p)) {
        return;
    }
    for (uint8_t* existing : out) {
        if (existing == p) {
            return;
        }
    }
    out.push_back(p);
}

void CollectSec3Candidates(std::uintptr_t base, std::vector<uint8_t*>& out) {
    AddUnique(out, Sec3FromHeap(base));
    for (const std::uintptr_t rva : kSec3PtrRvas) {
        AddUnique(out, ReadPtrRva(base, rva));
    }
}

uint16_t ReadItemCost(uint8_t* sec3, uint16_t item_id) {
    uint16_t cost = 0;
    __try {
        cost = *reinterpret_cast<uint16_t*>(sec3 + (static_cast<unsigned>(item_id) - 1u) * kRecordSize + 4);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        cost = 0;
    }
    return cost;
}

uint16_t ReadItemId(uint8_t* rec) {
    uint16_t id = 0;
    __try {
        id = *reinterpret_cast<uint16_t*>(rec);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        id = 0;
    }
    return id;
}

uint16_t ReadRecordCost(uint8_t* rec) {
    uint16_t cost = 0;
    __try {
        cost = *reinterpret_cast<uint16_t*>(rec + 4);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        cost = 0;
    }
    return cost;
}

unsigned ApplyOneRecord(uint8_t* rec, const ItemPatch& p, unsigned* already, unsigned* mismatch,
                        unsigned* id_mismatch) {
    const uint16_t live_id = ReadItemId(rec);
    if (live_id == 0) {
        return 0;
    }
    if (live_id != p.item_id) {
        ++(*id_mismatch);
        return 0;
    }
    if (BytesMatch(rec, p.redux, kRecordSize)) {
        ++(*already);
        return 0;
    }
    DWORD old = 0;
    if (!VirtualProtect(rec, kRecordSize, PAGE_READWRITE, &old)) {
        return 0;
    }
    unsigned wrote = 0;
    if (BytesMatch(rec, p.vanilla, kRecordSize)) {
        std::memcpy(rec, p.redux, kRecordSize);
        wrote = 1;
    } else {
        // Partial / divergent record: still force buy-cost (shop UI + sell half use +4).
        const uint16_t want = *reinterpret_cast<const uint16_t*>(p.redux + 4);
        const uint16_t live_cost = ReadRecordCost(rec);
        if (live_cost != want) {
            *reinterpret_cast<uint16_t*>(rec + 4) = want;
            wrote = 1;
        } else {
            ++(*mismatch);
        }
    }
    VirtualProtect(rec, kRecordSize, old, &old);
    return wrote;
}

bool LookupReduxBuyCost(uint16_t item_id, uint16_t* out_cost) {
    if (!g_patches_ready || !out_cost) {
        return false;
    }
    for (const auto& p : g_patches) {
        if (p.item_id == item_id) {
            *out_cost = *reinterpret_cast<const uint16_t*>(p.redux + 4);
            return true;
        }
    }
    return false;
}

unsigned ApplyToTable(uint8_t* sec3, unsigned* already, unsigned* mismatch, unsigned* id_mismatch) {
    unsigned ok = 0;
    for (const auto& p : g_patches) {
        uint8_t* rec = sec3 + static_cast<std::size_t>(p.item_id - 1) * kRecordSize;
        ok += ApplyOneRecord(rec, p, already, mismatch, id_mismatch);
    }
    return ok;
}

bool TableLooksRedux(uint8_t* sec3) {
    return ReadItemCost(sec3, kManaEggId) == kManaEggReduxCost &&
           ReadItemCost(sec3, kLorenzoId) == kLorenzoReduxCost;
}

}  // namespace

uint32_t ResolveSellBuyCost(uint32_t item_id, uint8_t* rec) {
    if (GetGameplayBalance() == 1) {
        TryApplyWindtSec3Redux();
        uint16_t redux_cost = 0;
        if (LookupReduxBuyCost(static_cast<uint16_t>(item_id), &redux_cost)) {
            return redux_cost;
        }
    }
    return ReadRecordCost(rec);
}

bool TryApplyWindtSec3Redux() {
    // Prefer Redux FIELD/WINDT.BIN via fopen overlay. If live sec3 is still vanilla,
    // fall back to the embedded by-item_id patch (size/layout mismatch safety net).
    LoadEmbeddedPatch();
    const std::uintptr_t base = GetGrandiaModuleBase();
    if (base == 0) {
        return false;
    }

    std::vector<uint8_t*> tables;
    CollectSec3Candidates(base, tables);
    if (tables.empty()) {
        return g_healthy.load();
    }

    bool healthy_now = false;
    for (uint8_t* sec3 : tables) {
        if (TableLooksRedux(sec3)) {
            healthy_now = true;
            break;
        }
    }

    unsigned total_ok = 0;
    unsigned already = 0;
    unsigned mismatch = 0;
    unsigned id_mismatch = 0;
    if (!healthy_now && g_patches_ready) {
        for (uint8_t* sec3 : tables) {
            total_ok += ApplyToTable(sec3, &already, &mismatch, &id_mismatch);
        }
        for (uint8_t* sec3 : tables) {
            if (TableLooksRedux(sec3)) {
                healthy_now = true;
                break;
            }
        }
    }

    g_healthy.store(healthy_now);
    return healthy_now;
}

bool IsWindtSec3ReduxApplied() {
    return g_healthy.load();
}

void ResetWindtSec3ReduxApplied() {
    g_healthy.store(false);
}

bool InstallWindtSec3Hooks() {
#if !defined(_M_IX86)
    return false;
#else
    if (g_fin_count > 0 || g_sell_count > 0) {
        return true;
    }
    const std::uintptr_t base = GetGrandiaModuleBase();
    if (base == 0) {
        return false;
    }

    void* detours[3] = {reinterpret_cast<void*>(ApWindtFinalizeM1),
                        reinterpret_cast<void*>(ApWindtFinalizeM2),
                        reinterpret_cast<void*>(ApWindtFinalizeM3)};
    void** orig_slots[3] = {&g_ap_windt_fin_m1, &g_ap_windt_fin_m2, &g_ap_windt_fin_m3};

    for (unsigned i = 0; i < 3; ++i) {
        auto* site = reinterpret_cast<uint8_t*>(base + kFinalizeCallRvas[i]);
        if (site[0] != 0xE8) {
            LogWarn("WINDT finalize: expected call at +0x%X (got %02X)",
                    static_cast<unsigned>(kFinalizeCallRvas[i]), site[0]);
            RemoveWindtSec3Hooks();
            return false;
        }
        *orig_slots[i] = CallTarget(site);
        if (!WriteCall(site, detours[i], g_fin_sites[g_fin_count].original)) {
            LogWarn("WINDT finalize: failed to patch call at +0x%X",
                    static_cast<unsigned>(kFinalizeCallRvas[i]));
            RemoveWindtSec3Hooks();
            return false;
        }
        g_fin_sites[g_fin_count].address = site;
        ++g_fin_count;
    }

    auto* half_list = reinterpret_cast<uint8_t*>(base + kSellHalfListRva);
    auto* half_sel = reinterpret_cast<uint8_t*>(base + kSellHalfSelectRva);
    if (std::memcmp(half_list, kSellHalfListExpect, sizeof(kSellHalfListExpect)) != 0 ||
        std::memcmp(half_sel, kSellHalfSelectExpect, sizeof(kSellHalfSelectExpect)) != 0) {
        LogWarn("WINDT sell half: unexpected bytes at cost/2 sites");
        RemoveWindtSec3Hooks();
        return false;
    }

    g_ap_sell_half_list_cont = half_list + sizeof(kSellHalfListExpect);
    g_ap_sell_half_sel_cont = half_sel + sizeof(kSellHalfSelectExpect);

    auto install_jump = [&](std::uintptr_t rva, void* detour, size_t patch_size,
                            void** tramp_slot) -> bool {
        auto* site = reinterpret_cast<uint8_t*>(base + rva);
        JumpSite& js = g_sell_sites[g_sell_count];
        if (tramp_slot) {
            js.trampoline = MakeTrampoline(site, patch_size, site + patch_size);
            if (!js.trampoline) {
                return false;
            }
            *tramp_slot = js.trampoline;
        }
        if (!WriteJump(site, detour, js.original, patch_size)) {
            if (js.trampoline) {
                VirtualFree(js.trampoline, 0, MEM_RELEASE);
                js.trampoline = nullptr;
                if (tramp_slot) {
                    *tramp_slot = nullptr;
                }
            }
            return false;
        }
        js.address = site;
        js.size = patch_size;
        ++g_sell_count;
        return true;
    };

    if (!install_jump(kSellHalfListRva, reinterpret_cast<void*>(ApSellHalfList),
                      sizeof(kSellHalfListExpect), nullptr) ||
        !install_jump(kSellHalfSelectRva, reinterpret_cast<void*>(ApSellHalfSelect),
                      sizeof(kSellHalfSelectExpect), nullptr) ||
        !install_jump(kSellListEntryRva, reinterpret_cast<void*>(ApSellListEntry), 6,
                      &g_ap_sell_list_orig) ||
        !install_jump(kSellSelectEntryRva, reinterpret_cast<void*>(ApSellSelectEntry), 6,
                      &g_ap_sell_sel_orig)) {
        LogWarn("WINDT sell UI: failed to install sell price hooks");
        RemoveWindtSec3Hooks();
        return false;
    }

    return true;
#endif
}

void RemoveWindtSec3Hooks() {
#if defined(_M_IX86)
    for (unsigned i = 0; i < g_fin_count; ++i) {
        if (g_fin_sites[i].address) {
            WriteBytes(g_fin_sites[i].address, g_fin_sites[i].original, 5, nullptr);
            g_fin_sites[i].address = nullptr;
        }
    }
    g_fin_count = 0;
    g_ap_windt_fin_m1 = nullptr;
    g_ap_windt_fin_m2 = nullptr;
    g_ap_windt_fin_m3 = nullptr;

    for (unsigned i = 0; i < g_sell_count; ++i) {
        if (g_sell_sites[i].address) {
            WriteBytes(g_sell_sites[i].address, g_sell_sites[i].original, g_sell_sites[i].size,
                       nullptr);
            g_sell_sites[i].address = nullptr;
        }
        if (g_sell_sites[i].trampoline) {
            VirtualFree(g_sell_sites[i].trampoline, 0, MEM_RELEASE);
            g_sell_sites[i].trampoline = nullptr;
        }
        g_sell_sites[i].size = 0;
    }
    g_sell_count = 0;
    g_ap_sell_half_list_cont = nullptr;
    g_ap_sell_half_sel_cont = nullptr;
    g_ap_sell_list_orig = nullptr;
    g_ap_sell_sel_orig = nullptr;
#endif
}

bool IsWindtSec3HookInstalled() {
    return g_fin_count > 0;
}

}  // namespace grandia_ap

#if defined(_M_IX86)

extern "C" void ApAfterWindtFinalize() {
    if (grandia_ap::GetGameplayBalance() == 1) {
        grandia_ap::TryApplyWindtSec3Redux();
    }
}

extern "C" uint32_t __cdecl ApLookupSellCost(uint32_t item_id, uint8_t* rec) {
    return grandia_ap::ResolveSellBuyCost(item_id, rec);
}

// Replaces `call orig` at WINDT finalize sites.
// Sec3 pointers are already published before these calls, so we patch first, then
// tail-jmp to orig. Must NOT `call orig` here — that would push an extra return
// address and shift any stack args (broke shop Buy cursor).
extern "C" __declspec(naked) void ApWindtFinalizeM1() {
    __asm {
        pushad
        call ApAfterWindtFinalize
        popad
        jmp dword ptr [g_ap_windt_fin_m1]
    }
}

extern "C" __declspec(naked) void ApWindtFinalizeM2() {
    __asm {
        pushad
        call ApAfterWindtFinalize
        popad
        jmp dword ptr [g_ap_windt_fin_m2]
    }
}

extern "C" __declspec(naked) void ApWindtFinalizeM3() {
    __asm {
        pushad
        call ApAfterWindtFinalize
        popad
        jmp dword ptr [g_ap_windt_fin_m3]
    }
}

// +0x1ECDDF: eax = item record → bake sell price = cost/2 into stack slot.
extern "C" __declspec(naked) void ApSellHalfList() {
    __asm {
        push ecx
        push edx
        movzx ecx, word ptr [eax]
        push eax
        push ecx
        call ApLookupSellCost
        add esp, 8
        pop edx
        pop ecx
        shr eax, 1
        jmp dword ptr [g_ap_sell_half_list_cont]
    }
}

// +0x1ED002: eax = item record → eax=cost, edx=cost/2 for select/hover gold UI.
extern "C" __declspec(naked) void ApSellHalfSelect() {
    __asm {
        push ecx
        movzx ecx, word ptr [eax]
        push eax
        push ecx
        call ApLookupSellCost
        add esp, 8
        pop ecx
        mov edx, eax
        shr edx, 1
        jmp dword ptr [g_ap_sell_half_sel_cont]
    }
}

extern "C" __declspec(naked) void ApSellListEntry() {
    __asm {
        pushad
        call ApAfterWindtFinalize
        popad
        jmp dword ptr [g_ap_sell_list_orig]
    }
}

extern "C" __declspec(naked) void ApSellSelectEntry() {
    __asm {
        pushad
        call ApAfterWindtFinalize
        popad
        jmp dword ptr [g_ap_sell_sel_orig]
    }
}

#endif
