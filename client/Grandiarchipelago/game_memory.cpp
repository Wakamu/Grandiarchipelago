#include "chest_pickup.h"
#include "game_memory.h"
#include "log.h"
#include "map_travel.h"
#include "m_dat_balance.h"
#include "movie_skip.h"
#include "save_sync.h"
#include "speed_turbo.h"
#include "xp_multiplier.h"

#include <Windows.h>

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

namespace grandia_ap {

namespace {

void* g_stash_hook_site = nullptr;
uint8_t g_stash_hook_original[8]{};

void* g_gold_hook_site = nullptr;
uint8_t g_gold_hook_original[8]{};

void* g_field_gold_hook_site = nullptr;
uint8_t g_field_gold_hook_original[8]{};
void* g_field_gold_trampoline_mem = nullptr;

void* g_chest_flag_hook_site = nullptr;
uint8_t g_chest_flag_hook_original[8]{};

void* g_assign_ui_hook_site = nullptr;
uint8_t g_assign_ui_hook_original[8]{};

// Assign UI handler entry (grandia.exe+0x1DC100): push ebp; mov ebp,esp; and esp,-8
constexpr std::uintptr_t kAssignUiEntryRva = 0x1DC100;
constexpr size_t kAssignUiEntryPatchSize = 6;
constexpr uint8_t kAssignUiEntryBytes[] = {0x55, 0x8B, 0xEC, 0x83, 0xE4, 0xF8};

// Field chest gold add: add [eax+4], edx ; call …  (grandia.exe+0x7612E / +0x76131).
// JMP patch is 5 bytes and overlaps the CALL — trampoline must replay add+call and resume at +8.
constexpr std::uintptr_t kFieldGoldAddRva = 0x7612Eu;
constexpr size_t kFieldGoldAddInsnSize = 3;   // 01 50 04
constexpr size_t kFieldGoldCallInsnSize = 5;  // E8 rel32
constexpr size_t kFieldGoldStolenSize = kFieldGoldAddInsnSize + kFieldGoldCallInsnSize;
constexpr size_t kFieldGoldPatchSize = 5;
constexpr uint8_t kFieldGoldAddBytes[] = {0x01, 0x50, 0x04};
constexpr uint8_t kFieldGoldCallOpcode = 0xE8;

// Chest event flag write: mov byte ptr [edx+esi], al; mov eax, [global] — loot caller +0x53C45.
// Steam build: +0x70505. Some builds place the same insn at +0x90505 (x32dbg RE).
constexpr std::uintptr_t kChestFlagWriteRvaSteam = 0x70505;
constexpr std::uintptr_t kChestFlagWriteRvaAlt = 0x90505;
constexpr size_t kChestFlagWriteInsnSize = 3;  // 88 04 32
constexpr size_t kChestFlagPostMovSkip = 8;    // skip through following mov eax, [imm32]
constexpr uint8_t kChestFlagWriteBytes[] = {0x88, 0x04, 0x32};
constexpr uint8_t kChestFlagWritePostOpcode = 0xA1;  // mov eax, [imm32]

// Stash transfer RE (x32dbg, base 0x00F30000).
// Deposit (party -> stash): +0x3F475 -> +0x44BE2 -> +0x44CA3 -> +0x61E0D -> +0x1E6EB2 -> +0x1EC589
// Withdraw (stash -> party): likely uses +0x44C3E at the same layer instead of +0x44CA3
// RVAs below are relative to grandia.exe base (verify in x32dbg: base + RVA == stack address).
constexpr std::uintptr_t kStashUiGlobalRva = 0x240E60;
constexpr std::uintptr_t kStashTransferEntryRva = 0x44BF0;
constexpr std::uintptr_t kStashDepositPathRva = 0x44CA3;     // deposit + party equip spine
constexpr std::uintptr_t kStashWithdrawPathRva = 0x44C3E;    // withdraw (tentative)
constexpr std::uintptr_t kItemGiveSpineRva = 0x61E0D;        // abs 0x00F91E0D @ base 0x00F30000
constexpr std::uintptr_t kStashTransferHandlerRva = 0x1E6EB2;  // abs 0x01116EB2
constexpr std::uintptr_t kStashDepositUiRva = 0x1EC589;      // abs 0x0111C589 (lea edx,[ebp-28])
constexpr std::uintptr_t kStashWithdrawUiRva = 0x1EB14F;      // abs 0x0111E14F
// Deposit stash write (111C572-586): mov eax,[grandia.exe+0x307FC4]; ... mov [ecx-1],al
constexpr std::uintptr_t kStashArrayGlobalRva = 0x307FC4;   // abs 0x01237FC4 @ base 0x00F30000
// Publish init (grandia+0x1E87CE): eax = [heap_block] + 0x21A0; mov [307FC4], eax
constexpr std::uintptr_t kStashHeapBlockGlobalRva = 0x240E64;  // abs 0x00640E64 @ image base 0x00400000
constexpr std::uintptr_t kStashArrayOffsetInHeap = 0x21A0;
constexpr std::uintptr_t kStashDepositWriteRva = 0x1EC586;  // mov byte ptr [ecx-1],al
constexpr uint8_t kStashDepositUiQtyCap = 0x63;               // game caps at 99 in deposit UI

}  // namespace

}  // namespace grandia_ap

extern "C" {
void* g_ap_stash_return = nullptr;
void* g_ap_gold_trampoline = nullptr;
void* g_ap_field_gold_trampoline = nullptr;
std::uintptr_t g_ap_stash_base = 0;  // last eax from hook (may be UI scratch)
std::uintptr_t g_ap_stash_persistent_base = 0;
std::uintptr_t g_ap_gold_base = 0;
std::uintptr_t g_ap_character_base = 0;
volatile unsigned g_ap_stash_hook_hits = 0;
volatile std::uintptr_t g_ap_stash_hook_last_eax = 0;
void* g_ap_chest_flag_return = nullptr;
uint32_t g_ap_chest_flag_eax_src = 0;
volatile std::uintptr_t g_ap_assign_return_addr = 0;
volatile std::uintptr_t g_ap_assign_stack_pointer = 0;
void* g_ap_assign_return = nullptr;
std::uintptr_t g_grandia_module_base = 0;
}

