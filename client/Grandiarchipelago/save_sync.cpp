#include "save_sync.h"

#include "d3d_overlay.h"
#include "game_memory.h"
#include "log.h"
#include "pipe_bridge.h"

#include <Windows.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>

using FwriteFn = std::size_t(__cdecl*)(const void*, std::size_t, std::size_t, std::FILE*);
using FreadFn = std::size_t(__cdecl*)(void*, std::size_t, std::size_t, std::FILE*);
using FseekFn = int(__cdecl*)(std::FILE*, long, int);
using FtellFn = long(__cdecl*)(std::FILE*);
using FopenFn = std::FILE*(__cdecl*)(const char*, const char*);
using FcloseFn = int(__cdecl*)(std::FILE*);

namespace {

constexpr std::uintptr_t kSaveFsmEntryRva = 0x2300u;
constexpr std::uintptr_t kSaveFsmResumeRva = 0x2309u;  // after `sub esp, 0x274`
constexpr std::uintptr_t kSaveFwriteCallRva = 0x254Eu;
constexpr std::uintptr_t kSaveFwriteResumeRva = 0x2554u;
constexpr std::uintptr_t kSaveFopenCallRva = 0x2649u;  // load-path fopen (NULL → clean abort)
constexpr std::uintptr_t kSaveFopenResumeRva = 0x264Fu;
constexpr std::uintptr_t kSaveFreadCallRva = 0x2673u;
constexpr std::uintptr_t kSaveFreadResumeRva = 0x2679u;
// Confirm-Yes Loading UI stores start at +0x657DA (`mov [0x6c301c],2`).
// Allow: trampoline runs those original bytes, then continues at +0x657E1.
// Deny: skip the stores entirely and jump the shared epilogue (never enter Loading).
constexpr std::uintptr_t kSaveConfirmUiRva = 0x657DAu;
constexpr std::uintptr_t kSaveConfirmUiContinueRva = 0x657E1u;
constexpr std::uintptr_t kSaveConfirmUiResumeRva = 0x66731u;
constexpr size_t kCallIatPatchSize = 6;
constexpr size_t kFsmEntryPatchSize = 9;  // push ebp; mov ebp,esp; sub esp,0x274
constexpr size_t kConfirmUiPatchSize = 7;  // mov byte ptr [0x6c301c], 2
constexpr size_t kConfirmUiTrampolineSize = 16;  // 7 original + 5 jmp + pad

constexpr std::uintptr_t kPreferredImageBase = 0x400000u;
constexpr std::uintptr_t VaToRva(std::uintptr_t preferred_va) {
    return preferred_va - kPreferredImageBase;
}
// Globals as RVAs. Live address = GetGrandiaModuleBase() + RVA.
constexpr std::uintptr_t kRvaSaveUiPhase = VaToRva(0x6C301Cu);      // 0=select, 2=loading
constexpr std::uintptr_t kRvaSaveUiInProgress = VaToRva(0x6C2E04u); // 1 while load/save locked
constexpr std::uintptr_t kRvaSaveUiCursor = VaToRva(0x71D14Eu);
constexpr std::uintptr_t kRvaSaveSelectedSlot = VaToRva(0x6C301Eu);
constexpr std::uintptr_t kRvaSaveObject = VaToRva(0x718C40u);  // +0x8 = profile dir id, +0xc = load/save arm
// Runtime "%APPDATA%\GRANDIA1\Saves\" (trailing slash); profile subdir comes from save object +0x8.
constexpr std::uintptr_t kRvaSaveDirPrefix = VaToRva(0x6C1D00u);

template <typename T>
T* GamePtr(std::uintptr_t rva) {
    const std::uintptr_t base = grandia_ap::GetGrandiaModuleBase();
    if (base == 0) {
        return nullptr;
    }
    return reinterpret_cast<T*>(base + rva);
}

// FSM [object+0x22] / [0x718c62] ops (RE Jul 2026):
//   3 = confirm LOAD (sets up fread), 4 = SAVE (fwrite), 6 = list refresh, 2 = idle poll
constexpr unsigned kFsmOpConfirmLoad = 3;
constexpr unsigned kFsmOpSave = 4;
constexpr unsigned kFsmOpListRefresh = 6;

void* g_fwrite_hook_site = nullptr;
void* g_fread_hook_site = nullptr;
void* g_fopen_hook_site = nullptr;
void* g_confirm_ui_hook_site = nullptr;
void* g_fsm_hook_site = nullptr;
uint8_t* g_confirm_ui_trampoline = nullptr;
uint8_t g_fwrite_hook_original[8]{};
uint8_t g_fread_hook_original[8]{};
uint8_t g_fopen_hook_original[8]{};
uint8_t g_confirm_ui_hook_original[8]{};
uint8_t g_fsm_hook_original[16]{};

FwriteFn g_crt_fwrite = nullptr;
FreadFn g_crt_fread = nullptr;
FseekFn g_crt_fseek = nullptr;
FtellFn g_crt_ftell = nullptr;
FopenFn g_crt_fopen = nullptr;
FcloseFn g_crt_fclose = nullptr;

grandia_ap::ApSaveTrailerV1 g_trailer{};
grandia_ap::ApSaveTrailerV1 g_pending_trailer{};
bool g_trailer_present = false;
bool g_trailer_dirty = false;
bool g_pending_present = false;
int g_pending_slot = -1;
bool g_confirm_load_armed = false;
bool g_load_denied = false;  // set at op=3; confirm-UI hook skips Loading phase
bool g_seed_gate_ok = false; // early op=3 peek passed — skip fopen re-eval
bool g_load_committed = false;
uint32_t g_expected_seed_hash = 0;  // from CONFIG seed_hash (connected AP room+slot)

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

void* ReadIatFunction(void* call_site) {
    const auto* bytes = reinterpret_cast<const uint8_t*>(call_site);
    if (bytes[0] != 0xFF || bytes[1] != 0x15) {
        return nullptr;
    }
    std::uintptr_t iat_va = 0;
    std::memcpy(&iat_va, bytes + 2, sizeof(iat_va));
    void* fn = nullptr;
    __try {
        fn = *reinterpret_cast<void**>(iat_va);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
    return fn;
}

void InitEmptyTrailer() {
    std::memset(&g_trailer, 0, sizeof(g_trailer));
    g_trailer.magic[0] = 'G';
    g_trailer.magic[1] = 'A';
    g_trailer.magic[2] = 'P';
    g_trailer.magic[3] = '1';
    g_trailer.version = grandia_ap::kApSaveTrailerVersion;
    g_trailer_present = false;
    g_trailer_dirty = false;
}

void ClearPendingTrailer() {
    std::memset(&g_pending_trailer, 0, sizeof(g_pending_trailer));
    g_pending_present = false;
    g_pending_slot = -1;
}

void TryAnnounceSaveSync(bool force = false);

bool IsVanillaSaveIo(std::size_t elem_size, std::size_t count) {
    return elem_size * count == grandia_ap::kVanillaSaveSize;
}

bool PeekGap1Trailer(std::FILE* file, grandia_ap::ApSaveTrailerV1* out) {
    if (!file || !out || !g_crt_fread || !g_crt_fseek) {
        return false;
    }
    __try {
        if (g_crt_fseek(file, static_cast<long>(grandia_ap::kVanillaSaveSize), SEEK_SET) != 0) {
            return false;
        }
        grandia_ap::ApSaveTrailerV1 tmp{};
        const std::size_t got = g_crt_fread(&tmp, 1, sizeof(tmp), file);
        if (got != sizeof(tmp) || std::memcmp(tmp.magic, "GAP1", 4) != 0) {
            return false;
        }
        if (tmp.version != grandia_ap::kApSaveTrailerVersion) {
            return false;
        }
        *out = tmp;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void CachePendingTrailer(std::FILE* file) {
    if (!file || !g_crt_fread || !g_crt_fseek) {
        return;
    }

    grandia_ap::ApSaveTrailerV1 tmp{};
    if (!PeekGap1Trailer(file, &tmp)) {
        ClearPendingTrailer();
        // Keep FP at trailer offset (same as prior peek behavior).
        g_crt_fseek(file, static_cast<long>(grandia_ap::kVanillaSaveSize), SEEK_SET);
        return;
    }
    g_pending_trailer = tmp;
    g_pending_present = true;
    g_crt_fseek(file, static_cast<long>(grandia_ap::kVanillaSaveSize), SEEK_SET);
}

void RestoreSaveSelectUi() {
    auto* phase = GamePtr<uint8_t>(kRvaSaveUiPhase);
    auto* in_progress = GamePtr<uint8_t>(kRvaSaveUiInProgress);
    auto* cursor = GamePtr<uint8_t>(kRvaSaveUiCursor);
    auto* save_obj = GamePtr<uint8_t>(kRvaSaveObject);
    if (!phase || !in_progress || !cursor || !save_obj) {
        grandia_ap::LogWarn("Save UI restore skipped — grandia base unknown");
        return;
    }
    __try {
        *phase = 0;        // leave Loading phase
        *in_progress = 0;  // unlock menu
        *cursor = 0;
        *reinterpret_cast<uint32_t*>(save_obj + 0xCu) = 0;  // clear load arm
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        grandia_ap::LogWarn("Save UI restore faulted (base=0x%08X)",
                            static_cast<unsigned>(grandia_ap::GetGrandiaModuleBase()));
    }
}

bool BuildSelectedSlotPath(char* out, std::size_t out_len) {
    if (!out || out_len < 32) {
        return false;
    }
    unsigned slot = 0;
    unsigned profile = 0;
    const char* save_dir = nullptr;
    __try {
        auto* slot_ptr = GamePtr<uint8_t>(kRvaSaveSelectedSlot);
        auto* save_obj = GamePtr<uint8_t>(kRvaSaveObject);
        auto* dir_ptr = GamePtr<char>(kRvaSaveDirPrefix);
        if (!slot_ptr || !save_obj || !dir_ptr || !dir_ptr[0]) {
            return false;
        }
        slot = *slot_ptr;
        // Path helper +0x23560: ecx = [save_obj+8] → decimal profile folder under Saves\.
        profile = *reinterpret_cast<uint32_t*>(save_obj + 0x8u);
        save_dir = dir_ptr;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    // Defensive: Steam cloud user folders are small integers (0, 16, …).
    if (profile > 999999u) {
        return false;
    }
    const int n =
        std::snprintf(out, out_len, "%s%u\\BISLPSP02124GRA-S%02u", save_dir, profile, slot);
    return n > 0 && static_cast<std::size_t>(n) < out_len;
}

// Returns true if load should proceed. On deny sets g_load_denied + toast.
bool EvaluateSlotSeedGate(std::FILE* file, bool show_toast) {
    if (g_expected_seed_hash == 0) {
        return true;
    }

    grandia_ap::ApSaveTrailerV1 trailer{};
    const bool has_trailer = PeekGap1Trailer(file, &trailer);

    if (!has_trailer) {
        g_load_denied = true;
        g_confirm_load_armed = false;
        ClearPendingTrailer();
        grandia_ap::LogWarn(
            "Load blocked — vanilla save (no GAP1). Start New Game while connected, then Save.");
        if (show_toast) {
            grandia_ap::ShowD3dOverlayToast("Vanilla save — New Game + Save required for AP", 6000,
                                            0xFA8072u);
        }
        return false;
    }

    if (trailer.seed_hash == 0) {
        // Legacy GAP1 (pre-seed-binding): treat like vanilla — must New Game + Save.
        g_load_denied = true;
        g_confirm_load_armed = false;
        ClearPendingTrailer();
        grandia_ap::LogWarn(
            "Load blocked — legacy GAP1 (seed=0). Start New Game while connected, then Save.");
        if (show_toast) {
            grandia_ap::ShowD3dOverlayToast("Legacy AP save — New Game + Save required for AP", 6000,
                                            0xFA8072u);
        }
        return false;
    }

    if (trailer.seed_hash != g_expected_seed_hash) {
        g_load_denied = true;
        g_confirm_load_armed = false;
        ClearPendingTrailer();
        grandia_ap::LogWarn("Load blocked — seed mismatch save=0x%08X expected=0x%08X",
                            trailer.seed_hash, g_expected_seed_hash);
        if (show_toast) {
            grandia_ap::ShowD3dOverlayToast("Save belongs to a different AP seed/slot", 6000,
                                            0xFA8072u);
        }
        return false;
    }

    grandia_ap::LogInfo("Load gate: OK seed=0x%08X", trailer.seed_hash);
    return true;
}

void EvaluateConfirmLoadAtOp3() {
    g_load_denied = false;
    g_confirm_load_armed = false;
    g_seed_gate_ok = false;

    if (g_expected_seed_hash == 0 || !g_crt_fopen || !g_crt_fclose) {
        // Fall through to fopen gate / post-load SYNC.
        g_confirm_load_armed = true;
        return;
    }

    char path[MAX_PATH]{};
    if (!BuildSelectedSlotPath(path, sizeof(path))) {
        grandia_ap::LogWarn("Load gate: could not build slot path — deferring to fopen gate");
        g_confirm_load_armed = true;
        return;
    }
    grandia_ap::LogInfo("Load gate: peek %s", path);

    std::FILE* file = g_crt_fopen(path, "rb");
    if (!file) {
        // Missing/empty slot — let the game handle it.
        g_confirm_load_armed = true;
        return;
    }

    const bool allow = EvaluateSlotSeedGate(file, true);
    g_crt_fclose(file);
    if (allow) {
        g_confirm_load_armed = true;
        g_load_denied = false;
        g_seed_gate_ok = true;
    }
}

void AppendTrailer(std::FILE* file) {
    if (!file || !g_crt_fwrite) {
        return;
    }
    __try {
        const uint32_t previous_seed = g_trailer.seed_hash;
        g_trailer.magic[0] = 'G';
        g_trailer.magic[1] = 'A';
        g_trailer.magic[2] = 'P';
        g_trailer.magic[3] = '1';
        g_trailer.version = grandia_ap::kApSaveTrailerVersion;
        // Bind this save to the connected AP room+slot on every write.
        if (g_expected_seed_hash != 0) {
            g_trailer.seed_hash = g_expected_seed_hash;
        }

        const std::size_t wrote = g_crt_fwrite(&g_trailer, 1, sizeof(g_trailer), file);
        if (wrote != sizeof(g_trailer)) {
            grandia_ap::LogWarn("Save trailer fwrite failed (%u / %u bytes)",
                                static_cast<unsigned>(wrote),
                                static_cast<unsigned>(sizeof(g_trailer)));
            return;
        }
        g_trailer_present = true;
        g_trailer_dirty = false;
        g_load_committed = true;
        grandia_ap::LogInfo(
            "Appended GAP1 trailer (received_index=%u seed=0x%08X, +%u bytes after 0xE80)",
            g_trailer.received_index, g_trailer.seed_hash,
            static_cast<unsigned>(sizeof(g_trailer)));
        // First bind (New Game → Save): announce SYNC so AP delivery can open without reload.
        if (previous_seed == 0 && g_trailer.seed_hash != 0) {
            TryAnnounceSaveSync(true);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        grandia_ap::LogWarn("Save trailer fwrite faulted — ignored");
    }
}

bool CommitPendingNow(const char* reason) {
    if (g_pending_present) {
        g_trailer = g_pending_trailer;
        g_trailer_present = true;
        g_trailer_dirty = false;
        grandia_ap::LogInfo(
            "Committed GAP1 trailer (%s) received_index=%u seed=0x%08X",
            reason ? reason : "?", g_trailer.received_index, g_trailer.seed_hash);
        ClearPendingTrailer();
        g_load_committed = true;
        TryAnnounceSaveSync(true);
        return true;
    }
    InitEmptyTrailer();
    grandia_ap::LogInfo("Committed load (%s) — no GAP1 trailer (legacy 0xE80 slot)",
                        reason ? reason : "?");
    g_load_committed = true;
    TryAnnounceSaveSync(true);
    return false;
}

void TryAnnounceSaveSync(bool force) {
    if (!g_load_committed) {
        return;
    }
    // Hold SYNC until stash can accept item deliveries.
    if (!grandia_ap::HasStashBase()) {
        return;
    }
    grandia_ap::PipeEnqueueSync(g_trailer.received_index, g_trailer.seed_hash,
                                g_trailer_present ? 1u : 0u, force);
}

}  // namespace

extern "C" {

volatile void* g_ap_save_fwrite_fn = nullptr;
volatile void* g_ap_save_fread_fn = nullptr;
volatile void* g_ap_save_fopen_fn = nullptr;
volatile void* g_ap_save_fwrite_resume = nullptr;
volatile void* g_ap_save_fread_resume = nullptr;
volatile void* g_ap_save_fopen_resume = nullptr;
volatile void* g_ap_save_confirm_ui_trampoline = nullptr;  // original +0x657DA bytes
volatile void* g_ap_save_confirm_ui_resume = nullptr;      // +0x66731
volatile void* g_ap_save_fsm_resume = nullptr;

volatile void* g_ap_save_io_ptr = nullptr;
volatile std::size_t g_ap_save_io_size = 0;
volatile std::size_t g_ap_save_io_count = 0;
volatile void* g_ap_save_io_file = nullptr;
volatile std::size_t g_ap_save_io_result = 0;

volatile unsigned g_ap_save_fsm_state = 0;
volatile void* g_ap_save_fsm_object = nullptr;
volatile unsigned g_ap_save_load_denied = 0;
volatile unsigned g_ap_confirm_ui_skip = 0;  // 1 = deny path for confirm-UI detour

void ApOnSaveFsmEnter() {
    const unsigned state = g_ap_save_fsm_state;
    if (state == kFsmOpConfirmLoad) {
        EvaluateConfirmLoadAtOp3();
        g_ap_save_load_denied = g_load_denied ? 1u : 0u;
        if (g_load_denied) {
            grandia_ap::LogInfo(
                "Save FSM op=3 (confirm LOAD) — skip Loading UI (seed gate deny)");
        } else {
            grandia_ap::LogInfo(
                "Save FSM op=3 (confirm LOAD) — seed OK (vanilla Loading UI trampoline)");
        }
    } else if (state == kFsmOpSave) {
        g_confirm_load_armed = false;
        g_load_denied = false;
        g_seed_gate_ok = false;
        g_ap_save_load_denied = 0;
        grandia_ap::LogInfo("Save FSM op=4 (SAVE)");
    } else if (state == kFsmOpListRefresh) {
        grandia_ap::LogDebug("Save FSM op=6 (list refresh)");
    }
}

// Deny at +0x657DA: never write Loading flags; clear load arm so fopen never runs.
void ApOnSaveLoadDeniedSkipLoading() {
    g_load_denied = false;
    g_ap_save_load_denied = 0;
    g_confirm_load_armed = false;
    g_seed_gate_ok = false;
    auto* save_obj = GamePtr<uint8_t>(kRvaSaveObject);
    if (save_obj) {
        __try {
            *reinterpret_cast<uint32_t*>(save_obj + 0xCu) = 0;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            grandia_ap::LogWarn("Save deny: could not clear load arm (base=0x%08X)",
                                static_cast<unsigned>(grandia_ap::GetGrandiaModuleBase()));
        }
    }
    // Also force save-select phase in case Loading stores already ran on a prior attempt.
    RestoreSaveSelectUi();
    grandia_ap::LogInfo("Save load denied — skipped Loading UI, cleared load arm");
}

void ApOnSaveConfirmUiDecide() {
    g_ap_confirm_ui_skip = 0;
    if (g_ap_save_load_denied != 0) {
        ApOnSaveLoadDeniedSkipLoading();
        g_ap_confirm_ui_skip = 1;
    }
}

// After load-path fopen: backstop if early gate missed (path mismatch, etc.).
void ApOnSaveFopenGate() {
    auto* file = reinterpret_cast<std::FILE*>(const_cast<void*>(g_ap_save_io_file));
    if (!file || !g_confirm_load_armed) {
        return;
    }

    if (g_expected_seed_hash == 0) {
        if (g_crt_fseek) {
            g_crt_fseek(file, 0, SEEK_SET);
        }
        return;
    }

    // Early op=3 already validated this slot — don't re-peek (avoids false denies).
    if (g_seed_gate_ok) {
        if (g_crt_fseek) {
            g_crt_fseek(file, 0, SEEK_SET);
        }
        return;
    }

    // EvaluateSlotSeedGate peeks (seeks to trailer); rewind on allow.
    if (!EvaluateSlotSeedGate(file, true)) {
        if (g_crt_fclose) {
            g_crt_fclose(file);
        }
        g_ap_save_io_file = nullptr;
        g_confirm_load_armed = false;
        g_load_denied = false;
        g_ap_save_load_denied = 0;
        // Best-effort UI undo if we somehow reached fopen while Loading.
        RestoreSaveSelectUi();
        return;
    }

    if (g_crt_fseek) {
        g_crt_fseek(file, 0, SEEK_SET);
    }
}

void ApOnSaveFwriteComplete() {
    auto* file = reinterpret_cast<std::FILE*>(const_cast<void*>(g_ap_save_io_file));
    if (!IsVanillaSaveIo(g_ap_save_io_size, g_ap_save_io_count)) {
        return;
    }
    grandia_ap::LogInfo("Save fwrite hook: written=%u/%u",
                        static_cast<unsigned>(g_ap_save_io_result),
                        static_cast<unsigned>(grandia_ap::kVanillaSaveSize));
    if (g_ap_save_io_result != grandia_ap::kVanillaSaveSize) {
        grandia_ap::LogWarn("Save fwrite incomplete — skipping trailer");
        return;
    }
    AppendTrailer(file);
}

void ApOnSaveFreadComplete() {
    auto* file = reinterpret_cast<std::FILE*>(const_cast<void*>(g_ap_save_io_file));
    if (!IsVanillaSaveIo(g_ap_save_io_size, g_ap_save_io_count)) {
        return;
    }
    if (g_ap_save_io_result != grandia_ap::kVanillaSaveSize) {
        return;
    }

    CachePendingTrailer(file);

    if (g_confirm_load_armed) {
        CommitPendingNow("confirm load");
        g_confirm_load_armed = false;
    }
    // Else: save-list preview — leave active trailer alone.
}

#if defined(_M_IX86)

__declspec(naked) void ApSaveFsmDetour() {
    __asm {
        // ecx = save manager object; [ecx+0x22] already set by caller.
        mov dword ptr [g_ap_save_fsm_object], ecx
        movzx eax, byte ptr [ecx + 22h]
        mov dword ptr [g_ap_save_fsm_state], eax

        pushad
        mov eax, esp
        and esp, 0FFFFFFF0h
        sub esp, 16
        mov dword ptr [esp], eax
        call ApOnSaveFsmEnter
        mov esp, dword ptr [esp]
        popad

        // Original prologue at +0x2300
        push ebp
        mov ebp, esp
        sub esp, 274h
        jmp dword ptr [g_ap_save_fsm_resume]
    }
}

__declspec(naked) void ApSaveFwriteDetour() {
    __asm {
        mov eax, dword ptr [esp]
        mov dword ptr [g_ap_save_io_ptr], eax
        mov eax, dword ptr [esp + 4]
        mov dword ptr [g_ap_save_io_size], eax
        mov eax, dword ptr [esp + 8]
        mov dword ptr [g_ap_save_io_count], eax
        mov eax, dword ptr [esp + 0Ch]
        mov dword ptr [g_ap_save_io_file], eax

        call dword ptr [g_ap_save_fwrite_fn]
        mov dword ptr [g_ap_save_io_result], eax

        pushad
        mov eax, esp
        and esp, 0FFFFFFF0h
        sub esp, 16
        mov dword ptr [esp], eax
        call ApOnSaveFwriteComplete
        mov esp, dword ptr [esp]
        popad

        mov eax, dword ptr [g_ap_save_io_result]
        jmp dword ptr [g_ap_save_fwrite_resume]
    }
}

__declspec(naked) void ApSaveFreadDetour() {
    __asm {
        mov eax, dword ptr [esp]
        mov dword ptr [g_ap_save_io_ptr], eax
        mov eax, dword ptr [esp + 4]
        mov dword ptr [g_ap_save_io_size], eax
        mov eax, dword ptr [esp + 8]
        mov dword ptr [g_ap_save_io_count], eax
        mov eax, dword ptr [esp + 0Ch]
        mov dword ptr [g_ap_save_io_file], eax

        call dword ptr [g_ap_save_fread_fn]
        mov dword ptr [g_ap_save_io_result], eax

        pushad
        mov eax, esp
        and esp, 0FFFFFFF0h
        sub esp, 16
        mov dword ptr [esp], eax
        call ApOnSaveFreadComplete
        mov esp, dword ptr [esp]
        popad

        mov eax, dword ptr [g_ap_save_io_result]
        jmp dword ptr [g_ap_save_fread_resume]
    }
}

__declspec(naked) void ApSaveFopenDetour() {
    __asm {
        // Replaces `call [fopen]` on the load path. Stack still has (path, mode).
        call dword ptr [g_ap_save_fopen_fn]
        mov dword ptr [g_ap_save_io_file], eax

        pushad
        mov eax, esp
        and esp, 0FFFFFFF0h
        sub esp, 16
        mov dword ptr [esp], eax
        call ApOnSaveFopenGate
        mov esp, dword ptr [esp]
        popad

        mov eax, dword ptr [g_ap_save_io_file]
        jmp dword ptr [g_ap_save_fopen_resume]
    }
}

// Replaces `mov [0x6c301c],2` at +0x657DA.
// Allow: trampoline executes the original instruction then continues at +0x657E1.
// Deny: skip Loading stores, clear load arm, jump shared epilogue at +0x66731.
__declspec(naked) void ApSaveConfirmUiDetour() {
    __asm {
        pushad
        mov eax, esp
        and esp, 0FFFFFFF0h
        sub esp, 16
        mov dword ptr [esp], eax
        call ApOnSaveConfirmUiDecide
        mov esp, dword ptr [esp]
        popad

        cmp dword ptr [g_ap_confirm_ui_skip], 0
        jne deny

        jmp dword ptr [g_ap_save_confirm_ui_trampoline]

    deny:
        mov ebx, 0Ah
        jmp dword ptr [g_ap_save_confirm_ui_resume]
    }
}

#endif

}  // extern "C"

namespace grandia_ap {

bool InstallSaveSyncHooks() {
#if !defined(_M_IX86)
    LogWarn("Save sync hooks require 32-bit build");
    return false;
#else
    const std::uintptr_t base = GetGrandiaModuleBase();
    if (base == 0) {
        LogWarn("Save sync: grandia base unknown");
        return false;
    }

    InitEmptyTrailer();
    ClearPendingTrailer();
    g_confirm_load_armed = false;
    g_load_denied = false;
    g_seed_gate_ok = false;
    g_ap_save_load_denied = 0;
    g_ap_confirm_ui_skip = 0;
    g_load_committed = false;

    auto* fwrite_site = reinterpret_cast<uint8_t*>(base + kSaveFwriteCallRva);
    auto* fread_site = reinterpret_cast<uint8_t*>(base + kSaveFreadCallRva);
    auto* fopen_site = reinterpret_cast<uint8_t*>(base + kSaveFopenCallRva);
    auto* confirm_ui_site = reinterpret_cast<uint8_t*>(base + kSaveConfirmUiRva);
    auto* fsm_site = reinterpret_cast<uint8_t*>(base + kSaveFsmEntryRva);

    if (fwrite_site[0] != 0xFF || fwrite_site[1] != 0x15 || fread_site[0] != 0xFF ||
        fread_site[1] != 0x15 || fopen_site[0] != 0xFF || fopen_site[1] != 0x15) {
        LogWarn("Save sync: fwrite/fread/fopen call-site mismatch");
        return false;
    }
    // Expect: C6 05 1C 30 6C 00 02  — mov byte ptr [0x6c301c], 2
    if (confirm_ui_site[0] != 0xC6 || confirm_ui_site[1] != 0x05 || confirm_ui_site[6] != 0x02) {
        LogWarn("Save sync: confirm-UI site mismatch at +0x%X",
                static_cast<unsigned>(kSaveConfirmUiRva));
        return false;
    }
    // Expect: 55 8B EC 81 EC 74 02 00 00
    if (fsm_site[0] != 0x55 || fsm_site[1] != 0x8B || fsm_site[2] != 0xEC) {
        LogWarn("Save sync: FSM entry bytes mismatch at +0x2300");
        return false;
    }

    g_crt_fwrite = reinterpret_cast<FwriteFn>(ReadIatFunction(fwrite_site));
    g_crt_fread = reinterpret_cast<FreadFn>(ReadIatFunction(fread_site));
    g_crt_fopen = reinterpret_cast<FopenFn>(ReadIatFunction(fopen_site));
    if (!g_crt_fwrite || !g_crt_fread || !g_crt_fopen) {
        LogWarn("Save sync: IAT fwrite/fread/fopen unresolved");
        return false;
    }

    HMODULE crt_mod = nullptr;
    if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCSTR>(g_crt_fread), &crt_mod) ||
        !crt_mod) {
        LogWarn("Save sync: CRT module unresolved");
        return false;
    }
    g_crt_fseek = reinterpret_cast<FseekFn>(GetProcAddress(crt_mod, "fseek"));
    g_crt_ftell = reinterpret_cast<FtellFn>(GetProcAddress(crt_mod, "ftell"));
    g_crt_fclose = reinterpret_cast<FcloseFn>(GetProcAddress(crt_mod, "fclose"));
    if (!g_crt_fseek || !g_crt_fclose) {
        LogWarn("Save sync: fseek/fclose unresolved");
        return false;
    }

    // Trampoline: original mov [0x6c301c],2 then jmp +0x657E1.
    g_confirm_ui_trampoline = reinterpret_cast<uint8_t*>(
        VirtualAlloc(nullptr, kConfirmUiTrampolineSize, MEM_COMMIT | MEM_RESERVE,
                     PAGE_EXECUTE_READWRITE));
    if (!g_confirm_ui_trampoline) {
        LogWarn("Save sync: confirm-UI trampoline alloc failed");
        return false;
    }
    std::memcpy(g_confirm_ui_trampoline, confirm_ui_site, kConfirmUiPatchSize);
    {
        auto* cont = reinterpret_cast<uint8_t*>(base + kSaveConfirmUiContinueRva);
        g_confirm_ui_trampoline[kConfirmUiPatchSize] = 0xE9;
        const auto rel = static_cast<int32_t>(
            cont - (g_confirm_ui_trampoline + kConfirmUiPatchSize + 5));
        std::memcpy(g_confirm_ui_trampoline + kConfirmUiPatchSize + 1, &rel, sizeof(rel));
    }

    g_ap_save_fwrite_fn = reinterpret_cast<void*>(g_crt_fwrite);
    g_ap_save_fread_fn = reinterpret_cast<void*>(g_crt_fread);
    g_ap_save_fopen_fn = reinterpret_cast<void*>(g_crt_fopen);
    g_ap_save_fwrite_resume = reinterpret_cast<void*>(base + kSaveFwriteResumeRva);
    g_ap_save_fread_resume = reinterpret_cast<void*>(base + kSaveFreadResumeRva);
    g_ap_save_fopen_resume = reinterpret_cast<void*>(base + kSaveFopenResumeRva);
    g_ap_save_confirm_ui_trampoline = g_confirm_ui_trampoline;
    g_ap_save_confirm_ui_resume = reinterpret_cast<void*>(base + kSaveConfirmUiResumeRva);
    g_ap_save_fsm_resume = reinterpret_cast<void*>(base + kSaveFsmResumeRva);

    auto rollback_all = [&]() {
        if (g_fread_hook_site) {
            RestoreBytes(g_fread_hook_site, g_fread_hook_original, kCallIatPatchSize);
            g_fread_hook_site = nullptr;
        }
        if (g_fopen_hook_site) {
            RestoreBytes(g_fopen_hook_site, g_fopen_hook_original, kCallIatPatchSize);
            g_fopen_hook_site = nullptr;
        }
        if (g_confirm_ui_hook_site) {
            RestoreBytes(g_confirm_ui_hook_site, g_confirm_ui_hook_original, kConfirmUiPatchSize);
            g_confirm_ui_hook_site = nullptr;
        }
        if (g_fwrite_hook_site) {
            RestoreBytes(g_fwrite_hook_site, g_fwrite_hook_original, kCallIatPatchSize);
            g_fwrite_hook_site = nullptr;
        }
        if (g_fsm_hook_site) {
            RestoreBytes(g_fsm_hook_site, g_fsm_hook_original, kFsmEntryPatchSize);
            g_fsm_hook_site = nullptr;
        }
        if (g_confirm_ui_trampoline) {
            VirtualFree(g_confirm_ui_trampoline, 0, MEM_RELEASE);
            g_confirm_ui_trampoline = nullptr;
            g_ap_save_confirm_ui_trampoline = nullptr;
        }
    };

    if (!WriteJump(fsm_site, reinterpret_cast<void*>(ApSaveFsmDetour), g_fsm_hook_original,
                   kFsmEntryPatchSize)) {
        LogWarn("Save sync: FSM hook failed");
        return false;
    }
    g_fsm_hook_site = fsm_site;

    if (!WriteJump(fwrite_site, reinterpret_cast<void*>(ApSaveFwriteDetour), g_fwrite_hook_original,
                   kCallIatPatchSize)) {
        rollback_all();
        LogWarn("Save sync: fwrite hook failed");
        return false;
    }
    g_fwrite_hook_site = fwrite_site;

    if (!WriteJump(confirm_ui_site, reinterpret_cast<void*>(ApSaveConfirmUiDetour),
                   g_confirm_ui_hook_original, kConfirmUiPatchSize)) {
        rollback_all();
        LogWarn("Save sync: confirm-UI hook failed");
        return false;
    }
    g_confirm_ui_hook_site = confirm_ui_site;

    if (!WriteJump(fopen_site, reinterpret_cast<void*>(ApSaveFopenDetour), g_fopen_hook_original,
                   kCallIatPatchSize)) {
        rollback_all();
        LogWarn("Save sync: fopen hook failed");
        return false;
    }
    g_fopen_hook_site = fopen_site;

    if (!WriteJump(fread_site, reinterpret_cast<void*>(ApSaveFreadDetour), g_fread_hook_original,
                   kCallIatPatchSize)) {
        rollback_all();
        LogWarn("Save sync: fread hook failed");
        return false;
    }
    g_fread_hook_site = fread_site;

    LogInfo(
        "Installed save sync hooks (FSM +0x%X, confirm-UI +0x%X trampoline, fopen +0x%X, "
        "fwrite +0x%X, fread +0x%X) — deny skips Loading UI (base=0x%08X)",
        static_cast<unsigned>(kSaveFsmEntryRva), static_cast<unsigned>(kSaveConfirmUiRva),
        static_cast<unsigned>(kSaveFopenCallRva), static_cast<unsigned>(kSaveFwriteCallRva),
        static_cast<unsigned>(kSaveFreadCallRva), static_cast<unsigned>(base));
    return true;
#endif
}

void RemoveSaveSyncHooks() {
#if defined(_M_IX86)
    if (g_fread_hook_site) {
        RestoreBytes(g_fread_hook_site, g_fread_hook_original, kCallIatPatchSize);
        g_fread_hook_site = nullptr;
    }
    if (g_fopen_hook_site) {
        RestoreBytes(g_fopen_hook_site, g_fopen_hook_original, kCallIatPatchSize);
        g_fopen_hook_site = nullptr;
    }
    if (g_confirm_ui_hook_site) {
        RestoreBytes(g_confirm_ui_hook_site, g_confirm_ui_hook_original, kConfirmUiPatchSize);
        g_confirm_ui_hook_site = nullptr;
    }
    if (g_fwrite_hook_site) {
        RestoreBytes(g_fwrite_hook_site, g_fwrite_hook_original, kCallIatPatchSize);
        g_fwrite_hook_site = nullptr;
    }
    if (g_fsm_hook_site) {
        RestoreBytes(g_fsm_hook_site, g_fsm_hook_original, kFsmEntryPatchSize);
        g_fsm_hook_site = nullptr;
    }
    if (g_confirm_ui_trampoline) {
        VirtualFree(g_confirm_ui_trampoline, 0, MEM_RELEASE);
        g_confirm_ui_trampoline = nullptr;
        g_ap_save_confirm_ui_trampoline = nullptr;
    }
#endif
}

bool IsSaveSyncHookInstalled() {
    return g_fwrite_hook_site != nullptr && g_fread_hook_site != nullptr &&
           g_fopen_hook_site != nullptr && g_confirm_ui_hook_site != nullptr &&
           g_fsm_hook_site != nullptr;
}

const ApSaveTrailerV1& GetSaveSyncTrailer() { return g_trailer; }

bool HasSaveSyncTrailer() { return g_trailer_present; }

bool HasPendingSaveTrailer() { return g_pending_present; }

bool CommitPendingSaveTrailer(const char* reason) { return CommitPendingNow(reason); }

void OnSaveSyncStashBecameReady() {
    // Confirm-load may commit before stash exists; announce SYNC once stash can accept items.
    TryAnnounceSaveSync(false);
}

void SetSaveSyncReceivedIndex(uint32_t received_index) {
    // Monotonic: never roll the watermark back (e.g. mis-ordered ITEM INDEX).
    if (g_trailer_present && received_index < g_trailer.received_index) {
        return;
    }
    g_trailer.received_index = received_index;
    g_trailer_dirty = true;
    g_trailer_present = true;
}

void SetSaveSyncExpectedSeedHash(uint32_t seed_hash) {
    g_expected_seed_hash = seed_hash;
    grandia_ap::LogInfo("CONFIG seed_hash=0x%08X", seed_hash);
}

uint32_t GetSaveSyncExpectedSeedHash() {
    return g_expected_seed_hash;
}

}  // namespace grandia_ap
