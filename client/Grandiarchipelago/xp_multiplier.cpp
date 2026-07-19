#include "xp_multiplier.h"

#include "game_memory.h"
#include "log.h"

#include <Windows.h>

#include <atomic>
#include <cstdint>
#include <cstring>

#if defined(_M_IX86)
extern "C" {
void* g_ap_magic_xp_resume = nullptr;
void* g_ap_skill_xp_resume = nullptr;
void* g_ap_level_xp_fight_resume = nullptr;

unsigned ApGetMagicXpMultiplier();
unsigned ApGetSkillXpMultiplier();
unsigned ApGetLevelXpMultiplier();
void ApMagicXpDetour();
void ApSkillXpDetour();
void ApLevelXpFightDetour();
}
#endif

namespace grandia_ap {

namespace {

constexpr std::uintptr_t kMagicXpAddRva = 0xA4E96u;
constexpr size_t kMagicXpStolen = 10;
constexpr size_t kMagicXpPatch = 5;
constexpr uint8_t kMagicXpBytes[] = {0x8B, 0x45, 0xF8, 0x66, 0x01, 0x07, 0x66, 0x01, 0x47, 0x0A};

// Skill bar + results UI session field (+0x18e), through mov [ebp-0x10],eax.
constexpr std::uintptr_t kSkillXpAddRva = 0xA4FA7u;
constexpr size_t kSkillXpStolen = 22;
constexpr size_t kSkillXpPatch = 5;
constexpr uint8_t kSkillXpBytes[] = {0x05, 0xBF, 0x00, 0x00, 0x00, 0x66, 0x01, 0x34, 0x43,
                                     0x8D, 0x04, 0x43, 0x66, 0x01, 0xB3, 0x8E, 0x01, 0x00, 0x00,
                                     0x89, 0x45, 0xF0};

// In-fight kill EXP: movzx ecx,[esi+0x17e]; add [eax+0x88], ecx
constexpr std::uintptr_t kLevelXpFightRva = 0x1387C3u;
constexpr size_t kLevelXpFightStolen = 6;
constexpr size_t kLevelXpFightPatch = 5;
constexpr uint8_t kLevelXpFightBytes[] = {0x01, 0x88, 0x88, 0x00, 0x00, 0x00};

std::atomic<unsigned> g_magic_xp_mult{1};
std::atomic<unsigned> g_skill_xp_mult{1};
std::atomic<unsigned> g_level_xp_mult{1};

void* g_magic_site = nullptr;
void* g_skill_site = nullptr;
void* g_level_fight_site = nullptr;

bool BytesMatch(const uint8_t* a, const uint8_t* b, size_t n) {
    return std::memcmp(a, b, n) == 0;
}

bool WriteJump(void* site, void* destination, size_t patch_size) {
    DWORD old_protect = 0;
    if (!VirtualProtect(site, patch_size, PAGE_EXECUTE_READWRITE, &old_protect)) {
        return false;
    }
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

void NopTail(void* site, size_t patch_size, size_t stolen_size) {
    if (stolen_size <= patch_size) {
        return;
    }
    DWORD old = 0;
    VirtualProtect(site, stolen_size, PAGE_EXECUTE_READWRITE, &old);
    auto* b = reinterpret_cast<uint8_t*>(site);
    for (size_t i = patch_size; i < stolen_size; ++i) {
        b[i] = 0x90;
    }
    VirtualProtect(site, stolen_size, old, &old);
    FlushInstructionCache(GetCurrentProcess(), site, stolen_size);
}

void RestoreSite(void* site, const uint8_t* original, size_t size) {
    if (!site) {
        return;
    }
    DWORD old = 0;
    VirtualProtect(site, size, PAGE_EXECUTE_READWRITE, &old);
    std::memcpy(site, original, size);
    VirtualProtect(site, size, old, &old);
    FlushInstructionCache(GetCurrentProcess(), site, size);
}

unsigned ClampMultiplier(unsigned multiplier) {
    if (multiplier < 1) {
        return 1;
    }
    if (multiplier > 100) {
        return 100;
    }
    return multiplier;
}

}  // namespace

void SetMagicXpMultiplier(unsigned multiplier) {
    const unsigned v = ClampMultiplier(multiplier);
    g_magic_xp_mult.store(v);
    LogInfo("CONFIG magic_xp_multiplier=%u", v);
}

void SetSkillXpMultiplier(unsigned multiplier) {
    const unsigned v = ClampMultiplier(multiplier);
    g_skill_xp_mult.store(v);
    LogInfo("CONFIG skill_xp_multiplier=%u", v);
}

void SetLevelXpMultiplier(unsigned multiplier) {
    const unsigned v = ClampMultiplier(multiplier);
    g_level_xp_mult.store(v);
    LogInfo("CONFIG level_xp_multiplier=%u", v);
}

unsigned GetMagicXpMultiplier() {
    return g_magic_xp_mult.load();
}

unsigned GetSkillXpMultiplier() {
    return g_skill_xp_mult.load();
}

unsigned GetLevelXpMultiplier() {
    return g_level_xp_mult.load();
}

bool InstallXpMultiplierHooks() {
#if !defined(_M_IX86)
    return false;
#else
    if (g_magic_site && g_skill_site && g_level_fight_site) {
        return true;
    }
    const std::uintptr_t base = GetGrandiaModuleBase();
    if (base == 0) {
        return false;
    }

    void* magic_site = reinterpret_cast<void*>(base + kMagicXpAddRva);
    void* skill_site = reinterpret_cast<void*>(base + kSkillXpAddRva);
    void* level_fight = reinterpret_cast<void*>(base + kLevelXpFightRva);

    if (!BytesMatch(reinterpret_cast<const uint8_t*>(magic_site), kMagicXpBytes, sizeof(kMagicXpBytes))) {
        LogWarn("Magic XP bytes mismatch at +0x%X", static_cast<unsigned>(kMagicXpAddRva));
        return false;
    }
    if (!BytesMatch(reinterpret_cast<const uint8_t*>(skill_site), kSkillXpBytes, sizeof(kSkillXpBytes))) {
        LogWarn("Skill XP bytes mismatch at +0x%X", static_cast<unsigned>(kSkillXpAddRva));
        return false;
    }
    if (!BytesMatch(reinterpret_cast<const uint8_t*>(level_fight), kLevelXpFightBytes,
                    sizeof(kLevelXpFightBytes))) {
        LogWarn("Level XP fight-add bytes mismatch at +0x%X",
                static_cast<unsigned>(kLevelXpFightRva));
        return false;
    }

    g_ap_magic_xp_resume = reinterpret_cast<void*>(base + kMagicXpAddRva + kMagicXpStolen);
    g_ap_skill_xp_resume = reinterpret_cast<void*>(base + kSkillXpAddRva + kSkillXpStolen);
    g_ap_level_xp_fight_resume =
        reinterpret_cast<void*>(base + kLevelXpFightRva + kLevelXpFightStolen);

    if (!WriteJump(magic_site, reinterpret_cast<void*>(&ApMagicXpDetour), kMagicXpPatch) ||
        !WriteJump(skill_site, reinterpret_cast<void*>(&ApSkillXpDetour), kSkillXpPatch) ||
        !WriteJump(level_fight, reinterpret_cast<void*>(&ApLevelXpFightDetour),
                   kLevelXpFightPatch)) {
        RestoreSite(magic_site, kMagicXpBytes, kMagicXpStolen);
        RestoreSite(skill_site, kSkillXpBytes, kSkillXpStolen);
        RestoreSite(level_fight, kLevelXpFightBytes, kLevelXpFightStolen);
        g_ap_magic_xp_resume = nullptr;
        g_ap_skill_xp_resume = nullptr;
        g_ap_level_xp_fight_resume = nullptr;
        LogWarn("Failed to patch one or more XP multiplier sites");
        return false;
    }
    NopTail(magic_site, kMagicXpPatch, kMagicXpStolen);
    NopTail(skill_site, kSkillXpPatch, kSkillXpStolen);
    NopTail(level_fight, kLevelXpFightPatch, kLevelXpFightStolen);

    g_magic_site = magic_site;
    g_skill_site = skill_site;
    g_level_fight_site = level_fight;
    LogInfo("XP multipliers hooked (magic +0x%X, skill +0x%X, level-fight +0x%X)",
            static_cast<unsigned>(kMagicXpAddRva), static_cast<unsigned>(kSkillXpAddRva),
            static_cast<unsigned>(kLevelXpFightRva));
    return true;
#endif
}

void RemoveXpMultiplierHooks() {
#if defined(_M_IX86)
    RestoreSite(g_magic_site, kMagicXpBytes, kMagicXpStolen);
    RestoreSite(g_skill_site, kSkillXpBytes, kSkillXpStolen);
    RestoreSite(g_level_fight_site, kLevelXpFightBytes, kLevelXpFightStolen);
    g_magic_site = nullptr;
    g_skill_site = nullptr;
    g_level_fight_site = nullptr;
    g_ap_magic_xp_resume = nullptr;
    g_ap_skill_xp_resume = nullptr;
    g_ap_level_xp_fight_resume = nullptr;
#endif
}

bool IsXpMultiplierHookInstalled() {
    return g_magic_site != nullptr && g_skill_site != nullptr && g_level_fight_site != nullptr;
}

}  // namespace grandia_ap

#if defined(_M_IX86)

extern "C" unsigned ApGetMagicXpMultiplier() {
    return grandia_ap::GetMagicXpMultiplier();
}

extern "C" unsigned ApGetSkillXpMultiplier() {
    return grandia_ap::GetSkillXpMultiplier();
}

extern "C" unsigned ApGetLevelXpMultiplier() {
    return grandia_ap::GetLevelXpMultiplier();
}

extern "C" __declspec(naked) void ApMagicXpDetour() {
    __asm {
        mov eax, dword ptr [ebp - 8]
        push eax
        call ApGetMagicXpMultiplier
        mov ecx, eax
        pop eax
        cmp ecx, 1
        jbe magic_add
        imul eax, ecx
        cmp eax, 0FFFFh
        jbe magic_add
        mov eax, 0FFFFh
    magic_add:
        add word ptr [edi], ax
        add word ptr [edi + 0Ah], ax
        jmp dword ptr [g_ap_magic_xp_resume]
    }
}

extern "C" __declspec(naked) void ApSkillXpDetour() {
    __asm {
        // EAX = weapon index; SI = skill XP gain.
        // Also update [ebx+0x18e] — results UI reads that as "weapon EXP gained".
        add eax, 0BFh
        push eax
        movzx eax, si
        push eax
        call ApGetSkillXpMultiplier
        mov ecx, eax
        pop eax
        cmp ecx, 1
        jbe skill_ready
        imul eax, ecx
        cmp eax, 0FFFFh
        jbe skill_ready
        mov eax, 0FFFFh
    skill_ready:
        mov esi, eax
        pop eax
        add word ptr [ebx + eax*2], si
        lea eax, [ebx + eax*2]
        add word ptr [ebx + 18Eh], si
        mov dword ptr [ebp - 10h], eax
        jmp dword ptr [g_ap_skill_xp_resume]
    }
}

// +0x1387C3: ECX = enemy XP (from [esi+0x17e]), EAX = battle struct.
// Multiply the kill credit into the running battle EXP at [eax+0x88].
extern "C" __declspec(naked) void ApLevelXpFightDetour() {
    __asm {
        push eax
        push ecx
        call ApGetLevelXpMultiplier
        mov edx, eax
        pop ecx
        pop eax
        cmp edx, 1
        jbe level_fight_add
        imul ecx, edx
    level_fight_add:
        add dword ptr [eax + 88h], ecx
        jmp dword ptr [g_ap_level_xp_fight_resume]
    }
}

#endif
