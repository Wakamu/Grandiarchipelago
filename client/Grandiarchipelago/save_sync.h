#pragma once

#include <cstdint>

namespace grandia_ap {

// Persist AP sync metadata after the vanilla 0xE80-byte slot body (CRT fwrite/fread hooks).
// See docs/save_file_write_re.md.
//
// Save-select UI fread's every slot for preview. Confirm LOAD uses save FSM op=3
// ([0x718c62]=3 → grandia.exe+0x2300). We only commit the GAP1 trailer on that path.
// Opening the list in-game to save (op=4 / list refresh) does not change active sync state.

#pragma pack(push, 1)
struct ApSaveTrailerV1 {
    char magic[4];           // "GAP1"
    uint16_t version;        // 1
    uint16_t flags;          // reserved
    uint32_t seed_hash;      // AP room/seed identity (0 = unset in stub)
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
void SetSaveSyncSeedHash(uint32_t seed_hash);

// Stash watcher calls this when stash first appears — may flush SYNC if load already committed.
void OnSaveSyncStashBecameReady();

}  // namespace grandia_ap
