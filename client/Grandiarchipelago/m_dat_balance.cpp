#include "m_dat_balance.h"

#include "game_memory.h"
#include "log.h"
#include "m_dat_redux_patch_data.h"
#include "windt_balance.h"

#include <Windows.h>

#include <atomic>
#include <cstdint>
#include <cstring>
#include <vector>

#if defined(_M_IX86)
extern "C" {
// Absolute address of grandia ranged reader (+0x16A0).
void* g_ap_range_reader = nullptr;
void ApMdatRangeCallDetour();
void ApOnMdatRangeLoaded(const char* path, void* dest, unsigned file_offset, unsigned size);
}
#endif

namespace grandia_ap {

namespace {

// M_DAT-specific call sites: `call +0x16A0` (5-byte E8).
constexpr std::uintptr_t kMdatCallSites[] = {0x982A8u, 0x12C885u};
constexpr size_t kCallStolen = 5;

std::atomic<unsigned> g_gameplay_balance{0};  // 0 vanilla, 1 redux

struct HookSite {
    void* address = nullptr;
    uint8_t original[8]{};
};
HookSite g_sites[2]{};
unsigned g_site_count = 0;

struct PatchRegion {
    uint32_t offset = 0;
    uint16_t length = 0;
    const uint8_t* vanilla = nullptr;
    const uint8_t* redux = nullptr;
};

struct PatchTable {
    uint32_t file_size = 0;
    std::vector<PatchRegion> regions;
};

PatchTable g_table;
bool g_table_ready = false;

bool BytesMatch(const uint8_t* a, const uint8_t* b, size_t n) {
    return std::memcmp(a, b, n) == 0;
}

bool WriteCall(void* site, void* destination, uint8_t* original_out) {
    DWORD old = 0;
    if (!VirtualProtect(site, kCallStolen, PAGE_EXECUTE_READWRITE, &old)) {
        return false;
    }
    if (original_out) {
        std::memcpy(original_out, site, kCallStolen);
    }
    auto* bytes = reinterpret_cast<uint8_t*>(site);
    const auto rel = static_cast<int32_t>(reinterpret_cast<uint8_t*>(destination) -
                                          (bytes + 5));
    bytes[0] = 0xE8;
    std::memcpy(bytes + 1, &rel, sizeof(rel));
    VirtualProtect(site, kCallStolen, old, &old);
    FlushInstructionCache(GetCurrentProcess(), site, kCallStolen);
    return true;
}

void RestoreSite(void* site, const uint8_t* original) {
    if (!site || !original) {
        return;
    }
    DWORD old = 0;
    VirtualProtect(site, kCallStolen, PAGE_EXECUTE_READWRITE, &old);
    std::memcpy(site, original, kCallStolen);
    VirtualProtect(site, kCallStolen, old, &old);
    FlushInstructionCache(GetCurrentProcess(), site, kCallStolen);
}

bool PathLooksLikeMdat(const char* path) {
    if (!path) {
        return false;
    }
    // Require "...m_dat.bin" (avoid m_dat.brm / accidental substrings).
    const char* bin = nullptr;
    for (const char* p = path; *p; ++p) {
        if ((p[0] == 'm' || p[0] == 'M') && p[1] == '_' && (p[2] == 'd' || p[2] == 'D') &&
            (p[3] == 'a' || p[3] == 'A') && (p[4] == 't' || p[4] == 'T') && p[5] == '.' &&
            (p[6] == 'b' || p[6] == 'B') && (p[7] == 'i' || p[7] == 'I') &&
            (p[8] == 'n' || p[8] == 'N') && p[9] == '\0') {
            bin = p;
            break;
        }
    }
    return bin != nullptr;
}

void ApplyOverlapping(uint8_t* dest, unsigned file_offset, unsigned size) {
    if (!dest || size == 0 || !g_table_ready) {
        return;
    }
    const unsigned end = file_offset + size;
    for (const auto& reg : g_table.regions) {
        const unsigned r0 = reg.offset;
        const unsigned r1 = reg.offset + reg.length;
        if (r1 <= file_offset || r0 >= end) {
            continue;
        }
        const unsigned a0 = r0 > file_offset ? r0 : file_offset;
        const unsigned a1 = r1 < end ? r1 : end;
        const unsigned van_off = a0 - r0;
        const unsigned dst_off = a0 - file_offset;
        const unsigned n = a1 - a0;
        uint8_t* slot = dest + dst_off;
        if (BytesMatch(slot, reg.redux + van_off, n)) {
            continue;
        }
        if (!BytesMatch(slot, reg.vanilla + van_off, n)) {
            continue;
        }
        std::memcpy(slot, reg.redux + van_off, n);
    }
}

void OnMdatRangeLoaded(const char* path, void* dest, unsigned file_offset, unsigned size) {
    if (g_gameplay_balance.load() == 0) {
        return;
    }
    if (!PathLooksLikeMdat(path) || !dest || size == 0) {
        return;
    }
    ApplyOverlapping(static_cast<uint8_t*>(dest), file_offset, size);
}

bool LoadEmbeddedPatch() {
    if (g_table_ready) {
        return true;
    }
    const uint8_t* blob = kMdatReduxPatchData;
    if (std::memcmp(blob, "GAPM", 4) != 0) {
        LogWarn("M_DAT patch: bad magic");
        return false;
    }
    const uint32_t version = *reinterpret_cast<const uint32_t*>(blob + 4);
    if (version != 2) {
        LogWarn("M_DAT patch: unsupported version %u", version);
        return false;
    }
    g_table.file_size = *reinterpret_cast<const uint32_t*>(blob + 8);
    const uint32_t count = *reinterpret_cast<const uint32_t*>(blob + 12);
    g_table.regions.clear();
    g_table.regions.reserve(count);

    std::size_t pos = 16;
    for (uint32_t i = 0; i < count; ++i) {
        if (pos + 6 > kMdatReduxPatchSize) {
            LogWarn("M_DAT patch: truncated header at region %u", i);
            g_table.regions.clear();
            return false;
        }
        const uint32_t off = *reinterpret_cast<const uint32_t*>(blob + pos);
        const uint16_t ln = *reinterpret_cast<const uint16_t*>(blob + pos + 4);
        pos += 6;
        if (pos + static_cast<std::size_t>(ln) * 2 > kMdatReduxPatchSize) {
            LogWarn("M_DAT patch: truncated payload at region %u", i);
            g_table.regions.clear();
            return false;
        }
        PatchRegion reg;
        reg.offset = off;
        reg.length = ln;
        reg.vanilla = blob + pos;
        pos += ln;
        reg.redux = blob + pos;
        pos += ln;
        g_table.regions.push_back(reg);
    }
    g_table_ready = true;
    return true;
}

}  // namespace

void SetGameplayBalance(unsigned pack) {
    const unsigned v = pack > 1 ? 1 : pack;
    g_gameplay_balance.store(v);
    LogInfo("CONFIG gameplay_balance=%u (%s)", v, v ? "redux" : "vanilla");
    if (v == 1) {
        // Ensure WINDT fallback patch table is ready if fopen overlay is incomplete.
        TryApplyWindtSec3Redux();
    } else {
        ResetWindtSec3ReduxApplied();
    }
}

unsigned GetGameplayBalance() {
    return g_gameplay_balance.load();
}

bool InstallMdatBalanceHook() {
    // Redux enemies: BATLE/M_DAT.BIN via fopen overlay (same size as vanilla).
    return true;
}

void RemoveMdatBalanceHook() {
#if defined(_M_IX86)
    for (unsigned i = 0; i < g_site_count; ++i) {
        RestoreSite(g_sites[i].address, g_sites[i].original);
        g_sites[i].address = nullptr;
    }
    g_site_count = 0;
    g_ap_range_reader = nullptr;
#endif
}

bool IsMdatBalanceHookInstalled() {
    // Overlay path does not install call sites; report ready so hooks.cpp stays quiet.
    return true;
}

void HandleMdatRangeLoaded(const char* path, void* dest, unsigned file_offset, unsigned size) {
    OnMdatRangeLoaded(path, dest, file_offset, size);
}

}  // namespace grandia_ap