extern "C" void ApChestEventNotify();
extern "C" int ApAssignUiNotify();
extern "C" int ApShouldSuppressFieldGold();
extern "C" void ApOnGoldPtrCaptured();

extern "C" {
extern volatile std::uintptr_t g_ap_assign_return_addr;
extern volatile std::uintptr_t g_ap_assign_stack_pointer;
extern void* g_ap_assign_return;
}

extern "C" {
extern volatile unsigned g_ap_flag_event_id;
extern volatile unsigned g_ap_flag_offset;
extern volatile unsigned g_ap_flag_value;
extern volatile unsigned g_ap_flag_mask;
extern volatile unsigned g_ap_flag_ecx;
extern volatile std::uintptr_t g_ap_flag_save_base;
extern volatile std::uintptr_t g_ap_flag_caller;
extern volatile uint8_t g_ap_flag_value_byte;
}

#if defined(_M_IX86)

extern "C" __declspec(naked) void ApChestFlagWriteDetour() {
    __asm {
        mov byte ptr [g_ap_flag_value_byte], al

        test ebp, ebp
        jz chest_flag_use_esp_caller
        mov eax, dword ptr [ebp+4]
        jmp chest_flag_store_caller
    chest_flag_use_esp_caller:
        mov eax, dword ptr [esp]
    chest_flag_store_caller:
        mov dword ptr [g_ap_flag_caller], eax

        mov dword ptr [g_ap_flag_event_id], edi
        mov dword ptr [g_ap_flag_offset], edx
        mov dword ptr [g_ap_flag_mask], ebx
        mov dword ptr [g_ap_flag_ecx], ecx
        mov dword ptr [g_ap_flag_save_base], esi
        movzx eax, byte ptr [g_ap_flag_value_byte]
        mov dword ptr [g_ap_flag_value], eax
        pushad
        call ApChestEventNotify
        popad
        mov al, byte ptr [g_ap_flag_value_byte]
        mov byte ptr [edx+esi], al
        push ebx
        mov ebx, dword ptr [g_ap_chest_flag_eax_src]
        mov eax, dword ptr [ebx]
        pop ebx
        jmp dword ptr [g_ap_chest_flag_return]
    }
}

extern "C" __declspec(naked) void ApAssignUiEntryDetour() {
    __asm {
        mov eax, dword ptr [esp]
        mov dword ptr [g_ap_assign_return_addr], eax
        mov dword ptr [g_ap_assign_stack_pointer], esp

        pushad
        call ApAssignUiNotify
        test eax, eax
        popad
        jz assign_ui_pass_through

        mov eax, 1
        ret

    assign_ui_pass_through:
        push ebp
        mov ebp, esp
        and esp, 0FFFFFFF8h
        jmp dword ptr [g_ap_assign_return]
    }
}

extern "C" __declspec(naked) void ApFieldGoldAddDetour() {
    __asm {
        pushad
        call ApShouldSuppressFieldGold
        test eax, eax
        jz field_gold_keep
        // Zero saved EDX in pushad frame so add [eax+4], edx adds 0.
        mov dword ptr [esp+14h], 0
    field_gold_keep:
        popad
        jmp dword ptr [g_ap_field_gold_trampoline]
    }
}

#endif

#if defined(_M_IX86)

extern "C" void ApOnGoldPtrCaptured();

extern "C" __declspec(naked) void ApStashDetour() {
    __asm {
        mov dword ptr [g_ap_stash_hook_last_eax], eax
        mov dword ptr [g_ap_stash_base], eax
        inc dword ptr [g_ap_stash_hook_hits]
        mov [ebp-20h], ecx
        mov [ebp-18h], eax
        jmp dword ptr [g_ap_stash_return]
    }
}

extern "C" __declspec(naked) void ApGoldDetour() {
    __asm {
        mov dword ptr [g_ap_gold_base], esi
        lea eax, [esi+10Ch]
        mov dword ptr [g_ap_character_base], eax
        pushad
        call ApOnGoldPtrCaptured
        popad
        jmp dword ptr [g_ap_gold_trampoline]
    }
}

#endif

namespace grandia_ap {
namespace {

constexpr unsigned kGoldValueOffset = 4;
constexpr unsigned kGoldMax = 9999999u;
constexpr size_t kGoldHookPatchSize = 5;  // push 5; mov al,[esi+disp]

std::mutex g_gold_mutex;
unsigned g_pending_gold = 0;
void* g_gold_trampoline_mem = nullptr;

std::vector<int> ParsePattern(const char* pattern_text) {
    std::vector<int> bytes;
    std::string token;
    for (const char* p = pattern_text; *p; ++p) {
        if (*p == ' ') {
            if (!token.empty()) {
                bytes.push_back((token == "??" || token == "?")
                                    ? -1
                                    : static_cast<int>(strtoul(token.c_str(), nullptr, 16)));
                token.clear();
            }
            continue;
        }
        token.push_back(*p);
    }
    if (!token.empty()) {
        bytes.push_back((token == "??" || token == "?") ? -1
                                                        : static_cast<int>(strtoul(token.c_str(), nullptr, 16)));
    }
    return bytes;
}

bool PatternMatches(const uint8_t* data, const std::vector<int>& pattern) {
    for (size_t j = 0; j < pattern.size(); ++j) {
        if (pattern[j] != -1 && data[j] != static_cast<uint8_t>(pattern[j])) {
            return false;
        }
    }
    return true;
}

std::vector<std::uintptr_t> ScanExecutableSections(HMODULE module, const std::vector<int>& pattern) {
    std::vector<std::uintptr_t> matches;
    if (!module || pattern.empty()) {
        return matches;
    }

    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(module);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) {
        return matches;
    }

    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(reinterpret_cast<uint8_t*>(module) + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) {
        return matches;
    }

    auto* base = reinterpret_cast<uint8_t*>(module);
    auto* section = IMAGE_FIRST_SECTION(nt);
    for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i) {
        if ((section[i].Characteristics & IMAGE_SCN_MEM_EXECUTE) == 0) {
            continue;
        }

        const auto* start = base + section[i].VirtualAddress;
        const size_t size = section[i].Misc.VirtualSize;
        if (size < pattern.size()) {
            continue;
        }

        for (size_t offset = 0; offset + pattern.size() <= size; ++offset) {
            if (PatternMatches(start + offset, pattern)) {
                matches.push_back(reinterpret_cast<std::uintptr_t>(start + offset));
            }
        }
    }

