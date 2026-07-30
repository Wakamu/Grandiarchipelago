#include "menu_save_party_tab.h"

#include "d3d_overlay.h"
#include "game_memory.h"
#include "log.h"
#include "party_custom.h"

#include <Windows.h>

#include "xinput_dyn.h"
#include <Xinput.h>

#include <cstdint>
#include <cstdio>
#include <cstring>

namespace grandia_ap {
namespace {

// Save MC strip: MC1 | MC2 | Party. See docs/save_menu_party_checklist.md.

constexpr std::uintptr_t kPreferredImageBase = 0x400000u;
constexpr std::uintptr_t VaToRva(std::uintptr_t preferred_va) {
    return preferred_va - kPreferredImageBase;
}

constexpr std::uintptr_t kSlotListNavRva = 0x65AE0u;
constexpr std::uintptr_t kSlotListResumeRva = 0x65B25u;
constexpr size_t kSlotListNavSize = 69;

constexpr std::uintptr_t kPage2NavRva = 0x65210u;
constexpr std::uintptr_t kPage2ResumeRva = 0x65255u;
constexpr size_t kPage2NavSize = 69;

constexpr std::uintptr_t kSaveGatedNavRva = 0x6531Eu;
constexpr std::uintptr_t kSaveGatedResumeRva = 0x6535Eu;
constexpr size_t kSaveGatedNavSize = 64;

constexpr std::uintptr_t kLateMcNavRva = 0x661AEu;
constexpr std::uintptr_t kLateMcNavResumeRva = 0x661EBu;
constexpr size_t kLateMcNavSize = 61;

constexpr std::uintptr_t kStripInitCallRva = 0x64E8Au;  // call +66D60
constexpr std::uintptr_t kStripInitFnRva = 0x66D60u;
constexpr std::uintptr_t kAfterStripInitRva = 0x64E8Fu;

constexpr std::uintptr_t kSlotPadLoadRva = 0x658B8u;  // mov eax,[719444]
constexpr std::uintptr_t kSlotPadResumeRva = 0x658BDu;

constexpr std::uintptr_t kMcSwitchRva = 0x64B80u;
constexpr std::uintptr_t kRvaStripIndex = VaToRva(0x63FAB4u);
constexpr std::uintptr_t kRvaPad = VaToRva(0x719444u);
constexpr std::uintptr_t kRvaTabCount = VaToRva(0x71CD4Au);
constexpr std::uintptr_t kRvaMenuType = VaToRva(0x6C3018u);

// Widget port clamps inside +0x2300.
constexpr std::uintptr_t kClampAndEcx1Rvas[] = {0x2415u, 0x2585u, 0x26ACu, 0x298Bu};

constexpr uint32_t kPadLeft = 0x8004u;
constexpr uint32_t kPadRight = 0x2008u;
constexpr uint32_t kPadUp = 0x1000u;
constexpr uint32_t kPadDown = 0x4000u;
constexpr uint32_t kPadConfirm = 0x40u;
constexpr uint32_t kPadCancel = 0x2000u;  // B / Back
constexpr uint32_t kPadSelect = 0x800u;   // Select / Share (XINPUT BACK edge)

constexpr std::uintptr_t kRvaUiBusy = VaToRva(0x71942Cu);

enum class PartyRowKind : uint8_t { Slot, Add, Remove };

struct PartyRow {
    PartyRowKind kind = PartyRowKind::Slot;
    uint8_t slot = 0;
};

struct NavHook {
    std::uintptr_t site_rva = 0;
    std::uintptr_t resume_rva = 0;
    size_t size = 0;
    const char* label = "";
    uint8_t original[128]{};
    void* site = nullptr;
    void* trampoline = nullptr;
    void* resume = nullptr;
    void* detour = nullptr;
};

bool g_installed = false;
bool g_party_panel_open = false;
bool g_save_strip_active = false;
uint32_t g_pad_prev = 0;
uint32_t g_filtered_pad = 0;
bool g_select_prev_down = false;
bool g_l1_prev_down = false;
bool g_r1_prev_down = false;
int g_party_cursor = 0;
PartyRow g_party_rows[12]{};
int g_party_row_count = 0;

uint8_t g_clamp_originals[4]{};
bool g_clamps_patched = false;

uint8_t g_init_call_original[5]{};
void* g_init_call_site = nullptr;
void* g_strip_init_fn = nullptr;
void* g_after_strip_init = nullptr;

uint8_t g_slot_pad_original[5]{};
void* g_slot_pad_site = nullptr;
void* g_slot_pad_resume = nullptr;

void* g_mc_switch = nullptr;
uint32_t* g_strip_index = nullptr;
uint32_t* g_pad = nullptr;
uint8_t* g_tab_count = nullptr;
uint8_t* g_menu_type = nullptr;
uint32_t* g_ui_busy = nullptr;

void* g_slot_resume = nullptr;
void* g_slot_tramp = nullptr;
void* g_page2_resume = nullptr;
void* g_page2_tramp = nullptr;
void* g_gated_resume = nullptr;
void* g_gated_tramp = nullptr;
void* g_late_resume = nullptr;
void* g_late_tramp = nullptr;

int g_last_logged_strip = -1;

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

bool GamepadSelectDown() {
    for (DWORD i = 0; i < XUSER_MAX_COUNT; ++i) {
        XINPUT_STATE state{};
        if (XInputGetStateDyn(i, &state) != ERROR_SUCCESS) {
            continue;
        }
        // Match movie_skip hotkey: Select/Back = XINPUT_GAMEPAD_BACK.
        if (state.Gamepad.wButtons & XINPUT_GAMEPAD_BACK) {
            return true;
        }
    }
    return false;
}

bool GamepadL1Down() {
    for (DWORD i = 0; i < XUSER_MAX_COUNT; ++i) {
        XINPUT_STATE state{};
        if (XInputGetStateDyn(i, &state) != ERROR_SUCCESS) {
            continue;
        }
        if (state.Gamepad.wButtons & XINPUT_GAMEPAD_LEFT_SHOULDER) {
            return true;
        }
    }
    return false;
}

bool GamepadR1Down() {
    for (DWORD i = 0; i < XUSER_MAX_COUNT; ++i) {
        XINPUT_STATE state{};
        if (XInputGetStateDyn(i, &state) != ERROR_SUCCESS) {
            continue;
        }
        if (state.Gamepad.wButtons & XINPUT_GAMEPAD_RIGHT_SHOULDER) {
            return true;
        }
    }
    return false;
}

void* MakeTrampoline(const uint8_t* stolen, size_t stolen_size, void* continue_at) {
    void* mem = VirtualAlloc(nullptr, stolen_size + 16, MEM_COMMIT | MEM_RESERVE,
                             PAGE_EXECUTE_READWRITE);
    if (!mem) {
        return nullptr;
    }
    auto* tramp = reinterpret_cast<uint8_t*>(mem);
    std::memcpy(tramp, stolen, stolen_size);
    tramp[stolen_size] = 0xE9;
    const auto rel =
        static_cast<int32_t>(reinterpret_cast<uint8_t*>(continue_at) - (tramp + stolen_size + 5));
    std::memcpy(tramp + stolen_size + 1, &rel, sizeof(rel));
    FlushInstructionCache(GetCurrentProcess(), mem, stolen_size + 16);
    return mem;
}

bool WriteJumpAndNopRest(void* site, void* destination, size_t region_size) {
    if (region_size < 5 || region_size > 128) {
        return false;
    }
    uint8_t patch[128]{};
    std::memset(patch, 0x90, region_size);
    patch[0] = 0xE9;
    const auto rel = static_cast<int32_t>(reinterpret_cast<uint8_t*>(destination) -
                                          (reinterpret_cast<uint8_t*>(site) + 5));
    std::memcpy(patch + 1, &rel, sizeof(rel));
    return WriteBytes(site, patch, region_size);
}

bool WriteJump5(void* site, void* destination, uint8_t* original_out) {
    if (original_out) {
        std::memcpy(original_out, site, 5);
    }
    uint8_t patch[5]{0xE9, 0, 0, 0, 0};
    const auto rel = static_cast<int32_t>(reinterpret_cast<uint8_t*>(destination) -
                                          (reinterpret_cast<uint8_t*>(site) + 5));
    std::memcpy(patch + 1, &rel, sizeof(rel));
    return WriteBytes(site, patch, 5);
}

void RebuildPartyRows() {
    g_party_row_count = 0;
    const uint8_t n = PartyEditCount();
    for (uint8_t i = 0; i < n && g_party_row_count < 12; ++i) {
        g_party_rows[g_party_row_count++] = {PartyRowKind::Slot, i};
    }
    if (n < PartyMaxSlots() && g_party_row_count < 12) {
        g_party_rows[g_party_row_count++] = {PartyRowKind::Add, 0};
    }
    if (n > 1 && g_party_row_count < 12) {
        g_party_rows[g_party_row_count++] = {PartyRowKind::Remove, 0};
    }
    if (g_party_cursor >= g_party_row_count) {
        g_party_cursor = g_party_row_count > 0 ? g_party_row_count - 1 : 0;
    }
    if (g_party_cursor < 0) {
        g_party_cursor = 0;
    }
}

void RefreshPartyOverlay() {
    RebuildPartyRows();
    ClearD3dOverlayToast();

    char line_bufs[14][96]{};
    const char* lines[14]{};
    unsigned rgbs[14]{};
    std::size_t count = 0;

    auto push = [&](const char* text, unsigned rgb) {
        if (count >= 14) {
            return;
        }
        std::snprintf(line_bufs[count], sizeof(line_bufs[count]), "%s", text);
        lines[count] = line_bufs[count];
        rgbs[count] = rgb;
        ++count;
    };

    push("SAVE — Party", 0x7CFC00u);
    push("Up/Down move   L1/R1 change char", 0xA0A0A0u);

    for (int i = 0; i < g_party_row_count; ++i) {
        const PartyRow& row = g_party_rows[i];
        const bool sel = (i == g_party_cursor);
        const char* mark = sel ? ">" : " ";
        char buf[96]{};
        unsigned rgb = sel ? 0xFFE528u : 0xE0E0E0u;
        if (row.kind == PartyRowKind::Slot) {
            const uint8_t id = PartyEditIdAt(row.slot);
            std::snprintf(buf, sizeof(buf), "%s Slot %u: %s", mark,
                          static_cast<unsigned>(row.slot + 1), PartyCharName(id));
            if (sel) {
                PartyEditSetSelectedSlot(row.slot);
            }
        } else if (row.kind == PartyRowKind::Add) {
            std::snprintf(buf, sizeof(buf), "%s [+] Add member", mark);
            rgb = sel ? 0x7CFC00u : 0x90EE90u;
        } else {
            std::snprintf(buf, sizeof(buf), "%s [-] Remove member", mark);
            rgb = sel ? 0xFA8072u : 0xE9967Au;
        }
        push(buf, rgb);
    }

    SetD3dCenterPanel(lines, rgbs, count);
}

void ClosePartyToMc2() {
    if (g_strip_index) {
        *g_strip_index = 1;
    }
#if defined(_M_IX86)
    if (g_mc_switch) {
        __asm {
            mov ecx, 1
            call dword ptr [g_mc_switch]
        }
    }
#endif
}

extern "C" void ApSavePartyPanelEnter() {
    if (!IsPartyOverlayEnabled()) {
        return;
    }
    if (!g_party_panel_open) {
        g_party_panel_open = true;
        g_party_cursor = 0;
        PartyUiEnsureEnabled();
        LogInfo("Save MC strip: Party panel open");
    }
    RefreshPartyOverlay();
    g_last_logged_strip = 2;
}

extern "C" void ApSavePartyPanelLeave() {
    if (!g_party_panel_open) {
        ClearD3dCenterPanel();
        return;
    }
    // Leaving Party applies the current edit buffer.
    PartyUiApplyQuiet();
    g_party_panel_open = false;
    g_pad_prev = 0;
    ClearD3dCenterPanel();
    ClearD3dOverlayToast();
    LogInfo("Save MC strip: Party panel closed (roster applied)");
}

extern "C" uint32_t ApSavePartyFilterSlotPad(uint32_t pad) {
    const uint32_t edge = pad & ~g_pad_prev;

    if (!IsPartyOverlayEnabled()) {
        if (g_party_panel_open) {
            ApSavePartyPanelLeave();
        }
        if (g_strip_index && *g_strip_index >= 2) {
            *g_strip_index = 1;
        }
        g_pad_prev = pad;
        return pad;
    }

    if (!g_strip_index || *g_strip_index != 2) {
        if (g_party_panel_open) {
            ApSavePartyPanelLeave();
        }
        g_pad_prev = pad;
        return pad;
    }

    if (!g_party_panel_open) {
        ApSavePartyPanelEnter();
    }

    g_pad_prev = pad;

    RebuildPartyRows();

    // Right is 0x2008 and Cancel/Back is 0x2000 — Cancel bits are a subset of Right,
    // so test the distinctive low bit before treating input as Back.
    const bool pad_right = (edge & 0x0008u) != 0;
    const bool pad_cancel = ((edge & 0x2000u) != 0) && !pad_right;

    if (pad_cancel) {
        // Back closes Party first (return to MC2), do not leave Save.
        ClosePartyToMc2();
        ApSavePartyPanelLeave();
        // Block cancel + strip L/R so Save/MC nav do not also fire this frame.
        return pad & ~(kPadCancel | kPadUp | kPadDown | kPadConfirm | kPadLeft | kPadRight);
    }

    if (edge & kPadUp) {
        if (g_party_row_count > 0) {
            g_party_cursor = (g_party_cursor + g_party_row_count - 1) % g_party_row_count;
        }
        RefreshPartyOverlay();
    } else if (edge & kPadDown) {
        if (g_party_row_count > 0) {
            g_party_cursor = (g_party_cursor + 1) % g_party_row_count;
        }
        RefreshPartyOverlay();
    } else if (edge & kPadConfirm) {
        if (g_party_cursor >= 0 && g_party_cursor < g_party_row_count) {
            const PartyRow& row = g_party_rows[g_party_cursor];
            if (row.kind == PartyRowKind::Add) {
                PartyUiAddMemberQuiet();
                g_party_cursor = static_cast<int>(PartyEditCount()) - 1;
            } else if (row.kind == PartyRowKind::Remove) {
                PartyUiRemoveMemberQuiet();
            }
            RefreshPartyOverlay();
        }
    } else {
        // Character switching on Party slot line: use L1/R1 shoulder buttons.
        const bool l1_down = GamepadL1Down();
        const bool r1_down = GamepadR1Down();
        const bool l1_edge = l1_down && !g_l1_prev_down;
        const bool r1_edge = r1_down && !g_r1_prev_down;
        g_l1_prev_down = l1_down;
        g_r1_prev_down = r1_down;

        if ((l1_edge || r1_edge) && g_party_cursor >= 0 &&
            g_party_cursor < g_party_row_count && g_party_rows[g_party_cursor].kind ==
                PartyRowKind::Slot) {
            PartyEditSetSelectedSlot(g_party_rows[g_party_cursor].slot);
            PartyUiCycleCharQuiet(l1_edge ? -1 : 1);
            RefreshPartyOverlay();
        }
    }

    // While Party tab is selected, never let MC strip L/R through — nav hooks also
    // skip L/R when [63FAB4]==2 (they read raw [719444] after this filter).
    return pad & ~(kPadUp | kPadDown | kPadConfirm | kPadCancel | kPadLeft | kPadRight |
                   kPadSelect);
}

extern "C" void ApSaveStripAfterInit() {
    if (g_menu_type && *g_menu_type == 3 && IsPartyOverlayEnabled()) {
        if (g_tab_count) {
            *g_tab_count = 3;
        }
        g_save_strip_active = true;
    } else {
        g_save_strip_active = false;
    }
}

#if defined(_M_IX86)

extern "C" __declspec(naked) void ApSaveStripInitDetour() {
    __asm {
        call dword ptr [g_strip_init_fn]
        call ApSaveStripAfterInit
        jmp dword ptr [g_after_strip_init]
    }
}

extern "C" __declspec(naked) void ApSaveSlotPadDetour() {
    __asm {
        mov eax, dword ptr [g_pad]
        mov eax, dword ptr [eax]
        push eax
        call ApSavePartyFilterSlotPad
        add esp, 4
        mov dword ptr [g_filtered_pad], eax
        // Ensure other save/load nav code reads the filtered pad word too.
        // This prevents L/R from closing the Party overlay.
        mov ecx, dword ptr [g_pad]
        mov dword ptr [ecx], eax
        mov eax, dword ptr [g_filtered_pad]
        jmp dword ptr [g_slot_pad_resume]
    }
}

extern "C" __declspec(naked) void ApSaveSlotListMcNavDetour() {
    __asm {
        mov ecx, dword ptr [g_strip_index]
        movzx edx, bl
        cmp dword ptr [ebp - 0CCh], 0
        jne slot_no_party
        cmp dword ptr [ecx], 2
        jne slot_no_party
        jmp slot_done
    slot_no_party:

        test eax, 8004h
        je slot_right
        test edx, edx
        je slot_right
        xor ecx, ecx
        call dword ptr [g_mc_switch]
        mov eax, dword ptr [g_pad]
        mov eax, dword ptr [eax]
        mov ecx, dword ptr [g_strip_index]
        mov dword ptr [ecx], 0
        jmp slot_right

    slot_right:
        test eax, 2008h
        je slot_done
        mov ecx, 1
        call dword ptr [g_mc_switch]
        mov eax, dword ptr [g_pad]
        mov eax, dword ptr [eax]
        mov ecx, dword ptr [g_strip_index]
        mov dword ptr [ecx], 1

    slot_done:
        jmp dword ptr [g_slot_resume]
    }
}

extern "C" __declspec(naked) void ApSavePage2McNavDetour() {
    __asm {
        mov ecx, dword ptr [g_strip_index]
        movzx edx, bl
        cmp dword ptr [ebp - 0CCh], 0
        jne page2_no_party
        cmp dword ptr [ecx], 2
        jne page2_no_party
        jmp page2_done
    page2_no_party:

        test eax, 8004h
        je page2_right
        test edx, edx
        je page2_right
        xor ecx, ecx
        call dword ptr [g_mc_switch]
        mov eax, dword ptr [g_pad]
        mov eax, dword ptr [eax]
        mov ecx, dword ptr [g_strip_index]
        mov dword ptr [ecx], 0
        jmp page2_right

    page2_right:
        test eax, 2008h
        je page2_done
        mov ecx, 1
        call dword ptr [g_mc_switch]
        mov eax, dword ptr [g_pad]
        mov eax, dword ptr [eax]
        mov ecx, dword ptr [g_strip_index]
        mov dword ptr [ecx], 1

    page2_done:
        jmp dword ptr [g_page2_resume]
    }
}

extern "C" __declspec(naked) void ApSaveGatedMcNavDetour() {
    __asm {
        mov ecx, dword ptr [g_strip_index]
        movzx edx, bl
        cmp dword ptr [ebp - 0CCh], 0
        jne gated_no_party
        cmp dword ptr [ecx], 2
        jne gated_no_party
        jmp gated_done
    gated_no_party:

        test eax, 8004h
        je gated_right
        test edx, edx
        je gated_right
        xor ecx, ecx
        call dword ptr [g_mc_switch]
        mov eax, dword ptr [g_pad]
        mov eax, dword ptr [eax]
        mov ecx, dword ptr [g_strip_index]
        mov dword ptr [ecx], 0
        jmp gated_right

    gated_right:
        test eax, 2008h
        je gated_done
        mov ecx, 1
        call dword ptr [g_mc_switch]
        mov eax, dword ptr [g_pad]
        mov eax, dword ptr [eax]
        mov ecx, dword ptr [g_strip_index]
        mov dword ptr [ecx], 1

    gated_done:
        jmp dword ptr [g_gated_resume]
    }
}

extern "C" __declspec(naked) void ApSaveLateMcNavDetour() {
    __asm {
        cmp dword ptr [ebp - 0CCh], 0
        jne late_stock

        mov ecx, dword ptr [g_strip_index]
        cmp dword ptr [ecx], 2
        je late_done

        movzx edx, bl

        test eax, 8004h
        je late_right
        test edx, edx
        je late_right
        xor ecx, ecx
        call dword ptr [g_mc_switch]
        mov ecx, dword ptr [0x63f034]
        mov eax, dword ptr [g_pad]
        mov eax, dword ptr [eax]
        jmp late_right

    late_right:
        test eax, 2008h
        je late_done
        test edx, edx
        jne late_done
        mov ecx, 1
        call dword ptr [g_mc_switch]
        mov ecx, dword ptr [0x63f034]
        mov eax, dword ptr [g_pad]
        mov eax, dword ptr [eax]

    late_done:
        jmp dword ptr [g_late_resume]

    late_stock:
        jmp dword ptr [g_late_tramp]
    }
}

#endif  // _M_IX86

NavHook g_hooks[] = {
    {kSlotListNavRva, kSlotListResumeRva, kSlotListNavSize, "+0x65AE0"},
    {kPage2NavRva, kPage2ResumeRva, kPage2NavSize, "+0x65210"},
    {kSaveGatedNavRva, kSaveGatedResumeRva, kSaveGatedNavSize, "+0x6531E"},
    {kLateMcNavRva, kLateMcNavResumeRva, kLateMcNavSize, "+0x661A9"},
};

bool ExpectNavPrologue(const uint8_t* site, const char* label) {
    const bool test_eax_lr =
        site[0] == 0xA9 && site[1] == 0x04 && site[2] == 0x80 && site[3] == 0x00 &&
        site[4] == 0x00;
    const bool mov_eax_pad =
        site[0] == 0xA1 && site[1] == 0x44 && site[2] == 0x94 && site[3] == 0x71 &&
        site[4] == 0x00;
    if (!test_eax_lr && !mov_eax_pad) {
        LogWarn("Save party tab: prologue mismatch at %s (got %02X %02X %02X %02X %02X)", label,
                site[0], site[1], site[2], site[3], site[4]);
        return false;
    }
    return true;
}

bool PatchClamps(std::uintptr_t base) {
    for (size_t i = 0; i < 4; ++i) {
        auto* site = reinterpret_cast<uint8_t*>(base + kClampAndEcx1Rvas[i]);
        // and ecx, 1
        if (site[0] != 0x83 || site[1] != 0xE1 || site[2] != 0x01) {
            LogWarn("Save party tab: clamp mismatch at +0x%X",
                    static_cast<unsigned>(kClampAndEcx1Rvas[i]));
            return false;
        }
        g_clamp_originals[i] = site[2];
        const uint8_t next = 0x03;  // and ecx, 3 → allow index 0..2
        if (!WriteBytes(site + 2, &next, 1)) {
            return false;
        }
    }
    g_clamps_patched = true;
    return true;
}

void RestoreClamps(std::uintptr_t base) {
    if (!g_clamps_patched) {
        return;
    }
    for (size_t i = 0; i < 4; ++i) {
        auto* site = reinterpret_cast<uint8_t*>(base + kClampAndEcx1Rvas[i]);
        WriteBytes(site + 2, &g_clamp_originals[i], 1);
    }
    g_clamps_patched = false;
}

void FreeAllTrampolines() {
    for (auto& h : g_hooks) {
        if (h.trampoline) {
            VirtualFree(h.trampoline, 0, MEM_RELEASE);
            h.trampoline = nullptr;
        }
        h.site = nullptr;
    }
    g_slot_tramp = nullptr;
    g_page2_tramp = nullptr;
    g_gated_tramp = nullptr;
}

}  // namespace

bool InstallMenuSavePartyTabHook() {
    if (g_installed) {
        return true;
    }

#if !defined(_M_IX86)
    LogWarn("Save party tab: requires 32-bit build");
    return false;
#else
    const std::uintptr_t base = GetGrandiaModuleBase();
    if (base == 0) {
        LogWarn("Save party tab: grandia base unknown");
        return false;
    }

    g_mc_switch = reinterpret_cast<void*>(base + kMcSwitchRva);
    g_strip_index = reinterpret_cast<uint32_t*>(base + kRvaStripIndex);
    g_pad = reinterpret_cast<uint32_t*>(base + kRvaPad);
    g_tab_count = reinterpret_cast<uint8_t*>(base + kRvaTabCount);
    g_menu_type = reinterpret_cast<uint8_t*>(base + kRvaMenuType);
    g_ui_busy = reinterpret_cast<uint32_t*>(base + kRvaUiBusy);
    g_strip_init_fn = reinterpret_cast<void*>(base + kStripInitFnRva);
    g_after_strip_init = reinterpret_cast<void*>(base + kAfterStripInitRva);
    g_slot_pad_resume = reinterpret_cast<void*>(base + kSlotPadResumeRva);

    if (!PatchClamps(base)) {
        return false;
    }

    // After call +66D60: Save (bl&0x7F==0) → tab count 3.
    g_init_call_site = reinterpret_cast<void*>(base + kStripInitCallRva);
    auto* init_bytes = reinterpret_cast<uint8_t*>(g_init_call_site);
    if (init_bytes[0] != 0xE8) {
        LogWarn("Save party tab: strip init call mismatch at +0x64E8A");
        RestoreClamps(base);
        return false;
    }
    if (!WriteJump5(g_init_call_site, reinterpret_cast<void*>(&ApSaveStripInitDetour),
                    g_init_call_original)) {
        RestoreClamps(base);
        return false;
    }

    // Slot-list pad load: filter when Party selected.
    g_slot_pad_site = reinterpret_cast<void*>(base + kSlotPadLoadRva);
    auto* pad_bytes = reinterpret_cast<uint8_t*>(g_slot_pad_site);
    if (pad_bytes[0] != 0xA1) {
        LogWarn("Save party tab: slot pad load mismatch at +0x658B8");
        WriteBytes(g_init_call_site, g_init_call_original, 5);
        RestoreClamps(base);
        return false;
    }
    if (!WriteJump5(g_slot_pad_site, reinterpret_cast<void*>(&ApSaveSlotPadDetour),
                    g_slot_pad_original)) {
        WriteBytes(g_init_call_site, g_init_call_original, 5);
        RestoreClamps(base);
        return false;
    }

    void* detours[4] = {reinterpret_cast<void*>(&ApSaveSlotListMcNavDetour),
                        reinterpret_cast<void*>(&ApSavePage2McNavDetour),
                        reinterpret_cast<void*>(&ApSaveGatedMcNavDetour),
                        reinterpret_cast<void*>(&ApSaveLateMcNavDetour)};

    for (size_t i = 0; i < 4; ++i) {
        auto& h = g_hooks[i];
        h.detour = detours[i];
        h.site = reinterpret_cast<void*>(base + h.site_rva);
        h.resume = reinterpret_cast<void*>(base + h.resume_rva);
        auto* bytes = reinterpret_cast<uint8_t*>(h.site);
        if (!ExpectNavPrologue(bytes, h.label)) {
            WriteBytes(g_init_call_site, g_init_call_original, 5);
            WriteBytes(g_slot_pad_site, g_slot_pad_original, 5);
            RestoreClamps(base);
            FreeAllTrampolines();
            return false;
        }
        std::memcpy(h.original, bytes, h.size);
        h.trampoline = MakeTrampoline(h.original, h.size, h.resume);
        if (!h.trampoline) {
            WriteBytes(g_init_call_site, g_init_call_original, 5);
            WriteBytes(g_slot_pad_site, g_slot_pad_original, 5);
            RestoreClamps(base);
            FreeAllTrampolines();
            return false;
        }
    }

    g_slot_resume = g_hooks[0].resume;
    g_slot_tramp = g_hooks[0].trampoline;
    g_page2_resume = g_hooks[1].resume;
    g_page2_tramp = g_hooks[1].trampoline;
    g_gated_resume = g_hooks[2].resume;
    g_gated_tramp = g_hooks[2].trampoline;
    g_late_resume = g_hooks[3].resume;
    g_late_tramp = g_hooks[3].trampoline;

    for (size_t i = 0; i < 4; ++i) {
        auto& h = g_hooks[i];
        if (!WriteJumpAndNopRest(h.site, h.detour, h.size)) {
            for (size_t j = 0; j < i; ++j) {
                WriteBytes(g_hooks[j].site, g_hooks[j].original, g_hooks[j].size);
            }
            WriteBytes(g_init_call_site, g_init_call_original, 5);
            WriteBytes(g_slot_pad_site, g_slot_pad_original, 5);
            RestoreClamps(base);
            FreeAllTrampolines();
            return false;
        }
    }

    g_installed = true;
    LogInfo(
        "Save party tab: nav+count+panel active (Save count=3, clamps 0..2, Party overlay on "
        "index 2)");
    return true;
#endif
}

void RemoveMenuSavePartyTabHook() {
    if (!g_installed) {
        return;
    }
    ApSavePartyPanelLeave();
    for (auto& h : g_hooks) {
        if (h.site) {
            WriteBytes(h.site, h.original, h.size);
        }
    }
    if (g_init_call_site) {
        WriteBytes(g_init_call_site, g_init_call_original, 5);
    }
    if (g_slot_pad_site) {
        WriteBytes(g_slot_pad_site, g_slot_pad_original, 5);
    }
    RestoreClamps(GetGrandiaModuleBase());
    FreeAllTrampolines();
    g_init_call_site = nullptr;
    g_slot_pad_site = nullptr;
    g_installed = false;
    g_last_logged_strip = -1;
    LogInfo("Save party tab: hooks removed");
}

bool IsMenuSavePartyTabHookInstalled() { return g_installed; }

void PollMenuSavePartyTab() {
    if (!g_installed) {
        return;
    }

    if (!IsPartyOverlayEnabled()) {
        if (g_party_panel_open) {
            ApSavePartyPanelLeave();
        }
        if (g_strip_index && *g_strip_index >= 2) {
            *g_strip_index = 1;
        }
        g_select_prev_down = GamepadSelectDown();
        return;
    }

    const bool select_down = GamepadSelectDown();
    const bool select_edge = select_down && !g_select_prev_down;
    g_select_prev_down = select_down;

    const bool ui_open = g_ui_busy && *g_ui_busy != 0;
    const bool save_menu = g_menu_type && *g_menu_type == 3;
    const bool load_menu = g_menu_type && *g_menu_type == 4;
    if (save_menu) {
        g_save_strip_active = true;
    } else if (load_menu || !ui_open) {
        g_save_strip_active = false;
    }

    if (ui_open && save_menu && !g_party_panel_open && g_strip_index &&
        *g_strip_index < 2 && select_edge) {
        *g_strip_index = 2;
        ApSavePartyPanelEnter();
        return;
    }

    if (ui_open && g_strip_index && *g_strip_index == 2) {
        if (save_menu) {
            if (!g_party_panel_open) {
                ApSavePartyPanelEnter();
            }
        } else if (load_menu) {
            // Non-Save screens must never linger on the synthetic Party tab.
            ClosePartyToMc2();
            ApSavePartyPanelLeave();
            return;
        }
    }

    if (!g_party_panel_open) {
        return;
    }
    // Strip left Party, or Save/Load UI closed (Back exited menu).
    if ((g_strip_index && *g_strip_index != 2) || (g_ui_busy && *g_ui_busy == 0)) {
        ApSavePartyPanelLeave();
    }
}

}  // namespace grandia_ap
