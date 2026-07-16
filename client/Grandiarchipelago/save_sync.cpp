#include "save_sync.h"

#include "game_memory.h"
#include "log.h"
#include "pipe_bridge.h"

#include <Windows.h>

#include <cstdio>
#include <cstring>

using FwriteFn = std::size_t(__cdecl*)(const void*, std::size_t, std::size_t, std::FILE*);
using FreadFn = std::size_t(__cdecl*)(void*, std::size_t, std::size_t, std::FILE*);
using FseekFn = int(__cdecl*)(std::FILE*, long, int);
using FtellFn = long(__cdecl*)(std::FILE*);

namespace {

constexpr std::uintptr_t kSaveFsmEntryRva = 0x2300u;
constexpr std::uintptr_t kSaveFsmResumeRva = 0x2309u;  // after `sub esp, 0x274`
constexpr std::uintptr_t kSaveFwriteCallRva = 0x254Eu;
constexpr std::uintptr_t kSaveFwriteResumeRva = 0x2554u;
constexpr std::uintptr_t kSaveFreadCallRva = 0x2673u;
constexpr std::uintptr_t kSaveFreadResumeRva = 0x2679u;
constexpr size_t kCallIatPatchSize = 6;
constexpr size_t kFsmEntryPatchSize = 9;  // push ebp; mov ebp,esp; sub esp,0x274

// FSM [object+0x22] / [0x718c62] ops (RE Jul 2026):
//   3 = confirm LOAD (sets up fread), 4 = SAVE (fwrite), 6 = list refresh, 2 = idle poll
constexpr unsigned kFsmOpConfirmLoad = 3;
constexpr unsigned kFsmOpSave = 4;
constexpr unsigned kFsmOpListRefresh = 6;

void* g_fwrite_hook_site = nullptr;
void* g_fread_hook_site = nullptr;
void* g_fsm_hook_site = nullptr;
uint8_t g_fwrite_hook_original[8]{};
uint8_t g_fread_hook_original[8]{};
uint8_t g_fsm_hook_original[16]{};

FwriteFn g_crt_fwrite = nullptr;
FreadFn g_crt_fread = nullptr;
FseekFn g_crt_fseek = nullptr;
FtellFn g_crt_ftell = nullptr;

grandia_ap::ApSaveTrailerV1 g_trailer{};
grandia_ap::ApSaveTrailerV1 g_pending_trailer{};
bool g_trailer_present = false;
bool g_trailer_dirty = false;
bool g_pending_present = false;
int g_pending_slot = -1;
bool g_confirm_load_armed = false;
bool g_load_committed = false;

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

void CachePendingTrailer(std::FILE* file) {
    if (!file || !g_crt_fread || !g_crt_fseek) {
        return;
    }

    __try {
        if (g_crt_fseek(file, static_cast<long>(grandia_ap::kVanillaSaveSize), SEEK_SET) != 0) {
            ClearPendingTrailer();
            return;
        }

        grandia_ap::ApSaveTrailerV1 tmp{};
        const std::size_t got = g_crt_fread(&tmp, 1, sizeof(tmp), file);
        g_crt_fseek(file, static_cast<long>(grandia_ap::kVanillaSaveSize), SEEK_SET);

        if (got != sizeof(tmp) || std::memcmp(tmp.magic, "GAP1", 4) != 0) {
            ClearPendingTrailer();
            return;
        }
        if (tmp.version != grandia_ap::kApSaveTrailerVersion) {
            ClearPendingTrailer();
            return;
        }

        g_pending_trailer = tmp;
        g_pending_present = true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        ClearPendingTrailer();
        grandia_ap::LogWarn("Save trailer peek faulted — ignored");
    }
}

void AppendTrailer(std::FILE* file) {
    if (!file || !g_crt_fwrite) {
        return;
    }
    __try {
        g_trailer.magic[0] = 'G';
        g_trailer.magic[1] = 'A';
        g_trailer.magic[2] = 'P';
        g_trailer.magic[3] = '1';
        g_trailer.version = grandia_ap::kApSaveTrailerVersion;

        const std::size_t wrote = g_crt_fwrite(&g_trailer, 1, sizeof(g_trailer), file);
        if (wrote != sizeof(g_trailer)) {
            grandia_ap::LogWarn("Save trailer fwrite failed (%u / %u bytes)",
                                static_cast<unsigned>(wrote),
                                static_cast<unsigned>(sizeof(g_trailer)));
            return;
        }
        g_trailer_present = true;
        g_trailer_dirty = false;
        grandia_ap::LogInfo(
            "Appended GAP1 trailer (received_index=%u seed=0x%08X, +%u bytes after 0xE80)",
            g_trailer.received_index, g_trailer.seed_hash,
            static_cast<unsigned>(sizeof(g_trailer)));
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
    grandia_ap::PipeEnqueueSync(g_trailer.received_index, force);
}

}  // namespace

extern "C" {

volatile void* g_ap_save_fwrite_fn = nullptr;
volatile void* g_ap_save_fread_fn = nullptr;
volatile void* g_ap_save_fwrite_resume = nullptr;
volatile void* g_ap_save_fread_resume = nullptr;
volatile void* g_ap_save_fsm_resume = nullptr;

volatile void* g_ap_save_io_ptr = nullptr;
volatile std::size_t g_ap_save_io_size = 0;
volatile std::size_t g_ap_save_io_count = 0;
volatile void* g_ap_save_io_file = nullptr;
volatile std::size_t g_ap_save_io_result = 0;

volatile unsigned g_ap_save_fsm_state = 0;
volatile void* g_ap_save_fsm_object = nullptr;

void ApOnSaveFsmEnter() {
    const unsigned state = g_ap_save_fsm_state;
    if (state == kFsmOpConfirmLoad) {
        g_confirm_load_armed = true;
        grandia_ap::LogInfo("Save FSM op=3 (confirm LOAD) — next fread will commit trailer");
    } else if (state == kFsmOpSave) {
        g_confirm_load_armed = false;
        grandia_ap::LogInfo("Save FSM op=4 (SAVE)");
    } else if (state == kFsmOpListRefresh) {
        // List rebuild — do not arm confirm-load.
        grandia_ap::LogDebug("Save FSM op=6 (list refresh)");
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
    g_load_committed = false;

    auto* fwrite_site = reinterpret_cast<uint8_t*>(base + kSaveFwriteCallRva);
    auto* fread_site = reinterpret_cast<uint8_t*>(base + kSaveFreadCallRva);
    auto* fsm_site = reinterpret_cast<uint8_t*>(base + kSaveFsmEntryRva);

    if (fwrite_site[0] != 0xFF || fwrite_site[1] != 0x15 || fread_site[0] != 0xFF ||
        fread_site[1] != 0x15) {
        LogWarn("Save sync: fwrite/fread call-site mismatch");
        return false;
    }
    // Expect: 55 8B EC 81 EC 74 02 00 00
    if (fsm_site[0] != 0x55 || fsm_site[1] != 0x8B || fsm_site[2] != 0xEC) {
        LogWarn("Save sync: FSM entry bytes mismatch at +0x2300");
        return false;
    }

    g_crt_fwrite = reinterpret_cast<FwriteFn>(ReadIatFunction(fwrite_site));
    g_crt_fread = reinterpret_cast<FreadFn>(ReadIatFunction(fread_site));
    if (!g_crt_fwrite || !g_crt_fread) {
        LogWarn("Save sync: IAT fwrite/fread unresolved");
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
    if (!g_crt_fseek) {
        LogWarn("Save sync: fseek unresolved");
        return false;
    }

    g_ap_save_fwrite_fn = reinterpret_cast<void*>(g_crt_fwrite);
    g_ap_save_fread_fn = reinterpret_cast<void*>(g_crt_fread);
    g_ap_save_fwrite_resume = reinterpret_cast<void*>(base + kSaveFwriteResumeRva);
    g_ap_save_fread_resume = reinterpret_cast<void*>(base + kSaveFreadResumeRva);
    g_ap_save_fsm_resume = reinterpret_cast<void*>(base + kSaveFsmResumeRva);

    if (!WriteJump(fsm_site, reinterpret_cast<void*>(ApSaveFsmDetour), g_fsm_hook_original,
                   kFsmEntryPatchSize)) {
        LogWarn("Save sync: FSM hook failed");
        return false;
    }
    g_fsm_hook_site = fsm_site;

    if (!WriteJump(fwrite_site, reinterpret_cast<void*>(ApSaveFwriteDetour), g_fwrite_hook_original,
                   kCallIatPatchSize)) {
        RestoreBytes(g_fsm_hook_site, g_fsm_hook_original, kFsmEntryPatchSize);
        g_fsm_hook_site = nullptr;
        LogWarn("Save sync: fwrite hook failed");
        return false;
    }
    g_fwrite_hook_site = fwrite_site;

    if (!WriteJump(fread_site, reinterpret_cast<void*>(ApSaveFreadDetour), g_fread_hook_original,
                   kCallIatPatchSize)) {
        RestoreBytes(g_fwrite_hook_site, g_fwrite_hook_original, kCallIatPatchSize);
        RestoreBytes(g_fsm_hook_site, g_fsm_hook_original, kFsmEntryPatchSize);
        g_fwrite_hook_site = nullptr;
        g_fsm_hook_site = nullptr;
        LogWarn("Save sync: fread hook failed");
        return false;
    }
    g_fread_hook_site = fread_site;

    LogInfo(
        "Installed save sync hooks (FSM +0x%X, fwrite +0x%X, fread +0x%X) — commit on FSM op=3 only",
        static_cast<unsigned>(kSaveFsmEntryRva), static_cast<unsigned>(kSaveFwriteCallRva),
        static_cast<unsigned>(kSaveFreadCallRva));
    return true;
#endif
}

void RemoveSaveSyncHooks() {
#if defined(_M_IX86)
    if (g_fread_hook_site) {
        RestoreBytes(g_fread_hook_site, g_fread_hook_original, kCallIatPatchSize);
        g_fread_hook_site = nullptr;
    }
    if (g_fwrite_hook_site) {
        RestoreBytes(g_fwrite_hook_site, g_fwrite_hook_original, kCallIatPatchSize);
        g_fwrite_hook_site = nullptr;
    }
    if (g_fsm_hook_site) {
        RestoreBytes(g_fsm_hook_site, g_fsm_hook_original, kFsmEntryPatchSize);
        g_fsm_hook_site = nullptr;
    }
#endif
}

bool IsSaveSyncHookInstalled() {
    return g_fwrite_hook_site != nullptr && g_fread_hook_site != nullptr && g_fsm_hook_site != nullptr;
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
    g_trailer.received_index = received_index;
    g_trailer_dirty = true;
    g_trailer_present = true;
}

void SetSaveSyncSeedHash(uint32_t seed_hash) {
    g_trailer.seed_hash = seed_hash;
    g_trailer_dirty = true;
}

}  // namespace grandia_ap