    return matches;
}

std::vector<std::uintptr_t> ScanWholeModuleImage(HMODULE module, const std::vector<int>& pattern) {
    std::vector<std::uintptr_t> matches;
    if (!module || pattern.empty()) {
        return matches;
    }

    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(module);
    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(reinterpret_cast<uint8_t*>(module) + dos->e_lfanew);
    auto* base = reinterpret_cast<uint8_t*>(module);
    const size_t size = nt->OptionalHeader.SizeOfImage;

    for (size_t i = 0; i + pattern.size() <= size; ++i) {
        if (PatternMatches(base + i, pattern)) {
            matches.push_back(reinterpret_cast<std::uintptr_t>(base + i));
        }
    }
    return matches;
}

bool IsExecutableAddress(void* address) {
    MEMORY_BASIC_INFORMATION info{};
    if (VirtualQuery(address, &info, sizeof(info)) == 0) {
        return false;
    }
    const DWORD prot = info.Protect & 0xFF;
    return prot == PAGE_EXECUTE || prot == PAGE_EXECUTE_READ || prot == PAGE_EXECUTE_READWRITE ||
           prot == PAGE_EXECUTE_WRITECOPY;
}

bool WriteJump(void* site, void* destination, uint8_t* original_out, size_t patch_size) {
    DWORD old_protect = 0;
    if (!VirtualProtect(site, patch_size, PAGE_EXECUTE_READWRITE, &old_protect)) {
        return false;
    }

    std::memcpy(original_out, site, patch_size);
    const auto rel = static_cast<int32_t>(reinterpret_cast<uint8_t*>(destination) -
                                            (reinterpret_cast<uint8_t*>(site) + 5));
    auto* bytes = reinterpret_cast<uint8_t*>(site);
    bytes[0] = 0xE9;
    std::memcpy(bytes + 1, &rel, sizeof(rel));
    for (size_t i = 5; i < patch_size; ++i) {
        bytes[i] = 0x90;
    }

    VirtualProtect(site, patch_size, old_protect, &old_protect);
    FlushInstructionCache(GetCurrentProcess(), site, patch_size);
    return true;
}

void RestoreBytes(void* site, const uint8_t* original, size_t size) {
    DWORD old_protect = 0;
    if (!VirtualProtect(site, size, PAGE_EXECUTE_READWRITE, &old_protect)) {
        return;
    }
    std::memcpy(site, original, size);
    VirtualProtect(site, size, old_protect, &old_protect);
    FlushInstructionCache(GetCurrentProcess(), site, size);
}

bool BytesMatch(const uint8_t* data, const uint8_t* expected, size_t size) {
    for (size_t i = 0; i < size; ++i) {
        if (data[i] != expected[i]) {
            return false;
        }
    }
    return true;
}

bool IsChestFlagWriteSite(std::uintptr_t site) {
    if (!IsExecutableAddress(reinterpret_cast<void*>(site))) {
        return false;
    }
    const auto* bytes = reinterpret_cast<const uint8_t*>(site);
    if (!BytesMatch(bytes, kChestFlagWriteBytes, sizeof(kChestFlagWriteBytes))) {
        return false;
    }
    if (bytes[kChestFlagWriteInsnSize] != kChestFlagWritePostOpcode) {
        return false;
    }
    return true;
}

std::uintptr_t ResolveChestFlagWriteSite(HMODULE module) {
    const auto base = reinterpret_cast<std::uintptr_t>(module);

    const auto pattern = ParsePattern("88 04 32 A1 18 24 ?? 00");
    const auto matches = ScanExecutableSections(module, pattern);
    for (const auto site : matches) {
        if (IsChestFlagWriteSite(site)) {
            LogInfo("Chest flag write site via AOB at 0x%08X (grandia.exe+0x%X)",
                    static_cast<unsigned>(site), static_cast<unsigned>(site - base));
            return site;
        }
    }

    for (const std::uintptr_t rva : {kChestFlagWriteRvaSteam, kChestFlagWriteRvaAlt}) {
        const auto site = base + rva;
        if (IsChestFlagWriteSite(site)) {
            LogInfo("Chest flag write site via RVA 0x%08X (grandia.exe+0x%X)", static_cast<unsigned>(site),
                    static_cast<unsigned>(rva));
            return site;
        }
        LogWarn("Chest flag write RVA 0x%X bytes mismatch at 0x%08X", static_cast<unsigned>(rva),
                static_cast<unsigned>(site));
    }

    LogWarn("Chest flag write hook pattern not found — chest event checks disabled");
    return 0;
}

bool InstallChestFlagHook(std::uintptr_t site) {
#if !defined(_M_IX86)
    LogWarn("Chest flag hook requires Win32 build");
    return false;
#else
    if (!IsChestFlagWriteSite(site)) {
        LogWarn("Refusing chest flag hook — unexpected bytes at 0x%08X", static_cast<unsigned>(site));
        return false;
    }

    constexpr size_t kPatchSize = 5;
    const auto* bytes = reinterpret_cast<const uint8_t*>(site);
    g_ap_chest_flag_eax_src = *reinterpret_cast<const uint32_t*>(bytes + 4);
    g_chest_flag_hook_site = reinterpret_cast<void*>(site);
    g_ap_chest_flag_return = reinterpret_cast<void*>(site + kChestFlagPostMovSkip);

    const auto detour = reinterpret_cast<void*>(ApChestFlagWriteDetour);
    if (!WriteJump(g_chest_flag_hook_site, detour, g_chest_flag_hook_original, kPatchSize)) {
        LogWarn("Failed to install chest event flag hook");
        g_chest_flag_hook_site = nullptr;
        g_ap_chest_flag_return = nullptr;
        g_ap_chest_flag_eax_src = 0;
        return false;
    }

    const std::uintptr_t base = GetGrandiaModuleBase();
    const std::uintptr_t rva = base != 0 ? site - base : 0;
    LogInfo(
        "Installed chest event flag hook at 0x%08X (grandia.exe+0x%X, resume=0x%08X, eax_global=0x%08X)",
        static_cast<unsigned>(site), static_cast<unsigned>(rva),
        static_cast<unsigned>(site + kChestFlagPostMovSkip), g_ap_chest_flag_eax_src);
    return true;
#endif
}

