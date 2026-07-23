#include "party_custom.h"

#include "d3d_overlay.h"
#include "game_memory.h"
#include "log.h"

#include <Windows.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

#if defined(_M_IX86)
extern "C" {
void ApPartyCountDetour();
void ApPartyCharIdDetour();
unsigned ApPartyResolveCount(unsigned group_index);
unsigned ApPartyResolveCharId(unsigned slot);
void* g_ap_party_count_resume = nullptr;
void* g_ap_party_char_resume = nullptr;
}
#endif

namespace {

// Parked: custom party ID hooks + PGR fopen remap. Re-enable when resuming.
constexpr bool kPartyCustomEnabled = false;

// +0x7E660 roster apply: count from preset table, then char ids per slot.
constexpr std::uintptr_t kCountLoadRva = 0x7E6ABu;
constexpr std::uintptr_t kCountResumeRva = 0x7E6B3u;
constexpr std::uintptr_t kCharIdLoadRva = 0x7E6D7u;
constexpr std::uintptr_t kCharIdResumeRva = 0x7E6DEu;
constexpr std::uintptr_t kRosterApplyRva = 0x7E660u;
constexpr std::uintptr_t kGroupApplyRva = 0x54F10u;
constexpr std::uintptr_t kMapObjPtrRva = 0x23FA94u;
constexpr std::uintptr_t kPresetTableRva = 0x2015A0u;

constexpr int kToggleVk = VK_F9;
constexpr int kCycleVk = VK_F10;
constexpr int kApplyVk = VK_F11;

// Char IDs (TCRF / in-exe preset table): 1 Justin, 2 Feena, 3 Sue, 4 Gadwin,
// 5 Rapp, 6 Milda, 7 Guido, 8 Liete, 11 Leen, 12 Rem.
struct StockPack {
    uint8_t count;
    uint8_t ids[4];
    uint8_t group;  // PGR{group:02X}.CPD on disk
};

// Stock FIELD packs that round-trip / match known rosters.
constexpr StockPack kStockPacks[] = {
    {1, {1, 0, 0, 0}, 0x00},
    {2, {1, 3, 0, 0}, 0x01},       // Justin+Sue
    {3, {1, 3, 2, 0}, 0x02},       // Justin+Sue+Feena
    {2, {1, 2, 0, 0}, 0x03},       // Justin+Feena
    {4, {1, 3, 2, 4}, 0x05},       // +Gadwin
    {3, {1, 2, 4, 0}, 0x08},       // Justin+Feena+Gadwin
    {3, {1, 2, 5, 0}, 0x09},       // Justin+Feena+Rapp
    {4, {1, 2, 5, 6}, 0x0A},       // +Milda
    {4, {1, 2, 5, 7}, 0x0C},       // +Guido
    {4, {1, 2, 5, 8}, 0x0F},       // +Liete
    {3, {1, 2, 12, 0}, 0x10},      // +Rem
};
constexpr int kStockPackCount = sizeof(kStockPacks) / sizeof(kStockPacks[0]);

struct TestRoster {
    const char* label;
    uint8_t count;
    uint8_t ids[4];
};

constexpr TestRoster kTests[] = {
    {"Justin+Feena+Rapp+Liete", 4, {1, 2, 5, 8}},  // stock PGR0F
    {"Justin+Feena+Rapp", 3, {1, 2, 5, 0}},         // stock PGR09
    {"Justin+Sue+Feena", 3, {1, 3, 2, 0}},          // stock PGR02
    {"Justin+Feena+Gadwin", 3, {1, 2, 4, 0}},       // stock PGR08
    {"Justin+Sue+Rapp (merge)", 3, {1, 3, 5, 0}},   // merged CPD override
    {"Justin only", 1, {1, 0, 0, 0}},               // stock PGR00
};
constexpr int kTestCount = sizeof(kTests) / sizeof(kTests[0]);

std::atomic<bool> g_enabled{false};
std::atomic<uint8_t> g_count{4};
uint8_t g_ids[4] = {1, 2, 5, 8};
int g_test_index = 0;

// 0xFF = no remap; 0xFE = use party_override merged CPD; else stock group index.
std::atomic<uint8_t> g_asset_group{0xFFu};
char g_merged_cpd_path[MAX_PATH]{};

bool g_toggle_was_down = false;
bool g_cycle_was_down = false;
bool g_apply_was_down = false;

void* g_count_site = nullptr;
void* g_char_site = nullptr;
uint8_t g_count_original[8]{};
uint8_t g_char_original[8]{};
bool g_installed = false;

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

bool WriteJump(void* site, void* destination, uint8_t* original_out, size_t patch_size) {
    if (patch_size < 5) {
        return false;
    }
    uint8_t patch[16]{};
    std::memset(patch, 0x90, patch_size);
    patch[0] = 0xE9;
    const auto rel = static_cast<int32_t>(reinterpret_cast<uint8_t*>(destination) -
                                          (reinterpret_cast<uint8_t*>(site) + 5));
    std::memcpy(patch + 1, &rel, sizeof(rel));
    return WriteBytes(site, patch, patch_size, original_out);
}

void RestoreBytes(void* site, const uint8_t* original, size_t size) {
    if (!site || !original) {
        return;
    }
    WriteBytes(site, original, size, nullptr);
}

bool EdgePress(int vk, bool& was_down) {
    const bool down = (GetAsyncKeyState(vk) & 0x8000) != 0;
    if (!down) {
        was_down = false;
        return false;
    }
    if (was_down) {
        return false;
    }
    was_down = true;
    return true;
}

const char* CharName(uint8_t id) {
    switch (id) {
        case 1:
            return "Justin";
        case 2:
            return "Feena";
        case 3:
            return "Sue";
        case 4:
            return "Gadwin";
        case 5:
            return "Rapp";
        case 6:
            return "Milda";
        case 7:
            return "Guido";
        case 8:
            return "Liete";
        case 11:
            return "Leen";
        case 12:
            return "Rem";
        default:
            return "?";
    }
}

std::string Dirname(const std::string& path) {
    const auto slash = path.find_last_of("\\/");
    if (slash == std::string::npos) {
        return ".";
    }
    return path.substr(0, slash);
}

std::string ModuleDirectory() {
    char buf[MAX_PATH]{};
    HMODULE self = nullptr;
    if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCSTR>(&ModuleDirectory), &self)) {
        return {};
    }
    if (!GetModuleFileNameA(self, buf, MAX_PATH)) {
        return {};
    }
    return Dirname(buf);
}