#if defined(_M_IX86)

extern "C" void ApOnMdatRangeLoaded(const char* path, void* dest, unsigned file_offset,
                                    unsigned size) {
    grandia_ap::HandleMdatRangeLoaded(path, dest, file_offset, size);
}

// Replaces `call +0x16A0` at M_DAT-only sites.
// Entry (same as +0x16A0): ecx=path, edx=file_offset,
//   [esp]=return, [esp+4]=size, [esp+8]=dest. Caller cleans +8.
extern "C" __declspec(naked) void ApMdatRangeCallDetour() {
    __asm {
        push ebp
        mov ebp, esp
        sub esp, 16
        mov dword ptr [ebp - 4], ecx          ; path
        mov dword ptr [ebp - 8], edx          ; file offset
        mov eax, dword ptr [ebp + 8]
        mov dword ptr [ebp - 12], eax         ; size
        mov eax, dword ptr [ebp + 0Ch]
        mov dword ptr [ebp - 16], eax         ; dest

        push dword ptr [ebp - 16]             ; dest
        push dword ptr [ebp - 12]             ; size
        mov ecx, dword ptr [ebp - 4]
        mov edx, dword ptr [ebp - 8]
        call dword ptr [g_ap_range_reader]
        add esp, 8

        push dword ptr [ebp - 12]             ; size
        push dword ptr [ebp - 8]              ; offset
        push dword ptr [ebp - 16]             ; dest
        push dword ptr [ebp - 4]              ; path
        call ApOnMdatRangeLoaded
        add esp, 16

        mov esp, ebp
        pop ebp
        ret
    }
}

#endif