bool IsAssignUiEntrySite(std::uintptr_t site) {
    if (!IsExecutableAddress(reinterpret_cast<void*>(site))) {
        return false;
    }
    return BytesMatch(reinterpret_cast<const uint8_t*>(site), kAssignUiEntryBytes, sizeof(kAssignUiEntryBytes));
}

bool InstallAssignUiEntryHook(std::uintptr_t site) {
#if !defined(_M_IX86)
    LogWarn("Assign UI hook requires Win32 build");
    return false;
#else
    if (!IsAssignUiEntrySite(site)) {
        LogWarn("Refusing assign UI hook — unexpected bytes at 0x%08X (expected push ebp; mov ebp,esp; and esp,-8)",
                static_cast<unsigned>(site));
        return false;
    }

    g_assign_ui_hook_site = reinterpret_cast<void*>(site);
    g_ap_assign_return = reinterpret_cast<void*>(site + kAssignUiEntryPatchSize);

    const auto detour = reinterpret_cast<void*>(ApAssignUiEntryDetour);
    if (!WriteJump(g_assign_ui_hook_site, detour, g_assign_ui_hook_original, kAssignUiEntryPatchSize)) {
        LogWarn("Failed to install assign UI entry hook");
        g_assign_ui_hook_site = nullptr;
        g_ap_assign_return = nullptr;
        return false;
    }

    const std::uintptr_t base = GetGrandiaModuleBase();
    const std::uintptr_t rva = base != 0 ? site - base : 0;
    LogInfo("Installed assign UI entry hook at 0x%08X (grandia.exe+0x%X, resume=0x%08X)",
            static_cast<unsigned>(site), static_cast<unsigned>(rva),
            static_cast<unsigned>(site + kAssignUiEntryPatchSize));
    return true;
#endif
}

bool InstallStashHook(std::uintptr_t site) {
#if !defined(_M_IX86)
    LogWarn("Stash hook requires Win32 build");
    return false;
#else
    if (!IsExecutableAddress(reinterpret_cast<void*>(site))) {
        LogWarn("Refusing stash hook at non-executable address 0x%08X", static_cast<unsigned>(site));
        return false;
    }

    g_stash_hook_site = reinterpret_cast<void*>(site);
    g_ap_stash_return = reinterpret_cast<void*>(site + 6);

    const auto detour = reinterpret_cast<void*>(ApStashDetour);
    const int32_t rel = static_cast<int32_t>(reinterpret_cast<uint8_t*>(detour) -
                                             (reinterpret_cast<uint8_t*>(site) + 5));
    LogInfo("Stash hook: site=0x%08X detour=0x%08X rel=%d return=0x%08X", static_cast<unsigned>(site),
            static_cast<unsigned>(reinterpret_cast<std::uintptr_t>(detour)), rel,
            static_cast<unsigned>(site + 6));

    if (!WriteJump(g_stash_hook_site, detour, g_stash_hook_original, 6)) {
        LogWarn("Failed to install stash pointer hook");
        return false;
    }
    LogInfo("Installed GetStashPtrAOB hook at 0x%08X", static_cast<unsigned>(site));
    return true;
#endif
}

bool InstallGoldHook(std::uintptr_t site) {
#if !defined(_M_IX86)
    LogWarn("Gold hook requires Win32 build");
    return false;
#else
    if (!IsExecutableAddress(reinterpret_cast<void*>(site))) {
        LogWarn("Refusing gold hook at non-executable address 0x%08X", static_cast<unsigned>(site));
        return false;
    }

    const auto* bytes = reinterpret_cast<const uint8_t*>(site);
    // GetGoldPtrAOB starts: push 5; mov al,[esi+disp8]
    if (bytes[0] != 0x6A || bytes[1] != 0x05 || bytes[2] != 0x8A || bytes[3] != 0x46) {
        LogWarn("Refusing gold hook — unexpected bytes at 0x%08X", static_cast<unsigned>(site));
        return false;
    }

    g_gold_trampoline_mem = VirtualAlloc(nullptr, 32, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!g_gold_trampoline_mem) {
        LogWarn("Failed to allocate gold trampoline");
        return false;
    }

    auto* tramp = reinterpret_cast<uint8_t*>(g_gold_trampoline_mem);
    std::memcpy(tramp, bytes, kGoldHookPatchSize);
    tramp[kGoldHookPatchSize] = 0xE9;
    const auto resume = site + kGoldHookPatchSize;
    const auto rel = static_cast<int32_t>(resume - (reinterpret_cast<std::uintptr_t>(tramp) + kGoldHookPatchSize + 5));
    std::memcpy(tramp + kGoldHookPatchSize + 1, &rel, sizeof(rel));
    g_ap_gold_trampoline = g_gold_trampoline_mem;

    g_gold_hook_site = reinterpret_cast<void*>(site);
    if (!WriteJump(g_gold_hook_site, reinterpret_cast<void*>(ApGoldDetour), g_gold_hook_original,
                   kGoldHookPatchSize)) {
        LogWarn("Failed to install GetGoldPtrAOB hook");
        VirtualFree(g_gold_trampoline_mem, 0, MEM_RELEASE);
        g_gold_trampoline_mem = nullptr;
        g_ap_gold_trampoline = nullptr;
        g_gold_hook_site = nullptr;
        return false;
    }

    LogInfo("Installed GetGoldPtrAOB hook at 0x%08X (gold = [ESI]+4, cap %u)", static_cast<unsigned>(site),
            kGoldMax);
    return true;
#endif
}