bool FileExists(const char* path) {
    const DWORD attrs = GetFileAttributesA(path);
    return attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

bool RosterEquals(const uint8_t* ids, uint8_t count, const StockPack& pack) {
    if (count != pack.count) {
        return false;
    }
    for (uint8_t i = 0; i < count; ++i) {
        if (ids[i] != pack.ids[i]) {
            return false;
        }
    }
    return true;
}

int FindStockGroup(const uint8_t* ids, uint8_t count) {
    for (int i = 0; i < kStockPackCount; ++i) {
        if (RosterEquals(ids, count, kStockPacks[i])) {
            return kStockPacks[i].group;
        }
    }
    return -1;
}

void BuildMergedName(char* out, size_t out_size, const uint8_t* ids, uint8_t count) {
    // PGR_1_3_5.CPD
    char tmp[64]{};
    size_t n = 0;
    n += static_cast<size_t>(std::snprintf(tmp + n, sizeof(tmp) - n, "PGR"));
    for (uint8_t i = 0; i < count && n + 4 < sizeof(tmp); ++i) {
        n += static_cast<size_t>(std::snprintf(tmp + n, sizeof(tmp) - n, "_%u", ids[i]));
    }
    std::snprintf(out, out_size, "%s.CPD", tmp);
}

bool ResolveMergedCpdPath(const uint8_t* ids, uint8_t count, char* out, size_t out_size) {
    char name[64]{};
    BuildMergedName(name, sizeof(name), ids, count);
    const std::string dll_dir = ModuleDirectory();
    if (dll_dir.empty()) {
        return false;
    }
    const char* rels[] = {
        "\\party_override\\",
        "\\..\\party_override\\",
    };
    for (const char* rel : rels) {
        std::snprintf(out, out_size, "%s%s%s", dll_dir.c_str(), rel, name);
        if (FileExists(out)) {
            return true;
        }
    }
    return false;
}

void RefreshAssetBinding() {
    g_merged_cpd_path[0] = '\0';
    const uint8_t n = g_count.load();
    const int stock = FindStockGroup(g_ids, n);
    if (stock >= 0) {
        g_asset_group.store(static_cast<uint8_t>(stock));
        grandia_ap::LogInfo("Party assets: stock PGR%02X", stock);
        return;
    }
    if (ResolveMergedCpdPath(g_ids, n, g_merged_cpd_path, sizeof(g_merged_cpd_path))) {
        g_asset_group.store(0xFEu);
        grandia_ap::LogInfo("Party assets: merged CPD %s", g_merged_cpd_path);
        return;
    }
    g_asset_group.store(0xFFu);
    grandia_ap::LogWarn("Party assets: no stock/merged pack for current roster");
}

void ToastParty(const char* prefix) {
    char buf[160];
    const uint8_t n = g_count.load();
    const uint8_t ag = g_asset_group.load();
    char asset[32]{};
    if (ag == 0xFFu) {
        std::snprintf(asset, sizeof(asset), "no-pack");
    } else if (ag == 0xFEu) {
        std::snprintf(asset, sizeof(asset), "merged");
    } else {
        std::snprintf(asset, sizeof(asset), "PGR%02X", ag);
    }

    if (n == 1) {
        std::snprintf(buf, sizeof(buf), "%s %s [%s]", prefix, CharName(g_ids[0]), asset);
    } else if (n == 2) {
        std::snprintf(buf, sizeof(buf), "%s %s+%s [%s]", prefix, CharName(g_ids[0]), CharName(g_ids[1]),
                      asset);
    } else if (n == 3) {
        std::snprintf(buf, sizeof(buf), "%s %s+%s+%s [%s]", prefix, CharName(g_ids[0]),
                      CharName(g_ids[1]), CharName(g_ids[2]), asset);
    } else {
        std::snprintf(buf, sizeof(buf), "%s %s+%s+%s+%s [%s]", prefix, CharName(g_ids[0]),
                      CharName(g_ids[1]), CharName(g_ids[2]), CharName(g_ids[3]), asset);
    }
    grandia_ap::ShowD3dOverlayToast(buf, 3500, 0x7CFC00u);
}

void ApplyTestRoster(int index) {
    if (index < 0 || index >= kTestCount) {
        return;
    }
    const TestRoster& t = kTests[index];
    g_count.store(t.count);
    std::memcpy(g_ids, t.ids, sizeof(g_ids));
    g_test_index = index;
    RefreshAssetBinding();
    grandia_ap::LogInfo("Custom party preset %d: %s", index, t.label);
    ToastParty(g_enabled.load() ? "Party:" : "Party (off):");
}

bool TriggerPartyApply() {
    const std::uintptr_t base = grandia_ap::GetGrandiaModuleBase();
    if (base == 0) {
        return false;
    }
    RefreshAssetBinding();

    uint8_t group = 0;
    __try {
        auto* map_obj = *reinterpret_cast<uint8_t**>(base + kMapObjPtrRva);
        if (!map_obj) {
            return false;
        }
        group = map_obj[2];
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }

    using RosterFn = void(__cdecl*)();
    using GroupFn = void(__fastcall*)(uint8_t);
    auto* roster = reinterpret_cast<RosterFn>(base + kRosterApplyRva);
    auto* group_apply = reinterpret_cast<GroupFn>(base + kGroupApplyRva);

    __try {
        roster();
        group_apply(group);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        grandia_ap::LogWarn("Custom party apply faulted (group=%u)", static_cast<unsigned>(group));
        return false;
    }
    grandia_ap::LogInfo(
        "Custom party apply: map_group=%u asset=%u enabled=%d count=%u ids=%u,%u,%u,%u",
        static_cast<unsigned>(group), static_cast<unsigned>(g_asset_group.load()),
        g_enabled.load() ? 1 : 0, g_count.load(), g_ids[0], g_ids[1], g_ids[2], g_ids[3]);
    ToastParty("Applied:");
    return true;
}

// Parse PGR%02X.CPD or pgr%02x_party__* from a path; returns group or -1.
int ParsePgrGroupFromPath(const char* path, const char** suffix_out) {
    if (!path || !*path) {
        return -1;
    }
    const char* base = path;
    for (const char* p = path; *p; ++p) {
        if (*p == '\\' || *p == '/') {
            base = p + 1;
        }
    }
    // PGR08.CPD / pgr08.cpd
    if (_strnicmp(base, "PGR", 3) == 0 && std::strlen(base) >= 9) {
        if (((base[3] >= '0' && base[3] <= '9') || (base[3] >= 'A' && base[3] <= 'F') ||
             (base[3] >= 'a' && base[3] <= 'f')) &&
            ((base[4] >= '0' && base[4] <= '9') || (base[4] >= 'A' && base[4] <= 'F') ||
             (base[4] >= 'a' && base[4] <= 'f')) &&
            _stricmp(base + 5, ".CPD") == 0) {
            char hex[3] = {base[3], base[4], 0};
            const unsigned long g = std::strtoul(hex, nullptr, 16);
            if (suffix_out) {
                *suffix_out = nullptr;
            }
            return static_cast<int>(g);
        }
    }
    // pgr08_party__atlas.png / pgr08_party__spriteinfo.bin
    if (_strnicmp(base, "pgr", 3) == 0 && std::strlen(base) >= 14) {
        if (((base[3] >= '0' && base[3] <= '9') || (base[3] >= 'A' && base[3] <= 'F') ||
             (base[3] >= 'a' && base[3] <= 'f')) &&
            ((base[4] >= '0' && base[4] <= '9') || (base[4] >= 'A' && base[4] <= 'F') ||
             (base[4] >= 'a' && base[4] <= 'f')) &&
            _strnicmp(base + 5, "_party__", 8) == 0) {
            char hex[3] = {base[3], base[4], 0};
            const unsigned long g = std::strtoul(hex, nullptr, 16);
            if (suffix_out) {
                *suffix_out = base + 5;  // "_party__..."
            }
            return static_cast<int>(g);
        }
    }
    return -1;
}

bool ReplaceBasename(const char* original_path, const char* new_base, char* out, size_t out_size) {
    if (!original_path || !new_base || !out || out_size == 0) {
        return false;
    }
    const char* slash = nullptr;
    for (const char* p = original_path; *p; ++p) {
        if (*p == '\\' || *p == '/') {
            slash = p;
        }
    }
    if (!slash) {
        return std::snprintf(out, out_size, "%s", new_base) > 0;
    }
    const size_t dir_len = static_cast<size_t>(slash - original_path + 1);
    if (dir_len + std::strlen(new_base) + 1 > out_size) {
        return false;
    }
    std::memcpy(out, original_path, dir_len);
    std::memcpy(out + dir_len, new_base, std::strlen(new_base) + 1);
    return true;
}

}  // namespace

