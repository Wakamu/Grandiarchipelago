#pragma once

#include <cstdint>

namespace grandia_ap {

// Persist AP sync metadata after the vanilla 0xE80-byte slot body (CRT fwrite/fread hooks).
// See docs/save_file_write_re.md.
//
// Save-select UI fread's every slot for preview. Confirm LOAD uses save FSM op=3
// ([0x718c62]=3 → grandia.exe+0x2300). We peek the selected slot's GAP1 there.
// On mismatch/vanilla: skip Loading UI stores at +0x657DA (trampoline allow path
// keeps those stores exact for matching seeds) and clear the load arm so fopen
// never runs. fopen (+0x2649) remains a backstop. Allowed loads commit GAP1 on fread.

#pragma pack(push, 1)
struct ApSaveTrailerV1 {
    char magic[4];           // "GAP1"
    uint16_t version;        // 1
    uint16_t flags;          // reserved
    uint32_t seed_hash;      // AP seed+slot identity (0 = unbound / vanilla)
    uint32_t received_index; // last applied AP ReceivedItems index
    uint32_t check_count;    // checked location count (0 in stub)
    uint32_t crc32;          // optional integrity (0 skips check in stub)
};
#pragma pack(pop)

static_assert(sizeof(ApSaveTrailerV1) == 24, "ApSaveTrailerV1 size");

constexpr uint32_t kVanillaSaveSize = 0xE80u;
constexpr uint16_t kApSaveTrailerVersion = 1;

bool InstallSaveSyncHooks();
void RemoveSaveSyncHooks();
bool IsSaveSyncHookInstalled();

// Active (committed) trailer — safe to use for AP resync gating.
const ApSaveTrailerV1& GetSaveSyncTrailer();
bool HasSaveSyncTrailer();
bool HasPendingSaveTrailer();

// Promote last pending preview/confirm fread trailer → active (confirm-load op=3).
bool CommitPendingSaveTrailer(const char* reason);

void SetSaveSyncReceivedIndex(uint32_t received_index);
// Expected hash for the connected AP room+slot. Stamped into the trailer on SAVE.
// Does not overwrite a just-loaded trailer's hash until the next save.
void SetSaveSyncExpectedSeedHash(uint32_t seed_hash);
uint32_t GetSaveSyncExpectedSeedHash();

// Stash watcher calls this when stash first appears — may flush SYNC if load already committed.
void OnSaveSyncStashBecameReady();

}  // namespace grandia_ap