bool InstallFieldGoldAddHook(std::uintptr_t site) {
#if !defined(_M_IX86)
    LogWarn("Field gold suppress hook requires Win32 build");
    return false;
#else
    if (!IsExecutableAddress(reinterpret_cast<void*>(site))) {
        LogWarn("Refusing field gold hook at non-executable 0x%08X", static_cast<unsigned>(site));
        return false;
    }
    const auto* bytes = reinterpret_cast<const uint8_t*>(site);
    if (!BytesMatch(bytes, kFieldGoldAddBytes, sizeof(kFieldGoldAddBytes))) {
        LogWarn("Field gold add bytes mismatch at 0x%08X (expected 01 50 04)", static_cast<unsigned>(site));
        return false;
    }
    if (bytes[kFieldGoldAddInsnSize] != kFieldGoldCallOpcode) {
        LogWarn("Field gold follow-up is not CALL at 0x%08X (expected E8 after add)",
                static_cast<unsigned>(site + kFieldGoldAddInsnSize));
        return false;
    }

    const auto call_site = site + kFieldGoldAddInsnSize;
    const auto call_rel = *reinterpret_cast<const int32_t*>(bytes + kFieldGoldAddInsnSize + 1);
    const auto call_abs = call_site + kFieldGoldCallInsnSize + call_rel;
    const auto resume = site + kFieldGoldStolenSize;

    g_field_gold_trampoline_mem = VirtualAlloc(nullptr, 32, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!g_field_gold_trampoline_mem) {
        LogWarn("Failed to allocate field-gold trampoline");
        return false;
    }

    // Replay: add [eax+4],edx ; call <relocated> ; jmp site+8
    auto* tramp = reinterpret_cast<uint8_t*>(g_field_gold_trampoline_mem);
    const auto tramp_base = reinterpret_cast<std::uintptr_t>(tramp);
    std::memcpy(tramp, bytes, kFieldGoldAddInsnSize);
    tramp[kFieldGoldAddInsnSize] = kFieldGoldCallOpcode;
    const auto tramp_call = tramp_base + kFieldGoldAddInsnSize;
    const auto relocated_call_rel =
        static_cast<int32_t>(call_abs - (tramp_call + kFieldGoldCallInsnSize));
    std::memcpy(tramp + kFieldGoldAddInsnSize + 1, &relocated_call_rel, sizeof(relocated_call_rel));
    tramp[kFieldGoldStolenSize] = 0xE9;
    const auto jmp_rel = static_cast<int32_t>(resume - (tramp_base + kFieldGoldStolenSize + 5));
    std::memcpy(tramp + kFieldGoldStolenSize + 1, &jmp_rel, sizeof(jmp_rel));
    g_ap_field_gold_trampoline = g_field_gold_trampoline_mem;

    g_field_gold_hook_site = reinterpret_cast<void*>(site);
    if (!WriteJump(g_field_gold_hook_site, reinterpret_cast<void*>(ApFieldGoldAddDetour),
                   g_field_gold_hook_original, kFieldGoldPatchSize)) {
        LogWarn("Failed to install field gold add hook");
        VirtualFree(g_field_gold_trampoline_mem, 0, MEM_RELEASE);
        g_field_gold_trampoline_mem = nullptr;
        g_ap_field_gold_trampoline = nullptr;
        g_field_gold_hook_site = nullptr;
        return false;
    }

    LogInfo(
        "Installed field-chest gold suppress at grandia.exe+0x%X (add [eax+4],edx; call→0x%08X; resume=+0x%X)",
        static_cast<unsigned>(kFieldGoldAddRva), static_cast<unsigned>(call_abs),
        static_cast<unsigned>(kFieldGoldStolenSize));
    return true;
#endif
}

std::uintptr_t StashQuantityAddress(int item_id) {
    if (g_ap_stash_persistent_base == 0 || item_id <= 0) {
        return 0;
    }
    return g_ap_stash_persistent_base + static_cast<std::uintptr_t>(item_id - 1);
}

std::uintptr_t StashByteAddress(int byte_offset) {
    if (g_ap_stash_persistent_base == 0 || byte_offset < 0) {
        return 0;
    }
    return g_ap_stash_persistent_base + static_cast<std::uintptr_t>(byte_offset);
}