#if defined(_M_IX86)
extern "C" {

unsigned ApPartyResolveCount(unsigned group_index) {
    if (g_enabled.load()) {
        return g_count.load();
    }
    const std::uintptr_t base = grandia_ap::GetGrandiaModuleBase();
    if (base == 0 || group_index > 0x10u) {
        return 0;
    }
    const auto* row =
        reinterpret_cast<const uint8_t*>(base + kPresetTableRva + group_index * 5u);
    return row[0];
}

unsigned ApPartyResolveCharId(unsigned slot) {
    if (!g_enabled.load()) {
        return 0xFFu;
    }
    if (slot >= g_count.load() || slot >= 4u) {
        return 0;
    }
    return g_ids[slot];
}

void __declspec(naked) ApPartyCountDetour() {
    __asm {
        push ecx
        push edx
        push eax
        call ApPartyResolveCount
        add esp, 4
        pop edx
        pop ecx
        jmp dword ptr [g_ap_party_count_resume]
    }
}

void __declspec(naked) ApPartyCharIdDetour() {
    __asm {
        push eax
        push ecx
        push edx
        mov eax, dword ptr [ebp - 4]
        push eax
        call ApPartyResolveCharId
        add esp, 4
        cmp al, 0xFF
        je use_original
        mov bl, al
        pop edx
        pop ecx
        pop eax
        jmp dword ptr [g_ap_party_char_resume]

    use_original:
        pop edx
        pop ecx
        pop eax
        mov bl, byte ptr [edx + ecx + 0x6015A1]
        jmp dword ptr [g_ap_party_char_resume]
    }
}

}  // extern "C"
#endif