bool SafeReadPointer(std::uintptr_t address, void** out_pointer) {
    if (!out_pointer) {
        return false;
    }
    *out_pointer = nullptr;
    __try {
        *out_pointer = *reinterpret_cast<void**>(address);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

std::uintptr_t ResolveStashUiGlobalAddress(HMODULE module) {
    const auto base = reinterpret_cast<std::uintptr_t>(module);
    const auto fixed = base + kStashUiGlobalRva;

    void* fixed_ptr = nullptr;
    if (SafeReadPointer(fixed, &fixed_ptr) && fixed_ptr != nullptr) {
        return fixed;
    }

    const auto pattern = ParsePattern("8B 0D ?? ?? ?? ??");
    const auto matches = ScanExecutableSections(module, pattern);
    for (const auto site : matches) {
        uint32_t absolute = 0;
        std::memcpy(&absolute, reinterpret_cast<const void*>(site + 2), sizeof(absolute));
        if (absolute < base) {
            continue;
        }
        void* candidate = nullptr;
        if (SafeReadPointer(absolute, &candidate) && candidate != nullptr) {
            LogInfo("Stash UI global via AOB at 0x%08X", absolute);
            return absolute;
        }
    }

    return fixed;
}

bool GetStashUiManager(void** out_manager) {
    if (!out_manager) {
        return false;
    }
    *out_manager = nullptr;

    HMODULE module = GetModuleHandleW(L"grandia.exe");
    if (!module) {
        return false;
    }

    const auto global = ResolveStashUiGlobalAddress(module);
    void* manager = nullptr;
    if (!SafeReadPointer(global, &manager) || manager == nullptr) {
        return false;
    }
    *out_manager = manager;
    return true;
}

std::uintptr_t ResolveStashArrayGlobalAddress(HMODULE module) {
    const auto base = reinterpret_cast<std::uintptr_t>(module);
    const auto deposit_write = base + kStashDepositWriteRva;
    const auto mov_eax_imm = deposit_write - 0x14;

    uint8_t opcode = 0;
    if (SafeReadByte(mov_eax_imm, &opcode) && opcode == 0xA1) {
        uint32_t absolute = 0;
        std::memcpy(&absolute, reinterpret_cast<const void*>(mov_eax_imm + 1), sizeof(absolute));
        LogInfo("Stash array global via deposit write insn at 0x%08X", absolute);
        return absolute;
    }

    const auto pattern = ParsePattern("A1 ?? ?? ?? ?? 03 C8 8A 41 FF");
    const auto matches = ScanExecutableSections(module, pattern);
    if (!matches.empty()) {
        uint32_t absolute = 0;
        std::memcpy(&absolute, reinterpret_cast<const void*>(matches[0] + 1), sizeof(absolute));
        LogInfo("Stash array global via AOB at 0x%08X", absolute);
        return absolute;
    }

    const auto fallback = base + kStashArrayGlobalRva;
    LogWarn("Stash array global fallback RVA 0x%08X (absolute 0x%08X)", static_cast<unsigned>(kStashArrayGlobalRva),
            static_cast<unsigned>(fallback));
    return fallback;
}

std::uintptr_t GetStashArrayGlobalAddressCached(HMODULE module) {
    static std::uintptr_t cached = 0;
    if (cached != 0) {
        return cached;
    }
    cached = ResolveStashArrayGlobalAddress(module);
    return cached;
}

std::uintptr_t ResolveStashHeapBlockGlobalAddress(HMODULE module) {
    const auto base = reinterpret_cast<std::uintptr_t>(module);
    const auto fixed = base + kStashHeapBlockGlobalRva;

    // Publish helper: A1 [heap]; test; alloc 0x80000; A3 [heap]; add eax, 0x21A0; A3 [StashPtr]
    const auto pattern = ParsePattern("A1 ?? ?? ?? ?? 85 C0 75 ?? 68 00 00 08 00");
    const auto matches = ScanExecutableSections(module, pattern);
    for (const auto site : matches) {
        uint32_t absolute = 0;
        std::memcpy(&absolute, reinterpret_cast<const void*>(site + 1), sizeof(absolute));
        if (absolute < base) {
            continue;
        }
        LogInfo("Stash heap block global via publish init at 0x%08X", absolute);
        return absolute;
    }

    LogWarn("Stash heap block global fallback RVA 0x%08X (absolute 0x%08X)",
            static_cast<unsigned>(kStashHeapBlockGlobalRva), static_cast<unsigned>(fixed));
    return fixed;
}

std::uintptr_t GetStashHeapBlockGlobalAddressCached(HMODULE module) {
    static std::uintptr_t cached = 0;
    if (cached != 0) {
        return cached;
    }
    cached = ResolveStashHeapBlockGlobalAddress(module);
    return cached;
}

bool IsPlausibleStashBase(std::uintptr_t candidate) {
    if (candidate < 0x10000) {
        return false;
    }

    uint8_t probe = 0;
    if (!SafeReadByte(candidate, &probe)) {
        return false;
    }
    if (!SafeReadByte(candidate + 0x1FF, &probe)) {
        return false;
    }
    return true;
}

void LogStashGlobalProbe(HMODULE module) {
    const auto heap_global = GetStashHeapBlockGlobalAddressCached(module);
    void* heap_block = nullptr;
    const bool heap_readable = SafeReadPointer(heap_global, &heap_block);
    if (!heap_readable) {
        LogInfo("Stash heap block probe: addr=0x%08X unreadable", static_cast<unsigned>(heap_global));
    } else if (heap_block == nullptr) {
        LogInfo("Stash heap block probe: addr=0x%08X value=null", static_cast<unsigned>(heap_global));
    } else {
        const auto derived =
            reinterpret_cast<std::uintptr_t>(heap_block) + kStashArrayOffsetInHeap;
        LogInfo("Stash heap block probe: addr=0x%08X heap=0x%08X derived=0x%08X",
                static_cast<unsigned>(heap_global),
                static_cast<unsigned>(reinterpret_cast<std::uintptr_t>(heap_block)),
                static_cast<unsigned>(derived));
    }

    const auto global = GetStashArrayGlobalAddressCached(module);
    void* ptr = nullptr;
    const bool readable = SafeReadPointer(global, &ptr);
    if (!readable) {
        LogInfo("Stash global probe: addr=0x%08X unreadable", static_cast<unsigned>(global));
        return;
    }
    if (ptr == nullptr) {
        LogInfo("Stash global probe: addr=0x%08X value=null (published ptr; heap derive may still work)",
                static_cast<unsigned>(global));
        return;
    }
    LogInfo("Stash global probe: addr=0x%08X value=0x%08X", static_cast<unsigned>(global),
            static_cast<unsigned>(reinterpret_cast<std::uintptr_t>(ptr)));
}

void LogPatternMatches(const char* label, const std::vector<std::uintptr_t>& exec_matches,
                       const std::vector<std::uintptr_t>& image_matches) {
    LogInfo("%s: %zu executable match(es), %zu whole-image match(es)", label, exec_matches.size(),
            image_matches.size());
    for (size_t i = 0; i < exec_matches.size() && i < 5; ++i) {
        LogInfo("  exec match[%zu]=0x%08X", i, static_cast<unsigned>(exec_matches[i]));
    }
    if (exec_matches.empty()) {
        for (size_t i = 0; i < image_matches.size() && i < 3; ++i) {
            LogWarn("  image-only match[%zu]=0x%08X (data section — not hooked)", i,
                    static_cast<unsigned>(image_matches[i]));
        }
    }
}

}  // namespace

bool TryAdoptStashBaseFromGlobal();
bool TryAdoptStashBaseFromHeapBlock();
bool EnsureStashBaseResolved();

bool InitializeGameMemory() {
    HMODULE module = GetModuleHandleW(L"grandia.exe");
    if (!module) {
        LogWarn("grandia.exe module handle not found");
        return false;
    }

    g_grandia_module_base = reinterpret_cast<std::uintptr_t>(module);
    LogInfo("grandia.exe base=0x%08X", static_cast<unsigned>(g_grandia_module_base));

    const auto stash_pattern = ParsePattern("89 4D E0 89 45 E8 80 38 00");
    const auto stash_exec = ScanExecutableSections(module, stash_pattern);
    const auto stash_image = ScanWholeModuleImage(module, stash_pattern);
    LogPatternMatches("GetStashPtrAOB", stash_exec, stash_image);

    if (!stash_exec.empty()) {
        InstallStashHook(stash_exec[0]);
    } else if (!stash_image.empty()) {
        LogWarn("GetStashPtrAOB only found in non-executable memory — pattern may be wrong for this build");
    } else {
        LogWarn("GetStashPtrAOB pattern not found — game version may differ from CE table");
    }

    const auto gold_pattern = ParsePattern("6A 05 8A 46 ?? 88 44 24 ??");
    const auto gold_exec = ScanExecutableSections(module, gold_pattern);
    const auto gold_image = ScanWholeModuleImage(module, gold_pattern);
    LogPatternMatches("GetGoldPtrAOB", gold_exec, gold_image);

    if (!gold_exec.empty()) {
        InstallGoldHook(gold_exec[0]);
    } else if (!gold_image.empty()) {
        LogWarn("GetGoldPtrAOB only found in non-executable memory — pattern may be wrong for this build");
    } else {
        LogWarn("GetGoldPtrAOB pattern not found — gold delivery disabled until pattern matches");
    }

    const std::uintptr_t chest_flag_site = ResolveChestFlagWriteSite(module);
    if (chest_flag_site != 0) {
        InstallChestFlagHook(chest_flag_site);
    }

    const std::uintptr_t assign_ui_site = g_grandia_module_base + kAssignUiEntryRva;
    if (IsAssignUiEntrySite(assign_ui_site)) {
        InstallAssignUiEntryHook(assign_ui_site);
    } else {
        LogWarn("Assign UI entry RVA 0x%X byte mismatch at 0x%08X — party assign skip disabled",
                static_cast<unsigned>(kAssignUiEntryRva), static_cast<unsigned>(assign_ui_site));
    }

    LogStashGlobalProbe(module);
    EnsureStashBaseResolved();

    InstallSaveSyncHooks();
    InstallMapTravelHook();

    if (g_grandia_module_base != 0) {
        InstallFieldGoldAddHook(g_grandia_module_base + kFieldGoldAddRva);
        InstallXpMultiplierHooks();
        InstallMdatBalanceHook();
    }

    return !stash_exec.empty() || !gold_exec.empty() || g_chest_flag_hook_site != nullptr ||
           g_assign_ui_hook_site != nullptr || g_field_gold_hook_site != nullptr ||
           IsSaveSyncHookInstalled() || IsMapTravelHookInstalled() || IsXpMultiplierHookInstalled() ||
           IsMdatBalanceHookInstalled();
}

bool TryAdoptStashBaseFromGlobal() {
    if (HasStashBase()) {
        return true;
    }

    HMODULE module = GetModuleHandleW(L"grandia.exe");
    if (!module) {
        return false;
    }

    const auto global = GetStashArrayGlobalAddressCached(module);
    void* stash_array = nullptr;
    if (!SafeReadPointer(global, &stash_array)) {
        static bool logged_unreadable = false;
        if (!logged_unreadable) {
            LogInfo("Cannot read stash array global at 0x%08X (will keep retrying)", static_cast<unsigned>(global));
            logged_unreadable = true;
        }
        return false;
    }

    if (stash_array == nullptr) {
        return false;
    }

    return AdoptStashBase(reinterpret_cast<std::uintptr_t>(stash_array), "stash array global");
}

bool TryAdoptStashBaseFromHeapBlock() {
    if (HasStashBase()) {
        return true;
    }

    HMODULE module = GetModuleHandleW(L"grandia.exe");
    if (!module) {
        return false;
    }

    const auto heap_global = GetStashHeapBlockGlobalAddressCached(module);
    void* heap_block = nullptr;
    if (!SafeReadPointer(heap_global, &heap_block) || heap_block == nullptr) {
        return false;
    }

    const auto candidate =
        reinterpret_cast<std::uintptr_t>(heap_block) + kStashArrayOffsetInHeap;
    if (!IsPlausibleStashBase(candidate)) {
        LogDebug("Stash heap derive 0x%08X failed readability probe", static_cast<unsigned>(candidate));
        return false;
    }

    return AdoptStashBase(candidate, "stash heap block+0x21A0");
}

bool EnsureStashBaseResolved() {
    if (HasStashBase()) {
        return true;
    }
    if (TryAdoptStashBaseFromGlobal()) {
        return true;
    }
    if (TryAdoptStashBaseFromHeapBlock()) {
        return true;
    }
    if (g_ap_stash_base != 0) {
        return AdoptStashBase(g_ap_stash_base, "stash UI hook");
    }
    return false;
}

void ShutdownGameMemory() {
#if defined(_M_IX86)
    RemoveMovieSkipHook();
    RemoveSpeedTurbo();
    RemoveXpMultiplierHooks();
    RemoveMapTravelHook();
    RemoveSaveSyncHooks();
    if (g_stash_hook_site) {
        RestoreBytes(g_stash_hook_site, g_stash_hook_original, 6);
        g_stash_hook_site = nullptr;
    }
    if (g_gold_hook_site) {
        RestoreBytes(g_gold_hook_site, g_gold_hook_original, kGoldHookPatchSize);
        g_gold_hook_site = nullptr;
    }
    if (g_gold_trampoline_mem) {
        VirtualFree(g_gold_trampoline_mem, 0, MEM_RELEASE);
        g_gold_trampoline_mem = nullptr;
        g_ap_gold_trampoline = nullptr;
    }
    if (g_field_gold_hook_site) {
        RestoreBytes(g_field_gold_hook_site, g_field_gold_hook_original, 5);
        g_field_gold_hook_site = nullptr;
    }
    if (g_field_gold_trampoline_mem) {
        VirtualFree(g_field_gold_trampoline_mem, 0, MEM_RELEASE);
        g_field_gold_trampoline_mem = nullptr;
        g_ap_field_gold_trampoline = nullptr;
    }
    if (g_chest_flag_hook_site) {
        RestoreBytes(g_chest_flag_hook_site, g_chest_flag_hook_original, 5);
        g_chest_flag_hook_site = nullptr;
        g_ap_chest_flag_return = nullptr;
        g_ap_chest_flag_eax_src = 0;
    }
    if (g_assign_ui_hook_site) {
        RestoreBytes(g_assign_ui_hook_site, g_assign_ui_hook_original, kAssignUiEntryPatchSize);
        g_assign_ui_hook_site = nullptr;
        g_ap_assign_return = nullptr;
    }
#endif
}

bool IsChestFlagHookInstalled() { return g_chest_flag_hook_site != nullptr; }

bool IsAssignUiEntryHookInstalled() { return g_assign_ui_hook_site != nullptr; }

bool IsFieldGoldAddHookInstalled() { return g_field_gold_hook_site != nullptr; }

bool IsPartyInventoryWriteHookInstalled() { return false; }

std::uintptr_t GetGrandiaModuleBase() { return g_grandia_module_base; }

bool HasStashBase() { return g_ap_stash_persistent_base != 0; }
std::uintptr_t GetStashBase() { return g_ap_stash_persistent_base; }

bool AdoptStashBase(std::uintptr_t candidate, const char* reason) {
    if (candidate == 0) {
        return false;
    }

    if (g_ap_stash_persistent_base == 0) {
        g_ap_stash_persistent_base = candidate;
        LogInfo("Adopted stash base 0x%08X (%s)", static_cast<unsigned>(candidate), reason);
        OnSaveSyncStashBecameReady();
        return true;
    }

    if (g_ap_stash_persistent_base == candidate) {
        return true;
    }

    // Keep the first stable base — later hook hits are stash UI refreshes for the same array.
    LogDebug("Ignored alternate stash pointer 0x%08X (%s)", static_cast<unsigned>(candidate), reason);
    return false;
}

unsigned GetStashHookHitCount() { return g_ap_stash_hook_hits; }
std::uintptr_t GetStashHookLastEax() { return g_ap_stash_hook_last_eax; }

bool SafeReadByte(std::uintptr_t address, uint8_t* out_byte) {
    if (!out_byte || address == 0) {
        return false;
    }
    __try {
        *out_byte = *reinterpret_cast<const uint8_t*>(address);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool ReadStashByteAtOffset(int byte_offset, uint8_t* out_byte) {
    if (g_ap_stash_persistent_base == 0 || byte_offset < 0 || !out_byte) {
        return false;
    }
    return SafeReadByte(g_ap_stash_persistent_base + static_cast<std::uintptr_t>(byte_offset), out_byte);
}

bool ReadStashQuantity(int item_id, uint8_t* out_quantity) {
    return ReadStashByteAtOffset(item_id - 1, out_quantity);
}

bool WriteStashQuantity(int item_id, uint8_t quantity) {
    const auto address = StashQuantityAddress(item_id);
    if (!address) {
        return false;
    }
    DWORD old_protect = 0;
    if (!VirtualProtect(reinterpret_cast<void*>(address), 1, PAGE_EXECUTE_READWRITE, &old_protect)) {
        return false;
    }
    *reinterpret_cast<uint8_t*>(address) = quantity;
    VirtualProtect(reinterpret_cast<void*>(address), 1, old_protect, &old_protect);
    return true;
}

bool HasStashUiManager() {
    void* manager = nullptr;
    return GetStashUiManager(&manager);
}

bool CallGameAddStashItem(int item_id, uint8_t quantity) {
    if (item_id <= 0 || quantity == 0) {
        return false;
    }

    if (!EnsureStashBaseResolved()) {
        LogWarn("Cannot deliver item=%d — stash base unknown (load a save in-game first)", item_id);
        return false;
    }

    uint8_t before = 0;
    if (!ReadStashQuantity(item_id, &before)) {
        LogWarn("Cannot read stash qty for item=%d at base 0x%08X", item_id,
                static_cast<unsigned>(g_ap_stash_persistent_base));
        return false;
    }

    // Replicates grandia+0x1EC586: mov byte ptr [stash_base+item_id-1], al (after inc al).
    int next = static_cast<int>(before) + static_cast<int>(quantity);
    if (next > 255) {
        next = 255;
    }
    if (next == before) {
        return before > 0 || quantity == 0;
    }

    if (!WriteStashQuantity(item_id, static_cast<uint8_t>(next))) {
        return false;
    }

    LogInfo("Stash delivery via deposit write logic (item=%d qty %u -> %u)", item_id, before,
            static_cast<unsigned>(next));
    return true;
}

bool AddStashQuantity(int item_id, uint8_t delta) {
    return CallGameAddStashItem(item_id, delta);
}

bool HasGoldBase() { return g_ap_gold_base != 0; }

std::uintptr_t GetGoldBase() { return g_ap_gold_base; }

std::uintptr_t GetCharacterStatsBase() { return g_ap_character_base; }

bool ApplyGoldAmountLocked(unsigned amount) {
    if (amount == 0) {
        return true;
    }
    if (g_ap_gold_base == 0) {
        return false;
    }

    const auto address = g_ap_gold_base + kGoldValueOffset;
    uint32_t current = 0;
    __try {
        current = *reinterpret_cast<volatile uint32_t*>(address);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        LogWarn("Failed to read gold at 0x%08X", static_cast<unsigned>(address));
        return false;
    }

    unsigned long long next = static_cast<unsigned long long>(current) + amount;
    if (next > kGoldMax) {
        next = kGoldMax;
    }

    DWORD old_protect = 0;
    if (!VirtualProtect(reinterpret_cast<void*>(address), sizeof(uint32_t), PAGE_EXECUTE_READWRITE, &old_protect)) {
        LogWarn("VirtualProtect failed for gold write at 0x%08X", static_cast<unsigned>(address));
        return false;
    }
    *reinterpret_cast<volatile uint32_t*>(address) = static_cast<uint32_t>(next);
    VirtualProtect(reinterpret_cast<void*>(address), sizeof(uint32_t), old_protect, &old_protect);

    return true;
}

bool AddGoldAmount(unsigned amount) {
    if (amount == 0) {
        return true;
    }

    std::lock_guard<std::mutex> lock(g_gold_mutex);
    if (g_ap_gold_base == 0) {
        const unsigned long long queued = static_cast<unsigned long long>(g_pending_gold) + amount;
        g_pending_gold = queued > kGoldMax ? kGoldMax : static_cast<unsigned>(queued);
        LogInfo("Gold +%u queued (open Status/menu once so GetGoldPtrAOB captures GoldPtr; pending=%u)",
                amount, g_pending_gold);
        return true;
    }
    return ApplyGoldAmountLocked(amount);
}

void FlushPendingGold() {
    std::lock_guard<std::mutex> lock(g_gold_mutex);
    if (g_ap_gold_base == 0 || g_pending_gold == 0) {
        return;
    }
    const unsigned amount = g_pending_gold;
    g_pending_gold = 0;
    ApplyGoldAmountLocked(amount);
}

}  // namespace grandia_ap

extern "C" void ApOnGoldPtrCaptured() {
    grandia_ap::FlushPendingGold();
}