namespace grandia_ap {

bool IsPartyCustomEnabled() { return g_enabled.load(); }

void SetPartyCustomEnabled(bool enabled) { g_enabled.store(enabled); }

bool SetCustomParty(const uint8_t* ids, uint8_t count) {
    if (!ids || count < 1 || count > 4) {
        return false;
    }
    g_count.store(count);
    std::memset(g_ids, 0, sizeof(g_ids));
    std::memcpy(g_ids, ids, count);
    RefreshAssetBinding();
    return true;
}

bool TryPartyAssetOverlay(const char* original_path, char* out_path, size_t out_size) {
    if (!kPartyCustomEnabled) {
        (void)original_path;
        (void)out_path;
        (void)out_size;
        return false;
    }
    if (!g_enabled.load() || !original_path || !out_path || out_size == 0) {
        return false;
    }
    const uint8_t ag = g_asset_group.load();
    if (ag == 0xFFu) {
        return false;
    }

    const char* suffix = nullptr;
    const int from_group = ParsePgrGroupFromPath(original_path, &suffix);
    if (from_group < 0) {
        return false;
    }

    // Merged CPD override: only remap .CPD (atlas still needs a stock pack — skip).
    if (ag == 0xFEu) {
        if (suffix != nullptr) {
            return false;  // no merged atlas yet
        }
        if (g_merged_cpd_path[0] == '\0' || !FileExists(g_merged_cpd_path)) {
            return false;
        }
        if (std::strlen(g_merged_cpd_path) + 1 > out_size) {
            return false;
        }
        std::memcpy(out_path, g_merged_cpd_path, std::strlen(g_merged_cpd_path) + 1);
        return true;
    }

    if (static_cast<unsigned>(from_group) == ag) {
        return false;  // already the right pack
    }

    char new_base[64]{};
    if (suffix == nullptr) {
        std::snprintf(new_base, sizeof(new_base), "PGR%02X.CPD", ag);
    } else {
        std::snprintf(new_base, sizeof(new_base), "pgr%02x%s", ag, suffix);
    }
    if (!ReplaceBasename(original_path, new_base, out_path, out_size)) {
        return false;
    }
    if (!FileExists(out_path)) {
        return false;
    }
    return true;
}

bool InstallPartyCustomHook() {
    if (!kPartyCustomEnabled) {
        LogInfo("Party custom: parked (F9/F10/F11 + PGR remap deferred)");
        return true;
    }
#if !defined(_M_IX86)
    return false;
#else
    if (g_installed) {
        return true;
    }
    const std::uintptr_t base = GetGrandiaModuleBase();
    if (base == 0) {
        LogWarn("Party custom: grandia base unknown");
        return false;
    }

    auto* count_site = reinterpret_cast<uint8_t*>(base + kCountLoadRva);
    auto* char_site = reinterpret_cast<uint8_t*>(base + kCharIdLoadRva);
    if (count_site[0] != 0x0F || count_site[1] != 0xB6 || count_site[2] != 0x84 ||
        count_site[3] != 0x80) {
        LogWarn("Party custom: count site mismatch at +0x%X", static_cast<unsigned>(kCountLoadRva));
        return false;
    }
    if (char_site[0] != 0x8A || char_site[1] != 0x9C || char_site[2] != 0x0A) {
        LogWarn("Party custom: char-id site mismatch at +0x%X",
                static_cast<unsigned>(kCharIdLoadRva));
        return false;
    }

    g_ap_party_count_resume = reinterpret_cast<void*>(base + kCountResumeRva);
    g_ap_party_char_resume = reinterpret_cast<void*>(base + kCharIdResumeRva);

    if (!WriteJump(count_site, reinterpret_cast<void*>(&ApPartyCountDetour), g_count_original, 8)) {
        LogWarn("Party custom: failed to patch count site");
        return false;
    }
    g_count_site = count_site;

    if (!WriteJump(char_site, reinterpret_cast<void*>(&ApPartyCharIdDetour), g_char_original, 7)) {
        RestoreBytes(g_count_site, g_count_original, 8);
        g_count_site = nullptr;
        LogWarn("Party custom: failed to patch char-id site");
        return false;
    }
    g_char_site = char_site;

    ApplyTestRoster(0);
    g_installed = true;
    LogInfo("Party custom hooks active (F9 toggle, F10 cycle, F11 apply) @ +0x%X / +0x%X",
            static_cast<unsigned>(kCountLoadRva), static_cast<unsigned>(kCharIdLoadRva));
    return true;
#endif
}

void RemovePartyCustomHook() {
    if (g_char_site) {
        RestoreBytes(g_char_site, g_char_original, 7);
        g_char_site = nullptr;
    }
    if (g_count_site) {
        RestoreBytes(g_count_site, g_count_original, 8);
        g_count_site = nullptr;
    }
#if defined(_M_IX86)
    g_ap_party_count_resume = nullptr;
    g_ap_party_char_resume = nullptr;
#endif
    g_enabled.store(false);
    g_asset_group.store(0xFFu);
    g_installed = false;
}

bool IsPartyCustomHookInstalled() { return g_installed; }

void PollPartyCustomHotkey() {
    if constexpr (!kPartyCustomEnabled) {
        return;
    }
    if (!g_installed) {
        return;
    }

    if (EdgePress(kToggleVk, g_toggle_was_down)) {
        const bool on = !g_enabled.load();
        g_enabled.store(on);
        if (on) {
            RefreshAssetBinding();
        }
        LogInfo("Custom party override %s", on ? "ON" : "OFF");
        ShowD3dOverlayToast(on ? "Custom party ON — F11 apply" : "Custom party OFF", 2500,
                            on ? 0x7CFC00u : 0xFFE528u);
    }

    if (EdgePress(kCycleVk, g_cycle_was_down)) {
        ApplyTestRoster((g_test_index + 1) % kTestCount);
    }

    if (EdgePress(kApplyVk, g_apply_was_down)) {
        if (!g_enabled.load()) {
            ShowD3dOverlayToast("Enable custom party first (F9)", 2500, 0xFA8072u);
        } else if (!TriggerPartyApply()) {
            ShowD3dOverlayToast("Party apply failed (need field map)", 2500, 0xFA8072u);
        }
    }
}

}  // namespace grandia_ap
