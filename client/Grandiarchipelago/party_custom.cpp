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
#include <vector>

#if defined(_M_IX86)
extern "C" {
void ApPartyCountDetour();
void ApPartyCharIdDetour();
void ApBattlePartyInitDetour();
void ApMenuFillDetour();
void ApStashFillDetour();
void ApItemFillDetour();
void ApBattleAllyCountDetour();
void ApBattleAllySpawnGateDetour();
void ApBattleAllyCharIdDetour();
void ApBattlePlayablePackProbeDetour();
void ApBattleKeyedTaskEnqueueProbeDetour();
void ApBattlePackWriteProbeDetour();
void ApBattlePackPrepProbeDetour();
void ApBattleLoadDetour();
void ApBattleModelBindDetour();
void ApBattleAnimBindDetour();
void ApHdLoadContentDetour();
int ApHdLoadContentShouldSkip(const char* a1, const char* a2, const char* a3);
unsigned ApPartyResolveCount(unsigned group_index);
unsigned ApPartyResolveCharId(unsigned slot);
unsigned ApBattleAllyResolveCount(unsigned formation_index);
int ApBattleAllyShouldForceSpawn();
unsigned ApBattleAllyResolveCharId(unsigned slot0, unsigned table_index, unsigned table_base);
unsigned ApBattleAllyResolveModelSlot(unsigned spawn_slot0, unsigned char_id);
void ApBattleLogSpawnState(unsigned combatant, unsigned char_id);
void ApBattleFixupAllyAnimTables(unsigned combatant, unsigned c8a_slot);
unsigned ApBattleResolvePlayablePackProbe(unsigned combatant, unsigned battle_ctx, unsigned selector);
void ApBattleLogKeyedTaskEnqueue(unsigned owner, unsigned task, unsigned free_slot,
                                 unsigned task_slot, unsigned selector);
void TryInjectExtraKeyedTasks(unsigned owner);
void ApBattleLogPackWrite(unsigned combatant, unsigned dest_rel);
void ApBattleLogPackPrep(unsigned combatant, unsigned rel18, unsigned rel1c, unsigned rel20);
bool PtrReadable(const void* p, size_t bytes);
bool ModelHeaderReadable(void* header);
void ApOnBattlePartyInitEntry();
void ApOnBattleLoadEntry();
void LogPackedAllyModelSlots(std::uintptr_t base, const char* tag);
void TrySynthesizeC8aFromKeyedEntries(std::uintptr_t base, const char* tag);
void TryRemapC8aFromKeyedBuffers(std::uintptr_t base, const char* tag);
void FreeKeyedSlotBuffers();
void TrySplicePdatPlayables(std::uintptr_t base, const char* tag);
void* ApBattleModelBindResolve(void* header);
int ApBattleAnimBindResolve(unsigned bank);
int ApMenuFillTryCustom();
int ApStashFillTryCustom();
int ApItemFillTryCustom();
void ApStatusCustomFaces();
void ApStashCustomFaces();
void ApItemCustomFaces();
void ApFaceBankLoadDetour();
void ApInvAddDetour();
void ApInvRemoveDetour();
void ApEquipAfterRemoveDetour();
void ApItemGiveEquipADetour();
void ApItemGiveEquipBDetour();
void ApItemGiveEquipCDetour();
void ApItemGiveEquipDDetour();
void ApItemGiveEquipEDetour();
void ApItemPaintDetour();
void ApItemPaint2Detour();
void ApSanitizePartyInventoriesFromHook();
void ApRebuildItemUiAfterEquipRemove();
void ApLogHookHit(const char* tag);
void ApOnInvAddDone();
void ApOnInvRemoveDone();
void ApOnItemGiveDone(int which);
void ApOnItemPaintDone();
void* g_ap_party_count_resume = nullptr;
void* g_ap_party_char_resume = nullptr;
void* g_ap_battle_party_init_tramp = nullptr;
void* g_ap_menu_fill_continue = nullptr;  // +1C3B5C after stolen mov esi
void* g_ap_menu_fill_skip = nullptr;      // +1C3C8C finalize (after custom faces)
void* g_ap_menu_fill_tramp = nullptr;
void* g_ap_stash_fill_skip = nullptr;     // +1E695C finalize
void* g_ap_stash_fill_tramp = nullptr;
void* g_ap_item_fill_skip = nullptr;      // +1DBF9C finalize
void* g_ap_item_fill_tramp = nullptr;
void* g_ap_inv_add_tramp = nullptr;       // +1E0BA0 bag write
void* g_ap_inv_remove_tramp = nullptr;    // +1E0C30 bag remove/compact
void* g_ap_inv_add_ret = nullptr;
void* g_ap_inv_remove_ret = nullptr;
void* g_ap_equip_after_remove_exit = nullptr;  // +1DDE97
void* g_ap_item_give_a_tramp = nullptr;
void* g_ap_item_give_b_tramp = nullptr;
void* g_ap_item_give_c_tramp = nullptr;
void* g_ap_item_give_d_tramp = nullptr;
void* g_ap_item_give_e_tramp = nullptr;
void* g_ap_item_give_a_ret = nullptr;
void* g_ap_item_give_b_ret = nullptr;
void* g_ap_item_give_c_ret = nullptr;
void* g_ap_item_give_d_ret = nullptr;
void* g_ap_item_give_e_ret = nullptr;
void* g_ap_item_paint_tramp = nullptr;
void* g_ap_item_paint2_tramp = nullptr;
// When set, +56FF0 uses this arena as esi instead of [640E6C] (menu faces only).
void* g_ap_face_bank_override = nullptr;
void* g_ap_face_bank_resume = nullptr;  // +57016 after stolen mov esi,[640E6C]
// Absolute address of dword [640E6C] (ASLR-relocated).
std::uintptr_t g_ap_face_bank_slot_abs = 0;
void* g_ap_battle_ally_count_resume = nullptr;   // +12C4D6 cmp edi,eax
void* g_ap_battle_ally_gate_resume = nullptr;    // +12C4E4 after stolen lea/cmp
void* g_ap_battle_ally_spawn = nullptr;          // +12C601 force-spawn
void* g_ap_battle_ally_char_resume = nullptr;    // +13BD87 after +10F write
void* g_ap_battle_playable_pack_probe_resume = nullptr;
void* g_ap_battle_keyed_task_enqueue_probe_resume = nullptr;
void* g_ap_battle_pack_write_probe_resume = nullptr;
void* g_ap_battle_pack_prep_probe_resume = nullptr;
void* g_ap_battle_load_tramp = nullptr;
void* g_ap_battle_model_bind_tramp = nullptr;
void* g_ap_battle_anim_bind_tramp = nullptr;
void* g_ap_hd_load_content_tramp = nullptr;
// Relocated absolute displacement from the original
// `mov bl, [edx+ecx+disp32]` (preferred 0x6015A1 @ image base 0x400000).
std::uintptr_t g_ap_party_char_table_abs = 0;
}
#endif

namespace {

// Menu/battle roster override is active.
// Field must stay untouched: F11 must NOT call +54F10 / roster() / rewrite MapObj+0A.
// Custom party is applied to menu cache + battle actors only. PGR does not drive
// battle membership (Sue still fought when absent from the PGR file).
constexpr bool kPartyCustomEnabled = true;
constexpr bool kPartyAssetRemapEnabled = false;
// Verbose battle/spawn/P_DAT probe spam — off for normal play; flip to debug fights.
constexpr bool kPartyBattleLogs = false;

#define PartyBattleLog(...) \
    do { \
        if constexpr (kPartyBattleLogs) { \
            ::grandia_ap::LogInfo(__VA_ARGS__); \
        } \
    } while (0)
#define PartyBattleWarn(...) \
    do { \
        if constexpr (kPartyBattleLogs) { \
            ::grandia_ap::LogWarn(__VA_ARGS__); \
        } \
    } while (0)

// +0x7E660 roster apply: count from preset table, then char ids per slot.
constexpr std::uintptr_t kCountLoadRva = 0x7E6ABu;
constexpr std::uintptr_t kCountResumeRva = 0x7E6B3u;
constexpr std::uintptr_t kCharIdLoadRva = 0x7E6D7u;
constexpr std::uintptr_t kCharIdResumeRva = 0x7E6DEu;
constexpr std::uintptr_t kRosterApplyRva = 0x7E660u;
// Tail of +7E660: call +7E9F0 writes field formation positions (causes encounter teleport).
constexpr std::uintptr_t kRosterFormationCallRva = 0x7E94Du;
// +54F10 = field PGR group apply — NEVER used. Field companions must stay as-is;
 // custom party is menu + battle only (user requirement + PGR-swap evidence).
constexpr std::uintptr_t kMapObjPtrRva = 0x23FA94u;
constexpr std::uintptr_t kPresetTableRva = 0x2015A0u;
constexpr std::uintptr_t kMenuPartyCacheRva = 0x30CFF8u;   // VA 0x70CFF8
// Menu rebuild (+1C3B56) copies party IDs from [*70CF90]+0x0A into 70CFF8.
// CRITICAL: 70CF90 == MapObj (both are [640E64]+0x160). NEVER write [*70CF90]+0A —
// that rewrites the field/save roster and breaks encounter registration.
constexpr std::uintptr_t kMenuPartySrcPtrRva = 0x30CF90u;  // VA 0x70CF90 (== MapObj)
// Twin caches for stash/shop (+1E6780) and ITEM assign (+1DBDD0) — same MapObj source,
// separate destinations. Must be hooked like status or they show the field party.
constexpr std::uintptr_t kStashPartyCacheRva = 0x307FD0u;  // VA 0x707FD0
constexpr std::uintptr_t kItemPartyCacheRva = 0x309C68u;   // VA 0x709C68
constexpr std::uintptr_t kEventBitsPtrRva = 0x318BD8u;     // VA 0x718BD8
// Menu party fill: mov esi, [70CF90] then loop copies [*esi+0A] → 70CFF8.
// Hook skips that loop and writes g_ids when override is on.
constexpr std::uintptr_t kMenuFillLoopRva = 0x1C3B56u;
constexpr size_t kMenuFillPatchSize = 6u;
constexpr std::uintptr_t kMenuFillContinueRva = 0x1C3B5Cu;
// After custom faces: stock finalize (+57760 + WINDT via +6900). Do NOT jump to face loop.
constexpr std::uintptr_t kMenuFaceFinalizeRva = 0x1C3C8Cu;
// Stash/shop party fill (mov esi,[707124]) → 707FD0.
constexpr std::uintptr_t kStashFillLoopRva = 0x1E6821u;
constexpr std::uintptr_t kStashFaceFinalizeRva = 0x1E695Cu;
// ITEM assign party fill (mov esi,[709C60]) → 709C68.
constexpr std::uintptr_t kItemFillLoopRva = 0x1DBE66u;
constexpr std::uintptr_t kItemFaceFinalizeRva = 0x1DBF9Cu;
// Menu face loader: ecx=handle, edx=face_i, stack dest. Bank via [640E6C]+0xA3000.
constexpr std::uintptr_t kFaceLoadRva = 0x56FF0u;
constexpr std::uintptr_t kFaceBankLoadRva = 0x57010u;  // mov esi,[640E6C]
constexpr size_t kFaceBankLoadPatchSize = 6u;
constexpr std::uintptr_t kFaceBankLoadResumeRva = 0x57016u;
constexpr std::uintptr_t kFaceBankPtrRva = 0x240E6Cu;   // VA 0x640E6C
constexpr std::uintptr_t kFaceDestPtrRva = 0x240E68u;   // VA 0x640E68
constexpr std::uintptr_t kCostumeStatusTableRva = 0x201D2Fu;  // VA 0x601D2F
constexpr std::uintptr_t kStatusFaceHandleRva = 0x30B34Eu;    // VA 0x70B34E
constexpr std::uintptr_t kStashFaceHandleRva = 0x30719Eu;     // VA 0x70719E
constexpr std::uintptr_t kItemFaceHandleRva = 0x308E2Eu;      // VA 0x708E2E
constexpr size_t kFcPayloadOff = 0xA3000u;
constexpr unsigned kMaxFcRow = 40u;
// 71CD4B game mode: 0 field, 2 battle load, 3 encounter/combat (random fights stay at 3).
// Mode 1 is rare; stock only loads ally slots 1-3 when mode==1 (+56411).
constexpr std::uintptr_t kBattleAllyRegisterRva = 0x7FC20u;  // 47FC20 — IP/combatant grid
constexpr std::uintptr_t kBattleModeRva = 0x31CD4Bu;       // VA 0x71CD4B
constexpr std::uintptr_t kBattleSceneFlagRva = 0x31CD38u;  // VA 0x71CD38
constexpr std::uintptr_t kBattleSnapPtrRva = 0x23FA9Cu;    // VA 0x63FA9C
constexpr std::uintptr_t kBattleCountRva = 0x31BD08u;      // VA 0x71BD08
constexpr std::uintptr_t kBattlePresenceRva = 0x319948u;   // VA 0x719948
constexpr std::uintptr_t kBattleSlotLoadRva = 0x7ECA0u;   // in-combat ally loader (ecx=slot) — not hooked
constexpr std::uintptr_t kCharBattleTypeTableRva = 0x201577u;
constexpr std::uintptr_t kCharBattleStatTableRva = 0x201582u;
constexpr std::uintptr_t kBattlePartyInitRva = 0x81B00u;  // battle party tick (+563F7), before ally 47ECA0
constexpr size_t kBattlePartyInitPatchSize = 8u;
// Random battles set 719464&0x40; stock jne at +56420 skips 47ECA0 for ally slots 1-3.
constexpr std::uintptr_t kBattleAllySlotSkipRva = 0x56420u;
// After successful 47FA90, 47ECA0 does `js +7F4D8` and skips the full ally ATB/IP body
// at +7EE5F. NOP that js so allies continue into 47FC20 + post-+7EE66.
constexpr std::uintptr_t kBattleFa90EarlyExitJsRva = 0x7EDC1u;
constexpr size_t kBattleFa90EarlyExitJsSize = 6u;
// Real random-battle ally spawn (+12C4A7 loop) + char-id write (+13BD81).
// Count: mov eax,[713A78]; movzx eax,[ecx+eax]  → override with g_count.
// Gate: after count check, stock scans MapObj+0A vs formation table — bypass to spawn.
// Char: mov al,[ecx+eax]; mov [esi+0x10F],al → g_ids[slot0].
constexpr std::uintptr_t kBattleAllyCountRva = 0x12C4CDu;       // mov eax,[713A78]
constexpr size_t kBattleAllyCountPatchSize = 5u;
constexpr std::uintptr_t kBattleAllyCountResumeRva = 0x12C4D6u;  // cmp edi,eax
constexpr std::uintptr_t kBattleAllySpawnGateRva = 0x12C4DEu;    // lea eax,[ebx-1]
constexpr size_t kBattleAllySpawnGatePatchSize = 6u;
constexpr std::uintptr_t kBattleAllySpawnGateResumeRva = 0x12C4E4u;
constexpr std::uintptr_t kBattleAllySpawnRva = 0x12C601u;
constexpr std::uintptr_t kBattleAllyCharIdRva = 0x13BD7Eu;  // mov al,[ecx+eax]
constexpr size_t kBattleAllyCharIdPatchSize = 9u;
constexpr std::uintptr_t kBattleAllyCharIdResumeRva = 0x13BD87u;
constexpr std::uintptr_t kBattleFormTablePtrRva = 0x313A78u;  // VA 0x713A78
// Battle load callback (+12BDD0): packs enemy/ally battle assets then spawns allies.
// Must see custom MapObj+0A here or Feena/Gadwin get Sue/empty model slots (crash at +12D760).
constexpr std::uintptr_t kBattleLoadRva = 0x12BDD0u;
constexpr size_t kBattleLoadPatchSize = 6u;
// HdTextureManager::LoadContent (thiscall, 6 stack args, ret 0x18). Arg3 = name logged
// as HdTextureManager::LoadContent(); '%s'. Tester AV lands right after 'win'.
constexpr std::uintptr_t kHdLoadContentRva = 0x26810u;
constexpr size_t kHdLoadContentPatchSize = 5u;
// Diagnostic: no-op HdTextureManager::LoadContent.
// 0=off, 1=skip only "win", 2=skip every call.
constexpr int kSkipHdLoadContentMode = 0;
// Passive probe: stock does `mov al, [ecx+esi+0x64A07]` inside +12C8B0. We log the
// selected key but return the stock value unchanged.
constexpr std::uintptr_t kBattlePlayablePackProbeRva = 0x12C8F5u;
constexpr size_t kBattlePlayablePackProbePatchSize = 7u;
constexpr std::uintptr_t kBattlePlayablePackProbeResumeRva = 0x12C8FCu;
constexpr std::uintptr_t kBattleKeyedTaskEnqueueProbeRva = 0x12C211u;
constexpr size_t kBattleKeyedTaskEnqueueProbePatchSize = 13u;
constexpr std::uintptr_t kBattleKeyedTaskEnqueueProbeResumeRva = 0x12C21Eu;
constexpr std::uintptr_t kPackPathBuilderRva = 0x16A0u;
constexpr std::uintptr_t kPDatPathTemplateRva = 0x20868Cu;  // VA 0x60868c "P_DAT.BIN"
constexpr std::uintptr_t kBattleSchedulerEnqueueRva = 0x96DD0u;
constexpr std::uintptr_t kSchedulerSlotTableRva = 0x30F8E0u;  // VA 0x70F8E0, stride 8
constexpr std::uintptr_t kBattlePackWriteProbeRva = 0x12C773u;
constexpr size_t kBattlePackWriteProbePatchSize = 11u;
constexpr std::uintptr_t kBattlePackWriteProbeResumeRva = 0x12C77Eu;
constexpr std::uintptr_t kBattlePackPrepProbeRva = 0x12C860u;
constexpr size_t kBattlePackPrepProbePatchSize = 9u;
constexpr std::uintptr_t kBattlePackPrepProbeResumeRva = 0x12C869u;
// Model binder +12D730: push ebp; mov ebp,esp; mov eax,[imm32] = 8 bytes (NOT 5 —
// a 5-byte steal truncates mov eax,[imm] and jumps to garbage).
constexpr std::uintptr_t kBattleModelBindRva = 0x12D730u;
constexpr size_t kBattleModelBindPatchSize = 8u;
// Anim table reader +96020: uses [ctx+bank*36+0xa87c]; guard bad pointers from
// custom chars whose mesh banks were never packed.
constexpr std::uintptr_t kBattleAnimBindRva = 0x96020u;
constexpr size_t kBattleAnimBindPatchSize = 9u;
constexpr std::uintptr_t kBattleCtxPtrRva = 0x2D1A98u;  // VA 0x6D1A98
constexpr std::uintptr_t kActorArrayPtrRva = 0x31CD28u;     // VA 0x71CD28
constexpr std::uintptr_t kFieldCtxPtrRva = 0x31CD1Cu;       // VA 0x71CD1C — [+0x0A] must be 2 for 47FA90
// Battle unit list (IP/turn-order for some battle types). Stride 0x90.
// Random field encounters leave this empty — membership is MapObj+0A / +7E660 actors.
constexpr std::uintptr_t kBattleUnitListRva = 0x319BC0u;    // VA 0x719BC0
constexpr unsigned kBattleUnitStride = 0x90u;
constexpr unsigned kBattleUnitSlots = 12u;
constexpr std::uintptr_t kBattleUnitRegisterRva = 0x78EA0u; // type0=party, type1=enemy
constexpr unsigned kActorStride = 0xA0u;
constexpr std::uintptr_t kBattleSnapshotFnRva = 0x61940u;
constexpr std::uintptr_t kBattleSnapshotOff = 0x9Cu;     // [MapObj]+0x9C party bytes for 461940
constexpr std::uintptr_t kCharBlockOff = 0x10Cu;
constexpr std::uintptr_t kCharStride = 0x80u;
constexpr std::uintptr_t kCharEquipOff = 0x4Cu;  // 6×u16 equipped item ids
constexpr unsigned kCharEquipSlots = 6u;
constexpr std::uintptr_t kCharInvOff = 0x58u;  // 12×u16 packed bag (first 0 terminates)
constexpr unsigned kCharInvSlots = 12u;
constexpr unsigned kSlotExcludeBitBase = 0x8F5u;
// Stock bag helpers — add zeros the next slot; remove should compact. Custom party can
// leave mid-list zeros; UI then hides everything after the hole.
constexpr std::uintptr_t kInvAddRva = 0x1E0BA0u;
constexpr std::uintptr_t kInvRemoveRva = 0x1E0C30u;
constexpr size_t kInvOpPatchSize = 6u;
// ITEM-assign widget refresh (ecx = widget): paint only — does NOT rebuild rows from bags.
constexpr std::uintptr_t kItemWidgetRefreshRva = 0x1DE380u;
constexpr std::uintptr_t kItemWidgetARva = 0x308A00u;  // VA 0x708A00
constexpr std::uintptr_t kItemWidgetBRva = 0x308A50u;  // VA 0x708A50
constexpr std::uintptr_t kItemListRebuildRva = 0x1DD6B0u;
constexpr std::uintptr_t kItemListModeObjRva = 0x308A24u;   // VA 0x708A24
constexpr std::uintptr_t kItemUiCurrentRva = 0x30AA8Cu;     // VA 0x70AA8C
constexpr std::uintptr_t kItemUiControllerRva = 0x30AA84u;  // VA 0x70AA84
// After successful equip-from-party bag remove: paint A/B then `mov ecx,5; jmp exit`.
// Stock never rebuilds the row list here — that only happens on leave/re-enter.
constexpr std::uintptr_t kEquipAfterRemoveRva = 0x1DDD1Cu;
constexpr std::uintptr_t kEquipAfterRemoveExitRva = 0x1DDE97u;
constexpr size_t kEquipAfterRemovePatchSize = 10u;
// Outer ITEM give/equip handlers (callback table). Wrap so we rebuild after they return —
// +1DDD1C is only one branch and is often skipped.
constexpr std::uintptr_t kItemGiveEquipARva = 0x1DDB70u;  // mode-4 equip-from-party
constexpr std::uintptr_t kItemGiveEquipBRva = 0x1DE080u;  // give/move (add+remove)
constexpr std::uintptr_t kItemGiveEquipCRva = 0x1DD1A0u;  // inventory shuffle (add+remove)
constexpr std::uintptr_t kItemGiveEquipDRva = 0x1E3770u;  // alt callback in [708A94]
constexpr std::uintptr_t kItemGiveEquipERva = 0x1E5710u;  // stash/equip alt (calls remove)
constexpr size_t kItemGiveEquipPatchSize = 6u;
// Widget paint — fires whenever ITEM panels redraw (covers unknown equip code paths).
constexpr std::uintptr_t kItemPaintRva = 0x1DE380u;
constexpr size_t kItemPaintPatchSize = 7u;  // push esi; mov esi,ecx; cmp word [esi],0
constexpr std::uintptr_t kItemPaint2Rva = 0x1DE3D0u;
constexpr size_t kItemPaint2PatchSize = 8u;  // push edi; xor edi,edi; cmp [ecx],di; jz

const char* CharName(uint8_t id);
void SyncBattlePartyActors(std::uintptr_t base, uint8_t count);
bool CallBattleRosterApply(std::uintptr_t base);
void SnapshotFieldPartyIds(std::uintptr_t base);
void RestoreFieldPartyIdsIfNeeded(std::uintptr_t base);
void PatchFormationTableForBattle(std::uintptr_t base);
void PrepareBattlePartyAssets(std::uintptr_t base, const char* tag);
void LogBattlePartyState(std::uintptr_t base, const char* tag);
int SeedMissingCharacterBlocks(uint8_t* map_obj);
int CompactCharInventory(uint8_t* char_block);
int SanitizeCustomPartyInventories(uint8_t* map_obj);
uint8_t* MapObject(std::uintptr_t base);

// Menu/battle paths hard-reject ids > 8. Leen(11)/Rem(12) need extra patches later.
// 1 Justin, 2 Feena, 3 Sue, 4 Gadwin, 5 Rapp, 6 Milda, 7 Guido, 8 Liete.
constexpr uint8_t kCastPool[] = {1, 2, 3, 4, 5, 6, 7, 8};
constexpr int kCastPoolCount = sizeof(kCastPool) / sizeof(kCastPool[0]);
constexpr uint8_t kMaxPlayableCharId = 8;
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

std::atomic<bool> g_enabled{false};
// 0=vanilla, 1=roulette, 2=unlocks (see kCustomParty*).
std::atomic<unsigned> g_party_mode{0};
std::atomic<bool> g_ap_party_initialized{false};
// Bit N set => character id N unlocked (Justin=bit1). Used in unlocks / roulette.
std::atomic<uint32_t> g_unlock_mask{0};
std::atomic<uint8_t> g_count{1};
uint8_t g_ids[4] = {1, 0, 0, 0};  // default edit buffer: Justin only
uint8_t g_selected_slot = 0;

// 0xFF = no remap; 0xFE = use party_override merged CPD; else stock group index.
std::atomic<uint8_t> g_asset_group{0xFFu};
char g_merged_cpd_path[MAX_PATH]{};

int g_count_hook_logs_left = 0;
int g_char_hook_logs_left = 0;
int g_battle_ally_hook_logs_left = 0;
int g_battle_pdat_probe_logs_left = 0;
int g_battle_mode_diag_logs_left = 0;
uint8_t g_last_battle_mode = 0xFFu;

void* g_count_site = nullptr;
void* g_char_site = nullptr;
uint8_t g_count_original[8]{};
uint8_t g_char_original[8]{};
bool g_installed = false;
bool g_battle_custom_spawn_done = false;
bool g_battle_party_init_synced = false;
bool g_battle_allies_registered = false;
int g_battle_unit_wait_logs_left = 0;
void* g_battle_party_init_site = nullptr;
uint8_t g_battle_party_init_original[8]{};
void* g_battle_party_init_trampoline_mem = nullptr;
void* g_battle_ally_gate_site = nullptr;
uint8_t g_battle_ally_gate_original[2]{};
void* g_battle_fa90_js_site = nullptr;
uint8_t g_battle_fa90_js_original[6]{};
void* g_menu_fill_site = nullptr;
uint8_t g_menu_fill_original[6]{};
void* g_menu_fill_trampoline_mem = nullptr;
void* g_stash_fill_site = nullptr;
uint8_t g_stash_fill_original[6]{};
void* g_stash_fill_trampoline_mem = nullptr;
void* g_item_fill_site = nullptr;
uint8_t g_item_fill_original[6]{};
void* g_item_fill_trampoline_mem = nullptr;
void* g_inv_add_site = nullptr;
uint8_t g_inv_add_original[6]{};
void* g_inv_add_trampoline_mem = nullptr;
void* g_inv_remove_site = nullptr;
uint8_t g_inv_remove_original[6]{};
void* g_inv_remove_trampoline_mem = nullptr;
void* g_equip_after_remove_site = nullptr;
uint8_t g_equip_after_remove_original[16]{};
void* g_item_give_a_site = nullptr;
uint8_t g_item_give_a_original[6]{};
void* g_item_give_a_trampoline_mem = nullptr;
void* g_item_give_b_site = nullptr;
uint8_t g_item_give_b_original[6]{};
void* g_item_give_b_trampoline_mem = nullptr;
void* g_item_give_c_site = nullptr;
uint8_t g_item_give_c_original[6]{};
void* g_item_give_c_trampoline_mem = nullptr;
void* g_item_give_d_site = nullptr;
uint8_t g_item_give_d_original[6]{};
void* g_item_give_d_trampoline_mem = nullptr;
void* g_item_give_e_site = nullptr;
uint8_t g_item_give_e_original[6]{};
void* g_item_give_e_trampoline_mem = nullptr;
void* g_item_paint_site = nullptr;
uint8_t g_item_paint_original[8]{};
void* g_item_paint_trampoline_mem = nullptr;
void* g_item_paint2_site = nullptr;
uint8_t g_item_paint2_original[8]{};
void* g_item_paint2_trampoline_mem = nullptr;
bool g_in_item_ui_fix = false;
DWORD g_last_item_ui_rebuild_tick = 0;
int g_item_paint_hit_logs_left = 8;
void* g_face_bank_site = nullptr;
uint8_t g_face_bank_original[6]{};
uint8_t* g_fc_arenas[kMaxFcRow]{};
void* g_battle_ally_count_site = nullptr;
uint8_t g_battle_ally_count_original[8]{};
void* g_battle_ally_spawn_gate_site = nullptr;
uint8_t g_battle_ally_spawn_gate_original[8]{};
void* g_battle_ally_char_site = nullptr;
uint8_t g_battle_ally_char_original[16]{};
void* g_battle_ally_bind_tail_site = nullptr;
uint8_t g_battle_ally_bind_tail_original[16]{};
void* g_battle_ally_bind_tail_trampoline_mem = nullptr;
void* g_battle_load_site = nullptr;
uint8_t g_battle_load_original[8]{};
void* g_battle_load_trampoline_mem = nullptr;
void* g_hd_load_content_site = nullptr;
uint8_t g_hd_load_content_original[8]{};
void* g_hd_load_content_trampoline_mem = nullptr;
void* g_battle_playable_pack_probe_site = nullptr;
uint8_t g_battle_playable_pack_probe_original[8]{};
void* g_battle_keyed_task_enqueue_probe_site = nullptr;
uint8_t g_battle_keyed_task_enqueue_probe_original[16]{};
void* g_battle_pack_write_probe_site = nullptr;
uint8_t g_battle_pack_write_probe_original[16]{};
void* g_battle_pack_prep_probe_site = nullptr;
uint8_t g_battle_pack_prep_probe_original[16]{};
void* g_battle_model_bind_site = nullptr;
uint8_t g_battle_model_bind_original[16]{};
void* g_battle_model_bind_trampoline_mem = nullptr;
void* g_battle_anim_bind_site = nullptr;
uint8_t g_battle_anim_bind_original[16]{};
void* g_battle_anim_bind_trampoline_mem = nullptr;
void* g_battle_anim_walk_site = nullptr;
uint8_t g_battle_anim_walk_original[16]{};
void* g_battle_anim_walk_trampoline_mem = nullptr;
void* g_crash_veh = nullptr;
std::atomic<int> g_crash_logs_left{8};
bool g_battle_assets_prepared = false;
bool g_battle_pack_rebuilt = false;
// party spawn slot -> c8a header index (mesh). Allows arbitrary roster order while
// keeping stock-loaded pack blobs at their native c8a indices.
uint8_t g_party_slot_to_c8a[4] = {0, 1, 2, 3};
uint32_t g_ally_combatants[4]{};
uint8_t g_ally_combatant_logged = 0;
void* g_battle_scheduler_enqueue = nullptr;
void* g_battle_keyed_task_callback = nullptr;
uint8_t g_battle_keyed_task_last_free_slot = 0;
uint32_t g_battle_keyed_task_last_owner = 0;
uint32_t g_battle_keyed_task_last_task = 0;
bool g_battle_keyed_tasks_injected = false;
void* g_e6a0_alloc[4]{};
uint32_t g_e6a0_alloc_size[4]{};
// External P_DAT blobs (outside battle_ctx+c8a). Prefer one contiguous assembled
// pack in slot[0] (stock layout; anim spill stays valid). Per-slot allocs are
// fallback. Never memcpy large packs into ctx+0xC8A00 — ≳184KiB stomps scratch
// at ctx+0xF5A00..0x113A00 (+97240 anim AV).
void* g_pdat_slot_alloc[4]{};
uint32_t g_pdat_slot_alloc_size[4]{};
bool g_pdat_alloc_is_contiguous = false;

// Field MapObj+0A saved for the override session. Random battles inherit the field
// roster (Justin+Sue) — 719BC0 stays empty — so we temporarily write MapObj+0A and
// run formation-NOP'd +7E660 once per fight, then restore MapObj + actors after.
uint8_t g_saved_field0a[4]{};
bool g_field0a_saved_for_battle = false;
uint8_t g_saved_field_actors[4u * kActorStride]{};
bool g_field_actors_saved = false;
bool g_battle_roster_applied = false;

// Until battle assets/formation for custom allies are solved, keep encounters alive by
// only activating the leader in ATB/47ECA0. Menu roster stays full.
// Set false now that the 719950-nulling memset bug is fixed — retest full party.
constexpr bool kBattleClampAlliesToLeader = false;

LONG CALLBACK ApPartyCrashVeh(EXCEPTION_POINTERS* info) {
    if (!info || !info->ExceptionRecord) {
        return EXCEPTION_CONTINUE_SEARCH;
    }
    const DWORD code = info->ExceptionRecord->ExceptionCode;
    if (code != EXCEPTION_ACCESS_VIOLATION && code != EXCEPTION_ILLEGAL_INSTRUCTION) {
        return EXCEPTION_CONTINUE_SEARCH;
    }
    if (g_crash_logs_left.fetch_sub(1) <= 0) {
        return EXCEPTION_CONTINUE_SEARCH;
    }
    const std::uintptr_t base = grandia_ap::GetGrandiaModuleBase();
    const auto at = reinterpret_cast<std::uintptr_t>(info->ExceptionRecord->ExceptionAddress);
    const ULONG_PTR av_addr =
        info->ExceptionRecord->NumberParameters >= 2 ? info->ExceptionRecord->ExceptionInformation[1]
                                                     : 0;
    grandia_ap::LogWarn(
        "Party crash VEH: code=0x%08X at=%p (grandia+0x%X) av_addr=%p mode=%u 71BD08=%u",
        static_cast<unsigned>(code), reinterpret_cast<void*>(at),
        base ? static_cast<unsigned>(at - base) : 0u, reinterpret_cast<void*>(av_addr),
        base ? static_cast<unsigned>(*reinterpret_cast<uint8_t*>(base + 0x31CD4Bu)) : 0u,
        base ? static_cast<unsigned>(*reinterpret_cast<uint16_t*>(base + 0x31BD08u)) : 0u);
    return EXCEPTION_CONTINUE_SEARCH;
}

void ShowPartyDiagPopup(const char* text) {
    if (!text || text[0] == '\0') {
        return;
    }
    MessageBoxA(nullptr, text, "Grandiarchipelago Party Debug", MB_OK | MB_TOPMOST);
}

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

void SetBattleAllyGatePatch(std::uintptr_t base, bool enable) {
    if (base == 0) {
        return;
    }
    auto* site = reinterpret_cast<uint8_t*>(base + kBattleAllySlotSkipRva);
    if (!g_battle_ally_gate_site) {
        if (site[0] != 0x75 || site[1] != 0x15) {
            PartyBattleWarn("Party battle: ally slot gate mismatch at +0x%X",
                                static_cast<unsigned>(kBattleAllySlotSkipRva));
            return;
        }
        std::memcpy(g_battle_ally_gate_original, site, sizeof(g_battle_ally_gate_original));
        g_battle_ally_gate_site = site;
    }
    const bool already = (site[0] == 0x90 && site[1] == 0x90);
    if (enable == already) {
        return;
    }
    const uint8_t patch[2] = {0x90, 0x90};
    RestoreBytes(site, enable ? patch : g_battle_ally_gate_original, sizeof(patch));
    if (enable) {
        PartyBattleLog("Party battle: ally slot gate bypass ON (+0x%X)", kBattleAllySlotSkipRva);
    }
}

void SetBattleFa90EarlyExitPatch(std::uintptr_t base, bool enable) {
    if (base == 0) {
        return;
    }
    auto* site = reinterpret_cast<uint8_t*>(base + kBattleFa90EarlyExitJsRva);
    if (!g_battle_fa90_js_site) {
        // 0F 88 xx xx xx xx = js rel32
        if (site[0] != 0x0F || site[1] != 0x88) {
            PartyBattleWarn("Party battle: 47FA90 early-exit js mismatch at +0x%X",
                                static_cast<unsigned>(kBattleFa90EarlyExitJsRva));
            return;
        }
        std::memcpy(g_battle_fa90_js_original, site, kBattleFa90EarlyExitJsSize);
        g_battle_fa90_js_site = site;
    }
    bool already = true;
    for (size_t i = 0; i < kBattleFa90EarlyExitJsSize; ++i) {
        if (site[i] != 0x90) {
            already = false;
            break;
        }
    }
    if (enable == already) {
        return;
    }
    uint8_t nops[6] = {0x90, 0x90, 0x90, 0x90, 0x90, 0x90};
    RestoreBytes(site, enable ? nops : g_battle_fa90_js_original, kBattleFa90EarlyExitJsSize);
    if (enable) {
        PartyBattleLog(
            "Party battle: 47FA90 early-exit bypass ON (+0x%X) — allies take full IP path",
            static_cast<unsigned>(kBattleFa90EarlyExitJsRva));
    }
}

uint8_t* FieldContext(std::uintptr_t base) {
    if (base == 0) {
        return nullptr;
    }
    return *reinterpret_cast<uint8_t**>(base + kFieldCtxPtrRva);
}

void SyncBattleFieldContext(std::uintptr_t base) {
    // Do NOT force ctx+0A=2. That makes 47FA90 return -1 (js early-exit) and sends
    // follower init (+80520) down the 63FA98 formation path. Stock random fights often
    // leave ctx+0A alone; Justin works without this. Forcing it was self-inflicted.
    (void)base;
}

void PatchBattleActorIds(std::uintptr_t base, uint8_t count) {
    uint8_t* actors = *reinterpret_cast<uint8_t**>(base + kActorArrayPtrRva);
    if (!actors) {
        return;
    }
    __try {
        uint8_t* leader = actors;
        for (uint8_t slot = 0; slot < 4; ++slot) {
            uint8_t* actor = actors + static_cast<std::uintptr_t>(slot) * kActorStride;
            if (slot >= count || g_ids[slot] == 0) {
                actor[1] = 0;
                continue;
            }
            const uint8_t id = g_ids[slot];
            // New battle slots (e.g. Gadwin when field only had Justin+Sue) start empty —
            // clone leader then overlay identity so ATB/IP have a full actor row.
            if (slot > 0 && actor[1] == 0 && leader[1] != 0) {
                std::memcpy(actor, leader, static_cast<size_t>(kActorStride));
            }
            actor[2] = id;
            actor[3] = slot;
            actor[1] = (slot == 0) ? 1 : 2;
            *reinterpret_cast<uint16_t*>(actor + 0x1C) = 0;
            // Stock +7E71D / +7E726: battle type + stat word from char-id tables.
            actor[0x18] = *reinterpret_cast<const uint8_t*>(base + kCharBattleTypeTableRva + id);
            *reinterpret_cast<uint16_t*>(actor + 0x30) =
                *reinterpret_cast<const uint16_t*>(base + kCharBattleStatTableRva +
                                                   static_cast<std::uintptr_t>(id) * 2u);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        PartyBattleWarn("Party battle: PatchBattleActorIds faulted");
    }
}

bool BattleUnitListHasActorSlot(std::uintptr_t base, uint8_t actor_slot) {
    auto* units = reinterpret_cast<uint8_t*>(base + kBattleUnitListRva);
    for (unsigned i = 0; i < kBattleUnitSlots; ++i) {
        uint8_t* unit = units + static_cast<std::uintptr_t>(i) * kBattleUnitStride;
        if ((unit[0] & 0x0F) == 0) {
            continue;
        }
        if ((unit[2] & 0x7F) != 0) {
            continue;
        }
        if (*reinterpret_cast<uint16_t*>(unit + 0x0A) == actor_slot) {
            return true;
        }
    }
    return false;
}

bool BattleUnitListHasCharId(std::uintptr_t base, uint8_t char_id) {
    auto* units = reinterpret_cast<uint8_t*>(base + kBattleUnitListRva);
    for (unsigned i = 0; i < kBattleUnitSlots; ++i) {
        uint8_t* unit = units + static_cast<std::uintptr_t>(i) * kBattleUnitStride;
        if ((unit[0] & 0x0F) == 0) {
            continue;
        }
        if ((unit[2] & 0x7F) != 0) {
            continue;
        }
        if (unit[1] == char_id) {
            return true;
        }
    }
    return false;
}

// Stock +78EA0 finishes with unit[0]=0x41 (bit 0x40 = fully armed). Partial rows
// (f01/f02) from a failed register are not safe templates.
bool BattleUnitIsComplete(const uint8_t* unit) {
    return unit && (unit[0] & 0x40) != 0 && (unit[0] & 0x0F) != 0;
}

int FindFreeBattleUnitSlot(std::uintptr_t base) {
    auto* units = reinterpret_cast<uint8_t*>(base + kBattleUnitListRva);
    for (unsigned i = 0; i < kBattleUnitSlots; ++i) {
        uint8_t* unit = units + static_cast<std::uintptr_t>(i) * kBattleUnitStride;
        if ((unit[0] & 0x0F) == 0) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

uint8_t* FindCompletePartyUnitTemplate(std::uintptr_t base) {
    auto* units = reinterpret_cast<uint8_t*>(base + kBattleUnitListRva);
    uint8_t* any = nullptr;
    for (unsigned i = 0; i < kBattleUnitSlots; ++i) {
        uint8_t* unit = units + static_cast<std::uintptr_t>(i) * kBattleUnitStride;
        if (!BattleUnitIsComplete(unit)) {
            continue;
        }
        if ((unit[2] & 0x7F) != 0) {
            continue;
        }
        // Prefer leader / Justin row.
        if (*reinterpret_cast<const uint16_t*>(unit + 0x0A) == 0 || unit[1] == 1) {
            return unit;
        }
        if (!any) {
            any = unit;
        }
    }
    return any;
}

uint8_t* FindPartyUnitByCharId(std::uintptr_t base, uint8_t char_id) {
    auto* units = reinterpret_cast<uint8_t*>(base + kBattleUnitListRva);
    for (unsigned i = 0; i < kBattleUnitSlots; ++i) {
        uint8_t* unit = units + static_cast<std::uintptr_t>(i) * kBattleUnitStride;
        if ((unit[0] & 0x0F) == 0) {
            continue;
        }
        if ((unit[2] & 0x7F) != 0) {
            continue;
        }
        if (unit[1] == char_id) {
            return unit;
        }
    }
    return nullptr;
}

// Drop party rows whose unit[1] is not in the custom roster (e.g. field Sue).
int PruneForeignBattlePartyUnits(std::uintptr_t base, uint8_t count) {
    int pruned = 0;
    auto* units = reinterpret_cast<uint8_t*>(base + kBattleUnitListRva);
    __try {
        for (unsigned i = 0; i < kBattleUnitSlots; ++i) {
            uint8_t* unit = units + static_cast<std::uintptr_t>(i) * kBattleUnitStride;
            if ((unit[0] & 0x0F) == 0) {
                continue;
            }
            if ((unit[2] & 0x7F) != 0) {
                continue;
            }
            const uint8_t cid = unit[1];
            bool wanted = false;
            for (uint8_t s = 0; s < count; ++s) {
                if (g_ids[s] != 0 && g_ids[s] == cid) {
                    wanted = true;
                    break;
                }
            }
            if (!wanted) {
                PartyBattleLog("Party battle: 719BC0 prune unit[%u] id=%u (%s)", i, cid,
                                    CharName(cid));
                unit[0] = 0;
                ++pruned;
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1;
    }
    return pruned;
}

// Clone a stock-complete 719BC0 party row. Do NOT call +78EA0 with fabricated
// ecx/edx — that hits [71C1A8] with a null ptr (+790A1 AV).
bool WriteClonedBattlePartyUnit(std::uintptr_t base, uint8_t* templ, uint8_t actor_slot,
                                uint8_t char_id) {
    if (!templ || actor_slot >= 4 || char_id == 0) {
        return false;
    }
    const int free_i = FindFreeBattleUnitSlot(base);
    if (free_i < 0) {
        PartyBattleWarn("Party battle: 719BC0 full, cannot add id=%u", char_id);
        return false;
    }
    __try {
        auto* unit = reinterpret_cast<uint8_t*>(base + kBattleUnitListRva) +
                     static_cast<std::uintptr_t>(free_i) * kBattleUnitStride;
        std::memcpy(unit, templ, kBattleUnitStride);
        unit[0] = 0x41;
        unit[1] = char_id;
        unit[2] = 0;  // party type
        *reinterpret_cast<uint16_t*>(unit + 0x0A) = actor_slot;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        PartyBattleWarn("Party battle: clone unit faulted id=%u", char_id);
        return false;
    }
}

// Wait for stock to finish Justin (bit0x40), then clone that row for missing allies.
// Returns -1 if not ready, else number of units added/repaired.
int SyncBattleCombatantList(std::uintptr_t base, uint8_t count) {
    if (base == 0 || count == 0) {
        return 0;
    }
    const uint8_t cd38 = *reinterpret_cast<uint8_t*>(base + kBattleSceneFlagRva);
    if (cd38 != 0) {
        return -1;
    }

    uint8_t* templ = FindCompletePartyUnitTemplate(base);
    if (!templ) {
        return -1;  // stock has not finished registering Justin yet
    }

    int changed = 0;
    __try {
        PruneForeignBattlePartyUnits(base, count);

        // Repair incomplete custom rows left by a prior crashed +78EA0 attempt.
        for (uint8_t slot = 0; slot < count; ++slot) {
            const uint8_t id = g_ids[slot];
            if (id == 0) {
                continue;
            }
            uint8_t* existing = FindPartyUnitByCharId(base, id);
            if (existing && !BattleUnitIsComplete(existing)) {
                std::memcpy(existing, templ, kBattleUnitStride);
                existing[0] = 0x41;
                existing[1] = id;
                existing[2] = 0;
                *reinterpret_cast<uint16_t*>(existing + 0x0A) = slot;
                PartyBattleLog("Party battle: 719BC0 repaired id=%u (%s) slot=%u", id,
                                    CharName(id), slot);
                ++changed;
            }
        }

        for (uint8_t slot = 0; slot < count; ++slot) {
            const uint8_t id = g_ids[slot];
            if (id == 0) {
                continue;
            }
            if (BattleUnitListHasCharId(base, id)) {
                // Ensure actor_slot matches our dense index.
                if (uint8_t* u = FindPartyUnitByCharId(base, id)) {
                    *reinterpret_cast<uint16_t*>(u + 0x0A) = slot;
                }
                continue;
            }
            if (BattleUnitListHasActorSlot(base, slot)) {
                // Wrong char on this slot — overwrite id on that row from template.
                auto* units = reinterpret_cast<uint8_t*>(base + kBattleUnitListRva);
                for (unsigned i = 0; i < kBattleUnitSlots; ++i) {
                    uint8_t* unit = units + static_cast<std::uintptr_t>(i) * kBattleUnitStride;
                    if ((unit[0] & 0x0F) == 0 || (unit[2] & 0x7F) != 0) {
                        continue;
                    }
                    if (*reinterpret_cast<uint16_t*>(unit + 0x0A) != slot) {
                        continue;
                    }
                    std::memcpy(unit, templ, kBattleUnitStride);
                    unit[0] = 0x41;
                    unit[1] = id;
                    unit[2] = 0;
                    *reinterpret_cast<uint16_t*>(unit + 0x0A) = slot;
                    PartyBattleLog("Party battle: 719BC0 remap slot=%u -> id=%u (%s)", slot, id,
                                        CharName(id));
                    ++changed;
                    break;
                }
                continue;
            }
            if (WriteClonedBattlePartyUnit(base, templ, slot, id)) {
                PartyBattleLog("Party battle: 719BC0 cloned id=%u (%s) slot=%u", id,
                                    CharName(id), slot);
                ++changed;
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        PartyBattleWarn("Party battle: SyncBattleCombatantList faulted");
        return -1;
    }
    return changed;
}

void LogBattleUnitList(std::uintptr_t base) {
    if constexpr (!kPartyBattleLogs) {
        (void)base;
        return;
    }
    auto* units = reinterpret_cast<uint8_t*>(base + kBattleUnitListRva);
    char line[384]{};
    size_t used = 0;
    used += static_cast<size_t>(
        std::snprintf(line + used, sizeof(line) - used, "Party battle units:"));
    for (unsigned i = 0; i < 6u && used + 56 < sizeof(line); ++i) {
        uint8_t* unit = units + static_cast<std::uintptr_t>(i) * kBattleUnitStride;
        const uint8_t flags = unit[0];
        const uint8_t typ = unit[2];
        const uint8_t cid = unit[1];
        const uint16_t aslot = *reinterpret_cast<uint16_t*>(unit + 0x0A);
        used += static_cast<size_t>(std::snprintf(
            line + used, sizeof(line) - used, " [%u]=f%02X t%02X id%u(%s) slot%u", i, flags, typ,
            static_cast<unsigned>(cid), CharName(cid), static_cast<unsigned>(aslot)));
    }
    grandia_ap::LogInfo("%s", line);
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

uint8_t BattlePlayableSelectorForChar(uint8_t char_id) {
    switch (char_id) {
        case 1:
            return 0;  // Justin -> P1
        case 3:
            return 1;  // Sue -> P2
        case 2:
            return 2;  // Feena -> P3
        case 4:
            return 3;  // Gadwin -> P4
        case 5:
            return 4;  // Rapp -> P5
        case 6:
            return 5;  // Milda -> P6
        case 7:
            return 6;  // Guido -> P7
        case 8:
            return 7;  // Liete -> P8
        default:
            return 0;
    }
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
    if constexpr (!kPartyAssetRemapEnabled) {
        g_asset_group.store(0xFFu);
        g_merged_cpd_path[0] = '\0';
        return;
    }
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

void FormatRoster(char* buf, size_t buf_size, const char* prefix) {
    const uint8_t n = g_count.load();
    if (n == 1) {
        std::snprintf(buf, buf_size, "%s %s", prefix, CharName(g_ids[0]));
    } else if (n == 2) {
        std::snprintf(buf, buf_size, "%s %s+%s", prefix, CharName(g_ids[0]), CharName(g_ids[1]));
    } else if (n == 3) {
        std::snprintf(buf, buf_size, "%s %s+%s+%s", prefix, CharName(g_ids[0]), CharName(g_ids[1]),
                      CharName(g_ids[2]));
    } else {
        std::snprintf(buf, buf_size, "%s %s+%s+%s+%s", prefix, CharName(g_ids[0]), CharName(g_ids[1]),
                      CharName(g_ids[2]), CharName(g_ids[3]));
    }
}

void ToastParty(const char* prefix) {
    char buf[160];
    FormatRoster(buf, sizeof(buf), prefix);
    const uint8_t n = g_count.load();
    if (g_selected_slot < n) {
        char full[192];
        std::snprintf(full, sizeof(full), "%s  (slot %u=%s)", buf,
                      static_cast<unsigned>(g_selected_slot + 1), CharName(g_ids[g_selected_slot]));
        grandia_ap::ShowD3dOverlayToast(full, 3500, 0x7CFC00u);
    } else {
        grandia_ap::ShowD3dOverlayToast(buf, 3500, 0x7CFC00u);
    }
}

bool IdInRoster(uint8_t id, uint8_t except_slot = 0xFF) {
    const uint8_t n = g_count.load();
    for (uint8_t i = 0; i < n; ++i) {
        if (i != except_slot && g_ids[i] == id) {
            return true;
        }
    }
    return false;
}

int CastPoolIndex(uint8_t id) {
    for (int i = 0; i < kCastPoolCount; ++i) {
        if (kCastPool[i] == id) {
            return i;
        }
    }
    return 0;
}

void ClampSelectedSlot() {
    const uint8_t n = g_count.load();
    if (n == 0) {
        g_selected_slot = 0;
        return;
    }
    if (g_selected_slot >= n) {
        g_selected_slot = static_cast<uint8_t>(n - 1);
    }
}

void CycleSelectedSlot() {
    ClampSelectedSlot();
    const uint8_t n = g_count.load();
    if (n == 0) {
        return;
    }
    g_selected_slot = static_cast<uint8_t>((g_selected_slot + 1) % n);
    ToastParty("Slot:");
}

bool CharacterUnlocked(uint8_t id) {
    if (id < 1 || id > kMaxPlayableCharId) {
        return false;
    }
    const unsigned mode = g_party_mode.load();
    if (mode == grandia_ap::kCustomPartyVanilla) {
        return true;
    }
    // Roulette + unlocks: gated by unlock mask.
    return (g_unlock_mask.load() & (1u << id)) != 0;
}

uint8_t CountUnlockedCharacters() {
    if (g_party_mode.load() == grandia_ap::kCustomPartyVanilla) {
        return static_cast<uint8_t>(kCastPoolCount);
    }
    uint8_t n = 0;
    for (uint8_t id = 1; id <= kMaxPlayableCharId; ++id) {
        if (CharacterUnlocked(id)) {
            ++n;
        }
    }
    return n;
}

uint8_t MaxPartySlots() {
    if (g_party_mode.load() != grandia_ap::kCustomPartyUnlocks) {
        return 4;
    }
    const uint8_t unlocked = CountUnlockedCharacters();
    return unlocked < 4 ? unlocked : 4;
}

void CycleCharInSelectedSlot(int dir = 1, bool toast = true) {
    ClampSelectedSlot();
    const uint8_t n = g_count.load();
    if (n == 0) {
        return;
    }
    const int start = CastPoolIndex(g_ids[g_selected_slot]);
    const int step_dir = (dir < 0) ? -1 : 1;
    for (int step = 1; step <= kCastPoolCount; ++step) {
        int idx = (start + step * step_dir) % kCastPoolCount;
        if (idx < 0) {
            idx += kCastPoolCount;
        }
        const uint8_t candidate = kCastPool[idx];
        if (!CharacterUnlocked(candidate)) {
            continue;
        }
        if (!IdInRoster(candidate, g_selected_slot)) {
            g_ids[g_selected_slot] = candidate;
            RefreshAssetBinding();
            if (toast) {
                ToastParty("Set:");
            }
            return;
        }
    }
    if (toast) {
        grandia_ap::ShowD3dOverlayToast("No unused unlocked cast member", 2500, 0xFA8072u);
    }
}

void AddPartyMember(bool toast = true) {
    uint8_t n = g_count.load();
    const uint8_t max_slots = MaxPartySlots();
    if (n >= max_slots) {
        if (toast) {
            if (max_slots < 4) {
                grandia_ap::ShowD3dOverlayToast("Need more party members from AP", 2500, 0xFA8072u);
            } else {
                grandia_ap::ShowD3dOverlayToast("Party full (max 4)", 2500, 0xFA8072u);
            }
        }
        return;
    }
    for (int i = 0; i < kCastPoolCount; ++i) {
        const uint8_t id = kCastPool[i];
        if (!CharacterUnlocked(id)) {
            continue;
        }
        if (!IdInRoster(id)) {
            g_ids[n] = id;
            ++n;
            g_count.store(n);
            g_selected_slot = static_cast<uint8_t>(n - 1);
            RefreshAssetBinding();
            if (toast) {
                ToastParty("Added:");
            }
            return;
        }
    }
    if (toast) {
        grandia_ap::ShowD3dOverlayToast("No unused unlocked cast member", 2500, 0xFA8072u);
    }
}

void RemovePartyMember(bool toast = true) {
    uint8_t n = g_count.load();
    if (n <= 1) {
        if (toast) {
            grandia_ap::ShowD3dOverlayToast("Need at least 1 member", 2500, 0xFA8072u);
        }
        return;
    }
    // Remove selected slot (shift down); default feel = drop last if on last.
    for (uint8_t i = g_selected_slot; i + 1 < n; ++i) {
        g_ids[i] = g_ids[i + 1];
    }
    g_ids[n - 1] = 0;
    --n;
    g_count.store(n);
    ClampSelectedSlot();
    RefreshAssetBinding();
    if (toast) {
        ToastParty("Removed:");
    }
}

bool ModDown(int vk) { return (GetAsyncKeyState(vk) & 0x8000) != 0; }

uint8_t* MapObject(std::uintptr_t base) {
    return *reinterpret_cast<uint8_t**>(base + kMapObjPtrRva);
}

uint8_t* CharBlock(uint8_t* map_obj, uint8_t char_id) {
    if (!map_obj || char_id < 1 || char_id > kMaxPlayableCharId) {
        return nullptr;
    }
    return map_obj + kCharBlockOff + static_cast<std::uintptr_t>(char_id - 1) * kCharStride;
}

// Pack char+0x58 to a contiguous prefix of non-zero item ids (stock UI stops at first 0).
// Returns 1 if the bag changed.
int CompactCharInventory(uint8_t* char_block) {
    if (!char_block) {
        return 0;
    }
    auto* slots = reinterpret_cast<uint16_t*>(char_block + kCharInvOff);
    uint16_t packed[kCharInvSlots]{};
    unsigned n = 0;
    for (unsigned i = 0; i < kCharInvSlots; ++i) {
        if (slots[i] != 0) {
            packed[n++] = slots[i];
        }
    }
    bool dirty = false;
    for (unsigned i = 0; i < kCharInvSlots; ++i) {
        if (slots[i] != packed[i]) {
            dirty = true;
            break;
        }
    }
    if (!dirty) {
        return 0;
    }
    std::memcpy(slots, packed, sizeof(packed));
    return 1;
}

int SanitizeCustomPartyInventories(uint8_t* map_obj) {
    if (!map_obj || !g_enabled.load()) {
        return 0;
    }
    int fixed = 0;
    const uint8_t n = g_count.load();
    for (uint8_t i = 0; i < n; ++i) {
        const uint8_t id = g_ids[i];
        auto* blk = CharBlock(map_obj, id);
        if (!blk) {
            continue;
        }
        if (CompactCharInventory(blk)) {
            ++fixed;
            grandia_ap::LogInfo("Party: compacted bag holes for id=%u (%s)", id, CharName(id));
        }
    }
    return fixed;
}

// Re-draw ITEM-assign panels so an already-open inventory picks up compacted bags
// without requiring the player to leave and re-enter the menu.
void RefreshItemAssignWidgets(std::uintptr_t base) {
    if (base == 0) {
        return;
    }
    auto* refresh = reinterpret_cast<void(__fastcall*)(void*)>(base + kItemWidgetRefreshRva);
    auto* widget_a = reinterpret_cast<void*>(base + kItemWidgetARva);
    auto* widget_b = reinterpret_cast<void*>(base + kItemWidgetBRva);
    __try {
        // Skip if the ITEM UI widgets look inactive (first word 0 — same guard as stock).
        if (*reinterpret_cast<uint16_t*>(widget_a) != 0) {
            refresh(widget_a);
        }
        if (*reinterpret_cast<uint16_t*>(widget_b) != 0) {
            refresh(widget_b);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

// After +1DD6B0 AV, back off so Present/hooks don't spam the same bad UI state.
DWORD g_item_rebuild_cooldown_until = 0;

// Rebuild the merged party item rows from character bags (same path as leave/re-enter).
// Only safe in real ITEM-assign mode (==4). Field pickup / other UIs reuse the same
// widget words and crash inside +1DD6B0 if we force mode 4 (seen: mode 254 → AV @ +1DD700).
void RebuildItemAssignList(std::uintptr_t base) {
    if (base == 0) {
        return;
    }
    const DWORD now = GetTickCount();
    if (now < g_item_rebuild_cooldown_until) {
        return;
    }

    uint16_t* mode_word = nullptr;
    uint16_t saved_mode = 0;
    auto** ui_current = reinterpret_cast<void**>(base + kItemUiCurrentRva);
    void* saved_current = nullptr;
    bool touched_current = false;

    __try {
        auto* widget_b = reinterpret_cast<uint8_t*>(base + kItemWidgetBRva);
        auto* widget_a = reinterpret_cast<uint8_t*>(base + kItemWidgetARva);
        void* arg = nullptr;
        if (*reinterpret_cast<uint16_t*>(widget_b) != 0) {
            arg = widget_b;
        } else if (*reinterpret_cast<uint16_t*>(widget_a) != 0) {
            arg = widget_a;
        } else {
            return;
        }

        auto* mode_obj = reinterpret_cast<uint8_t*>(base + kItemListModeObjRva);
        mode_word = mode_obj ? reinterpret_cast<uint16_t*>(mode_obj + 2) : nullptr;
        const uint16_t mode = mode_word ? *mode_word : 0;
        if (mode != 4) {
            static int skip_logs_left = 6;
            if (skip_logs_left > 0) {
                --skip_logs_left;
                grandia_ap::LogInfo(
                    "Party: ITEM list rebuild skipped (mode=%u, need 4 — field pickup?)",
                    static_cast<unsigned>(mode));
            }
            return;
        }
        saved_mode = mode;

        // +1DD6B0 early-outs when [70AA8C] == [70AA84]; force a mismatch like stock's
        // post-equip path that writes 708A00 into 70AA8C.
        auto** ui_controller = reinterpret_cast<void**>(base + kItemUiControllerRva);
        saved_current = ui_current ? *ui_current : nullptr;
        if (ui_current && ui_controller && *ui_current == *ui_controller) {
            *ui_current = arg;
            touched_current = true;
        }

        grandia_ap::LogInfo("Party: ITEM list rebuild via +0x%X arg=%p mode=4",
                            static_cast<unsigned>(kItemListRebuildRva), arg);
        using ListRebuildFn = void(__cdecl*)(void*);
        reinterpret_cast<ListRebuildFn>(base + kItemListRebuildRva)(arg);
        RefreshItemAssignWidgets(base);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        grandia_ap::LogWarn("Party: ITEM list rebuild faulted — cooling down 5s");
        g_item_rebuild_cooldown_until = GetTickCount() + 5000u;
    }

    // Always restore — a fault mid-call previously left mode stuck at 4.
    __try {
        if (mode_word) {
            *mode_word = saved_mode;
        }
        if (touched_current && ui_current) {
            *ui_current = saved_current;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

void ClearSlotExcludeBit(std::uintptr_t base, unsigned slot) {
    auto* bits = *reinterpret_cast<uint8_t**>(base + kEventBitsPtrRva);
    if (!bits || slot >= 4) {
        return;
    }
    const unsigned bit = kSlotExcludeBitBase + slot;
    const unsigned mask = 1u << (7u - (bit & 7u));
    bits[bit >> 3] = static_cast<uint8_t>(bits[bit >> 3] & ~mask);
}

// Empty unrecruited blocks (level==0) are skipped by battle → softlock / missing actors.
// Clone a living template (prefer Justin, else first non-empty party member).
int SeedMissingCharacterBlocks(uint8_t* map_obj) {
    if (!map_obj) {
        return 0;
    }
    const uint8_t* template_block = nullptr;
    auto* justin = CharBlock(map_obj, 1);
    if (justin && justin[3] != 0) {
        template_block = justin;
    }
    if (!template_block) {
        const uint8_t n = g_count.load();
        for (uint8_t i = 0; i < n; ++i) {
            auto* blk = CharBlock(map_obj, g_ids[i]);
            if (blk && blk[3] != 0) {
                template_block = blk;
                break;
            }
        }
    }

    int seeded = 0;
    const uint8_t n = g_count.load();
    for (uint8_t i = 0; i < n; ++i) {
        const uint8_t id = g_ids[i];
        auto* blk = CharBlock(map_obj, id);
        if (!blk) {
            continue;
        }
        if (blk[3] != 0) {
            continue;  // already initialized
        }
        if (template_block && template_block != blk) {
            std::memcpy(blk, template_block, static_cast<size_t>(kCharStride));
            // Don't inherit the template's equipped gear / bag (duplicates break the
            // merged party item list when custom members are equip targets).
            std::memset(blk + kCharEquipOff, 0,
                        (kCharEquipSlots + kCharInvSlots) * sizeof(uint16_t));
        } else {
            std::memset(blk, 0, static_cast<size_t>(kCharStride));
            blk[3] = 1;                                        // level
            *reinterpret_cast<uint16_t*>(blk + 0x0A) = 100;    // max HP
            *reinterpret_cast<uint16_t*>(blk + 0x0C) = 100;    // cur HP
            *reinterpret_cast<uint16_t*>(blk + 0x16) = 50;     // max SP
            *reinterpret_cast<uint16_t*>(blk + 0x18) = 50;     // cur SP
        }
        ++seeded;
        grandia_ap::LogInfo("Party: seeded char block id=%u (%s) level=%u hp=%u", id, CharName(id),
                            blk[3], *reinterpret_cast<uint16_t*>(blk + 0x0C));
    }
    return seeded;
}

void SnapshotFieldPartyIds(std::uintptr_t base) {
    uint8_t* map_obj = MapObject(base);
    if (!map_obj) {
        return;
    }
    __try {
        uint8_t cur[4]{};
        for (int i = 0; i < 4; ++i) {
            cur[i] = map_obj[0x0A + i];
        }
        // If MapObj already equals custom g_ids, a prior buggy write corrupted it.
        const uint8_t n = g_count.load();
        bool matches_custom = true;
        for (uint8_t i = 0; i < 4; ++i) {
            const uint8_t want = (i < n) ? g_ids[i] : 0;
            if (cur[i] != want) {
                matches_custom = false;
                break;
            }
        }
        if (matches_custom && n > 0) {
            grandia_ap::LogWarn(
                "Party: MapObj+0A already equals custom party %u,%u,%u,%u — reload save to "
                "restore field roster (70CF90 was MapObj)",
                cur[0], cur[1], cur[2], cur[3]);
            return;
        }
        for (int i = 0; i < 4; ++i) {
            g_saved_field0a[i] = cur[i];
        }
        g_field0a_saved_for_battle = true;
        grandia_ap::LogInfo("Party: snapshotted field MapObj+0A=%u,%u,%u,%u", g_saved_field0a[0],
                            g_saved_field0a[1], g_saved_field0a[2], g_saved_field0a[3]);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        g_field0a_saved_for_battle = false;
    }
}

void RestoreFieldPartyIdsIfNeeded(std::uintptr_t base) {
    if (!g_field0a_saved_for_battle) {
        return;
    }
    uint8_t* map_obj = MapObject(base);
    if (!map_obj) {
        return;
    }
    __try {
        bool dirty = false;
        for (int i = 0; i < 4; ++i) {
            if (map_obj[0x0A + i] != g_saved_field0a[i]) {
                dirty = true;
                break;
            }
        }
        if (!dirty) {
            return;
        }
        for (int i = 0; i < 4; ++i) {
            map_obj[0x0A + i] = g_saved_field0a[i];
        }
        grandia_ap::LogInfo(
            "Party: restored field MapObj+0A to %u,%u,%u,%u (was rewritten — 70CF90==MapObj)",
            g_saved_field0a[0], g_saved_field0a[1], g_saved_field0a[2], g_saved_field0a[3]);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        grandia_ap::LogWarn("Party: MapObj+0A restore faulted");
    }
}

void WriteUiPartyCache(std::uintptr_t base, std::uintptr_t cache_rva) {
    const uint8_t n = g_count.load();
    auto* cache = reinterpret_cast<uint8_t*>(base + cache_rva);
    std::memset(cache, 0, 4);
    uint8_t wrote = 0;
    for (uint8_t slot = 0; slot < n && wrote < 4; ++slot) {
        const uint8_t id = g_ids[slot];
        if (id < 1 || id > kMaxPlayableCharId) {
            continue;
        }
        cache[wrote++] = id;
    }
}

void RebuildMenuPartyCache(std::uintptr_t base, uint8_t* map_obj) {
    (void)map_obj;
    // All three UI caches — never touch MapObj, event bits, or field actors from here.
    WriteUiPartyCache(base, kMenuPartyCacheRva);
    WriteUiPartyCache(base, kStashPartyCacheRva);
    WriteUiPartyCache(base, kItemPartyCacheRva);
    auto* cache = reinterpret_cast<uint8_t*>(base + kMenuPartyCacheRva);
    grandia_ap::LogInfo(
        "Party: UI caches 70CFF8/707FD0/709C68 = %u,%u,%u,%u (field untouched)", cache[0],
        cache[1], cache[2], cache[3]);
}

bool IsBattleMode(uint8_t mode) { return mode == 2 || mode == 3; }

void LogBattlePartyState(std::uintptr_t base, const char* tag) {
    if constexpr (!kPartyBattleLogs) {
        (void)base;
        (void)tag;
        return;
    }
    uint8_t* actors = *reinterpret_cast<uint8_t**>(base + kActorArrayPtrRva);
    uint8_t* ctx = FieldContext(base);
    uint8_t* map_obj = MapObject(base);
    const uint8_t n = g_count.load();
    const uint16_t battle_count =
        *reinterpret_cast<uint16_t*>(base + kBattleCountRva);
    const int map614 = map_obj ? static_cast<int>(static_cast<int8_t>(map_obj[0x614])) : 0;
    const uint8_t cd38 =
        base ? *reinterpret_cast<uint8_t*>(base + kBattleSceneFlagRva) : 0u;
    const uint8_t a0_15 = actors ? actors[0x15] : 0u;
    const uint8_t a0_18 = actors ? actors[0x18] : 0u;
    const uint16_t a0_30 = actors ? *reinterpret_cast<uint16_t*>(actors + 0x30) : 0u;
    const uint8_t a1_15 = actors ? actors[kActorStride + 0x15] : 0u;
    const uint8_t a1_18 = actors ? actors[kActorStride + 0x18] : 0u;
    const uint16_t a1_30 =
        actors ? *reinterpret_cast<uint16_t*>(actors + static_cast<std::uintptr_t>(kActorStride) + 0x30)
               : 0u;
    const uint16_t a1_1c =
        actors ? *reinterpret_cast<uint16_t*>(actors + static_cast<std::uintptr_t>(kActorStride) + 0x1Cu)
               : 0u;
    const uint16_t a2_1c =
        actors ? *reinterpret_cast<uint16_t*>(actors + static_cast<std::uintptr_t>(kActorStride) * 2 + 0x1Cu)
               : 0u;
    const uint32_t a0_6c = actors ? *reinterpret_cast<uint32_t*>(actors + 0x6Cu) : 0u;
    const uint32_t a1_6c =
        actors && n > 1
            ? *reinterpret_cast<uint32_t*>(actors + static_cast<std::uintptr_t>(kActorStride) + 0x6Cu)
            : 0u;
    const uint32_t a2_6c =
        actors && n > 2
            ? *reinterpret_cast<uint32_t*>(actors +
                                           static_cast<std::uintptr_t>(kActorStride) * 2 + 0x6Cu)
            : 0u;
    const uint8_t a0_3 = actors ? actors[3] : 0u;
    const uint8_t a1_3 = actors ? actors[kActorStride + 3] : 0u;
    const uint8_t a2_3 = actors ? actors[kActorStride * 2 + 3] : 0u;
    const uint8_t cd40 = base ? *reinterpret_cast<uint8_t*>(base + 0x31CD40u) : 0u;
    const uint8_t ctx34 = ctx ? ctx[0x34] : 0u;
    const uint16_t pad464 =
        base ? *reinterpret_cast<uint16_t*>(base + 0x319464u) : 0u;
    const uint8_t gate0 = g_battle_ally_gate_site
                              ? reinterpret_cast<uint8_t*>(g_battle_ally_gate_site)[0]
                              : 0u;
    const uint8_t gate1 = g_battle_ally_gate_site
                              ? reinterpret_cast<uint8_t*>(g_battle_ally_gate_site)[1]
                              : 0u;
    const uint8_t a2_15 = actors ? actors[kActorStride * 2 + 0x15] : 0u;
    const uint8_t a2_18 = actors ? actors[kActorStride * 2 + 0x18] : 0u;
    const uint16_t a2_30 =
        actors ? *reinterpret_cast<uint16_t*>(actors + static_cast<std::uintptr_t>(kActorStride) * 2 + 0x30)
               : 0u;
    uint8_t map0a[4] = {};
    uint8_t snap9c[4] = {};
    if (map_obj) {
        for (int i = 0; i < 4; ++i) {
            map0a[i] = map_obj[0x0A + i];
            snap9c[i] = map_obj[kBattleSnapshotOff + i];
        }
    }
    uint8_t* snap_obj = nullptr;
    if (base) {
        snap_obj = *reinterpret_cast<uint8_t**>(base + kBattleSnapPtrRva);
        if (snap_obj) {
            for (int i = 0; i < 4; ++i) {
                snap9c[i] = snap_obj[kBattleSnapshotOff + i];
            }
        }
    }
    PartyBattleLog(
        "Party battle [%s]: want count=%u ids=%u(%s),%u(%s),%u(%s),%u | map0A=%u,%u,%u,%u "
        "snapBlit=%u,%u,%u,%u snapEqMap=%d | mode=%u 71BD08=%u cd38=%u cd40=%u "
        "ctx+0A=%u ctx+34=%u map614=%d pad464=%04X gate=%02X%02X "
        "a0=%u,%u,%u,%u a0a=%u a0+11=%u a0+15=%u a0+18=%u a0+30=%u a0+6c=%08X "
        "a1=%u,%u,%u,%u a1a=%u a1+11=%u a1+1c=%u a1+15=%u a1+18=%u a1+30=%u a1+6c=%08X "
        "a2=%u,%u,%u,%u a2a=%u a2+11=%u a2+1c=%u a2+15=%u a2+18=%u a2+30=%u a2+6c=%08X "
        "pres=%u,%u,%u",
        tag, n, g_ids[0], CharName(g_ids[0]), g_ids[1], CharName(g_ids[1]), g_ids[2],
        CharName(g_ids[2]), g_ids[3], map0a[0], map0a[1], map0a[2], map0a[3], snap9c[0],
        snap9c[1], snap9c[2], snap9c[3],
        (snap_obj == map_obj || snap_obj == nullptr) ? 1 : 0,
        base ? static_cast<unsigned>(*reinterpret_cast<uint8_t*>(base + kBattleModeRva)) : 0u,
        static_cast<unsigned>(battle_count),
        static_cast<unsigned>(cd38), static_cast<unsigned>(cd40),
        ctx ? static_cast<unsigned>(ctx[0x0A]) : 0u, static_cast<unsigned>(ctx34), map614,
        static_cast<unsigned>(pad464), static_cast<unsigned>(gate0), static_cast<unsigned>(gate1),
        actors ? actors[0] : 0u, actors ? actors[1] : 0u, actors ? actors[2] : 0u, a0_3,
        actors ? actors[0x0A] : 0u, actors ? actors[0x11] : 0u, a0_15, a0_18,
        static_cast<unsigned>(a0_30), a0_6c, actors ? actors[kActorStride] : 0u,
        actors ? actors[kActorStride + 1] : 0u, actors ? actors[kActorStride + 2] : 0u, a1_3,
        actors ? actors[kActorStride + 0x0A] : 0u, actors ? actors[kActorStride + 0x11] : 0u,
        static_cast<unsigned>(a1_1c), a1_15, a1_18, static_cast<unsigned>(a1_30), a1_6c,
        actors ? actors[kActorStride * 2] : 0u, actors ? actors[kActorStride * 2 + 1] : 0u,
        actors ? actors[kActorStride * 2 + 2] : 0u, a2_3,
        actors ? actors[kActorStride * 2 + 0x0A] : 0u,
        actors ? actors[kActorStride * 2 + 0x11] : 0u, static_cast<unsigned>(a2_1c), a2_15, a2_18,
        static_cast<unsigned>(a2_30), a2_6c,
        base ? *reinterpret_cast<uint8_t*>(base + kBattlePresenceRva + 0) : 0u,
        base ? *reinterpret_cast<uint8_t*>(base + kBattlePresenceRva + 1) : 0u,
        base ? *reinterpret_cast<uint8_t*>(base + kBattlePresenceRva + 2) : 0u);
}

bool CallBattleRosterApply(std::uintptr_t base) {
    using RosterFn = void(__cdecl*)();
    auto* roster = reinterpret_cast<RosterFn>(base + kRosterApplyRva);
    auto* formation_call = reinterpret_cast<uint8_t*>(base + kRosterFormationCallRva);
    if (formation_call[0] != 0xE8) {
        PartyBattleWarn("Party battle: roster formation call site mismatch at +0x%X",
                            static_cast<unsigned>(kRosterFormationCallRva));
        return false;
    }

    uint8_t saved[5]{};
    const uint8_t nops[5] = {0x90, 0x90, 0x90, 0x90, 0x90};
    if (!WriteBytes(formation_call, nops, 5, saved)) {
        return false;
    }

    bool ok = true;
    __try {
        roster();
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        PartyBattleWarn("Party battle: roster apply faulted");
        ok = false;
    }
    WriteBytes(formation_call, saved, 5, nullptr);
    return ok;
}

// Battle-only roster apply was removed: calling +7E660 during encounter transition
// destroys field companions and still does not own the real combatant list (719BC0).

void SyncBattlePresenceMap(std::uintptr_t base, uint8_t count) {
    auto* presence = reinterpret_cast<uint8_t*>(base + kBattlePresenceRva);
    // Stock +7E660 / +7EAA7: presence[dense_slot] = slot*5. Only touch [0..3] —
    // never write into 0x719950 (ATB anim ptr at presence+8).
    for (uint8_t slot = 0; slot < 4; ++slot) {
        presence[slot] = (slot < count) ? static_cast<uint8_t>(slot * 5u) : 0;
    }
    (void)base;
}

void PromoteBattleActorFlags(std::uintptr_t base, uint8_t count) {
    uint8_t* actors = *reinterpret_cast<uint8_t**>(base + kActorArrayPtrRva);
    if (!actors) {
        return;
    }
    // HUD (+864E0) skips actor[0]&0x0C. Do NOT clear bit 0x20 — +78EA0 / field
    // registration uses it. Follower tick (+806C0) needs actor+0x11 bit0.
    for (uint8_t slot = 0; slot < count; ++slot) {
        uint8_t* actor = actors + static_cast<std::uintptr_t>(slot) * kActorStride;
        actor[0] = static_cast<uint8_t>((actor[0] & static_cast<uint8_t>(~0x0Cu)) | 0x01u);
        actor[0x11] = static_cast<uint8_t>(actor[0x11] | 0x01u);
    }
}

#if defined(_M_IX86)
void CallBattleAllyRegister(std::uintptr_t fn, unsigned slot) {
    void* target = reinterpret_cast<void*>(fn);
    __asm {
        mov ecx, slot
        call dword ptr [target]
    }
}
#else
void CallBattleAllyRegister(std::uintptr_t fn, unsigned slot) {
    (void)fn;
    (void)slot;
}
#endif

bool BattlePartyFullyRegistered(std::uintptr_t base, uint8_t count) {
    uint8_t* actors = *reinterpret_cast<uint8_t**>(base + kActorArrayPtrRva);
    if (!actors) {
        return false;
    }
    for (uint8_t slot = 0; slot < count; ++slot) {
        const uint8_t id = g_ids[slot];
        if (id == 0) {
            continue;
        }
        uint8_t* actor = actors + static_cast<std::uintptr_t>(slot) * kActorStride;
        if (actor[1] == 0 || actor[2] != id) {
            return false;
        }
    }
    return count > 0;
}

void RestoreFieldActorsAfterBattle(std::uintptr_t base) {
    if (!g_field_actors_saved) {
        return;
    }
    uint8_t* actors = *reinterpret_cast<uint8_t**>(base + kActorArrayPtrRva);
    if (!actors) {
        return;
    }
    __try {
        std::memcpy(actors, g_saved_field_actors, sizeof(g_saved_field_actors));
        PartyBattleLog("Party battle: restored field actor array after fight");
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        PartyBattleWarn("Party battle: field actor restore faulted");
    }
    g_field_actors_saved = false;
}

// Random encounters keep 719BC0 empty. Real membership is the +12C4A7 ally spawn
// loop (formation table count + MapObj match) and +13BD81 char-id write — hooked below.
// Old MapObj/+7E660 roster swap stays disabled (field teleport, wrong party source).
constexpr bool kBattleRosterSwapEnabled = false;

void EnsureAllyBattleRegistration(std::uintptr_t base, uint8_t count) {
    if constexpr (!kBattleRosterSwapEnabled) {
        (void)base;
        (void)count;
        return;
    }
    if (g_battle_custom_spawn_done || g_battle_roster_applied) {
        return;
    }
    if (count == 0) {
        return;
    }

    uint8_t* map_obj = MapObject(base);
    uint8_t* actors = *reinterpret_cast<uint8_t**>(base + kActorArrayPtrRva);
    if (!map_obj || !actors) {
        return;
    }

    if (!g_field0a_saved_for_battle) {
        SnapshotFieldPartyIds(base);
    }
    if (!g_field0a_saved_for_battle) {
        PartyBattleWarn("Party battle: no field MapObj snapshot — refusing roster swap");
        g_battle_custom_spawn_done = true;
        return;
    }

    __try {
        if (!g_field_actors_saved) {
            std::memcpy(g_saved_field_actors, actors, sizeof(g_saved_field_actors));
            g_field_actors_saved = true;
        }

        for (uint8_t i = 0; i < 4u; ++i) {
            map_obj[0x0A + i] = (i < count) ? g_ids[i] : 0;
        }
        SeedMissingCharacterBlocks(map_obj);

        const bool ok = CallBattleRosterApply(base);
        g_battle_roster_applied = true;
        SyncBattlePartyActors(base, count);

        PartyBattleLog(
            "Party battle: roster swap %s — MapObj+0A now %u,%u,%u,%u (field snap %u,%u,%u,%u)",
            ok ? "ok" : "FAILED", map_obj[0x0A], map_obj[0x0B], map_obj[0x0C], map_obj[0x0D],
            g_saved_field0a[0], g_saved_field0a[1], g_saved_field0a[2], g_saved_field0a[3]);
        LogBattlePartyState(base, "after roster");

        if (BattlePartyFullyRegistered(base, count)) {
            g_battle_custom_spawn_done = true;
            g_battle_allies_registered = true;
            PartyBattleLog("Party battle: custom actors ready after roster");
        } else {
            // Still mark done so we do not re-roster every poll (destroys field repeatedly).
            g_battle_custom_spawn_done = true;
            PartyBattleWarn("Party battle: roster ran but actors not fully matched");
            LogBattlePartyState(base, "roster mismatch");
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        PartyBattleWarn("Party battle: roster swap faulted");
        g_battle_custom_spawn_done = true;
    }
}

void CloneLeaderBattlePose(std::uintptr_t base, uint8_t count) {
    uint8_t* actors = *reinterpret_cast<uint8_t**>(base + kActorArrayPtrRva);
    if (!actors || count < 2) {
        return;
    }
    // Random fights often leave ally +68/+6c/+70 at 0. Offset clones of the leader
    // so follower init / draw have a non-origin pose (same idea as +80559 path).
    const int32_t lx = *reinterpret_cast<int32_t*>(actors + 0x68);
    const int32_t ly = *reinterpret_cast<int32_t*>(actors + 0x6C);
    const int32_t lz = *reinterpret_cast<int32_t*>(actors + 0x70);
    if (lx == 0 && ly == 0 && lz == 0) {
        return;
    }
    for (uint8_t slot = 1; slot < count; ++slot) {
        uint8_t* actor = actors + static_cast<std::uintptr_t>(slot) * kActorStride;
        const int32_t ax = *reinterpret_cast<int32_t*>(actor + 0x68);
        const int32_t ay = *reinterpret_cast<int32_t*>(actor + 0x6C);
        const int32_t az = *reinterpret_cast<int32_t*>(actor + 0x70);
        if (ax != 0 || ay != 0 || az != 0) {
            continue;
        }
        const int32_t dx = static_cast<int32_t>(slot) * 0x18000;
        *reinterpret_cast<int32_t*>(actor + 0x68) = lx + dx;
        *reinterpret_cast<int32_t*>(actor + 0x6C) = ly;
        *reinterpret_cast<int32_t*>(actor + 0x70) = lz + dx;
        *reinterpret_cast<int32_t*>(actor + 0x74) = *reinterpret_cast<int32_t*>(actor + 0x68);
        *reinterpret_cast<int32_t*>(actor + 0x78) = ly;
        *reinterpret_cast<int32_t*>(actor + 0x7C) = *reinterpret_cast<int32_t*>(actor + 0x70);
    }
}

void SyncBattlePartyActors(std::uintptr_t base, uint8_t count) {
    if (!g_enabled.load() || base == 0 || count == 0) {
        return;
    }
    __try {
        SyncBattleFieldContext(base);
        for (uint8_t slot = 0; slot < count; ++slot) {
            ClearSlotExcludeBit(base, slot);
        }
        PatchBattleActorIds(base, count);
        SyncBattlePresenceMap(base, count);
        PromoteBattleActorFlags(base, count);
        CloneLeaderBattlePose(base, count);

        if constexpr (kBattleClampAlliesToLeader) {
            uint8_t* actors = *reinterpret_cast<uint8_t**>(base + kActorArrayPtrRva);
            if (actors) {
                for (uint8_t slot = 1; slot < 4; ++slot) {
                    actors[static_cast<std::uintptr_t>(slot) * kActorStride + 1] = 0;
                }
            }
            *reinterpret_cast<uint16_t*>(base + kBattleCountRva) = 1;
            PartyBattleLog(
                "Party battle: clamped to leader only (custom allies crash combat — investigating)");
        } else {
            *reinterpret_cast<uint16_t*>(base + kBattleCountRva) = count;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        const uint8_t mode = *reinterpret_cast<uint8_t*>(base + kBattleModeRva);
        PartyBattleWarn("Party battle: actor sync faulted (mode=%u)", static_cast<unsigned>(mode));
    }
}

void ApSyncBattlePartyCounts(std::uintptr_t base) {
    SyncBattlePartyActors(base, g_count.load());
}

void PollBattleMode(std::uintptr_t base) {
    const uint8_t mode = *reinterpret_cast<uint8_t*>(base + kBattleModeRva);
    const bool in_battle = IsBattleMode(mode);
    const bool was_battle = IsBattleMode(g_last_battle_mode);

    if (mode != g_last_battle_mode && g_battle_mode_diag_logs_left > 0) {
        PartyBattleLog("Party diag: mode %u -> %u", static_cast<unsigned>(g_last_battle_mode),
                            static_cast<unsigned>(mode));
    }

    // Arm battle code patches only while fighting — never on the field.
    // Do NOT PrepareBattlePartyAssets here: mode-enter is after setup has begun and
    // MapObj/face writes mid-transition crash before ally spawn (+12BDD0 is the right site).
    if (!in_battle) {
        SetBattleAllyGatePatch(base, false);
        SetBattleFa90EarlyExitPatch(base, false);
    } else if (g_enabled.load() && !was_battle) {
        SetBattleAllyGatePatch(base, true);
        SetBattleFa90EarlyExitPatch(base, true);
    }
    if (was_battle && !in_battle) {
        g_battle_allies_registered = false;
        g_battle_assets_prepared = false;
        g_battle_pack_rebuilt = false;
        g_battle_keyed_tasks_injected = false;
        g_ally_combatant_logged = 0;
        for (uint8_t i = 0; i < 4u; ++i) {
            g_ally_combatants[i] = 0;
        }
        FreeKeyedSlotBuffers();
        RestoreFieldPartyIdsIfNeeded(base);
        if (g_battle_roster_applied) {
            RestoreFieldActorsAfterBattle(base);
            g_battle_roster_applied = false;
        }
    }

    if ((g_last_battle_mode == 0 || g_last_battle_mode == 2) && mode == 3) {
        g_battle_party_init_synced = false;
        g_battle_allies_registered = false;
        g_battle_custom_spawn_done = false;
        g_battle_roster_applied = false;
        g_battle_pack_rebuilt = false;
        g_battle_keyed_tasks_injected = false;
        g_battle_unit_wait_logs_left = kPartyBattleLogs ? 8 : 0;
    }

    if (g_last_battle_mode == 0 && in_battle) {
        g_battle_party_init_synced = false;
        g_battle_allies_registered = false;
        g_battle_custom_spawn_done = false;
        g_battle_roster_applied = false;
        g_battle_keyed_tasks_injected = false;
        if (g_battle_unit_wait_logs_left == 0) {
            g_battle_unit_wait_logs_left = kPartyBattleLogs ? 8 : 0;
        }
    }

    // One-shot MapObj+roster swap for custom battle party (719BC0 unused in random fights).
    if (g_enabled.load() && mode == 3 && !g_battle_custom_spawn_done) {
        EnsureAllyBattleRegistration(base, g_count.load());
    }

    if (mode == 0 && g_last_battle_mode != 0) {
        g_battle_assets_prepared = false;
        g_battle_pack_rebuilt = false;
        g_battle_keyed_tasks_injected = false;
        g_ally_combatant_logged = 0;
        for (uint8_t i = 0; i < 4u; ++i) {
            g_ally_combatants[i] = 0;
        }
        FreeKeyedSlotBuffers();
        if (g_battle_roster_applied) {
            RestoreFieldPartyIdsIfNeeded(base);
            RestoreFieldActorsAfterBattle(base);
            g_battle_roster_applied = false;
        } else {
            RestoreFieldPartyIdsIfNeeded(base);
        }
        g_battle_custom_spawn_done = false;
        g_battle_party_init_synced = false;
        g_battle_allies_registered = false;
        g_battle_unit_wait_logs_left = 0;
    }

    g_last_battle_mode = mode;
}

void WriteMinimalBattleParty(std::uintptr_t base) {
    if (!g_enabled.load() || base == 0) {
        return;
    }

    const uint8_t n = g_count.load();
    if (uint8_t* map_obj = MapObject(base)) {
        SeedMissingCharacterBlocks(map_obj);
    }
    SyncBattlePartyActors(base, n);
}

void PatchFormationTableForBattle(std::uintptr_t base) {
    if (base == 0 || !g_enabled.load()) {
        return;
    }
    __try {
        auto* battle_ctx = *reinterpret_cast<uint8_t**>(base + kBattleCtxPtrRva);
        auto* table = *reinterpret_cast<uint8_t**>(base + kBattleFormTablePtrRva);
        if (!battle_ctx || !table) {
            return;
        }
        const uint8_t form = battle_ctx[0x243];
        const uint8_t n = g_count.load();
        table[form] = n;
        for (uint8_t slot = 1; slot <= 4u; ++slot) {
            const uint8_t id = (slot <= n) ? g_ids[slot - 1u] : 0;
            table[static_cast<uint32_t>(form) * 4u + slot + 0x10u] = id;
        }
        if (n == 3u && PtrReadable(battle_ctx + 0x7A200u, 4)) {
            const uint32_t form_base_off = *reinterpret_cast<uint32_t*>(battle_ctx + 0x7A200u);
            auto* dst = battle_ctx + 0x7A200u + form_base_off + static_cast<uint32_t>(form) * 8u;
            if (PtrReadable(dst, 8)) {
                uint16_t new0 = 0, new2 = 0, new6 = 0;
                const char* tag = nullptr;
                if (g_ids[0] == 1u && g_ids[1] == 2u && g_ids[2] == 4u) {
                    new0 = 408u;
                    new2 = 69u;
                    new6 = 9u;
                    tag = "native 124";
                }
                if (tag) {
                    const uint16_t old0 = *reinterpret_cast<uint16_t*>(dst + 0);
                    const uint16_t old2 = *reinterpret_cast<uint16_t*>(dst + 2);
                    const uint16_t old6 = *reinterpret_cast<uint16_t*>(dst + 6);
                    *reinterpret_cast<uint16_t*>(dst + 0) = new0;
                    *reinterpret_cast<uint16_t*>(dst + 2) = new2;
                    *reinterpret_cast<uint16_t*>(dst + 6) = new6;
                    PartyBattleLog(
                        "Party battle: form row patch (%s) form=%u {%u,%u,%u}->{%u,%u,%u}", tag, form,
                        old0, old2, old6, new0, new2, new6);
                }
            }
        }
        PartyBattleLog("Party battle: formation table form=%u count=%u ids=%u,%u,%u,%u", form, n,
                            g_ids[0], g_ids[1], g_ids[2], g_ids[3]);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        PartyBattleWarn("Party battle: formation table patch faulted");
    }
}

// Snapshot field party, seed char blocks, patch formation ids/count.
// Do NOT rewrite MapObj+0A here: ally models are packed from the field party
// (c8a00/c8a10/…), not MapObj — writing MapObj does not load Feena/Gadwin meshes
// and can disturb field state. Model slots are remapped onto packed field models.
void PrepareBattlePartyAssets(std::uintptr_t base, const char* tag) {
    if (!g_enabled.load() || base == 0) {
        return;
    }
    __try {
        if (!g_field0a_saved_for_battle) {
            SnapshotFieldPartyIds(base);
        }
        uint8_t* map_obj = MapObject(base);
        if (map_obj) {
            const uint8_t n = g_count.load();
            for (uint8_t i = 0; i < 4u; ++i) {
                map_obj[0x0A + i] = (i < n) ? g_ids[i] : 0;
            }
            const int seeded = SeedMissingCharacterBlocks(map_obj);
            PartyBattleLog("Party battle: staged MapObj+0A=%u,%u,%u,%u before pack (seeded=%d)",
                                map_obj[0x0A], map_obj[0x0B], map_obj[0x0C], map_obj[0x0D], seeded);
        }
        PatchFormationTableForBattle(base);
        // Let +12BDD0 pack against the staged custom MapObj roster, then restore on battle exit.
        g_battle_assets_prepared = false;
        PartyBattleLog(
            "Party battle: spawn prep (%s) formation patched; field snap=%u,%u,%u,%u "
            "battle map=%u,%u,%u,%u",
            tag ? tag : "?", g_saved_field0a[0], g_saved_field0a[1], g_saved_field0a[2],
            g_saved_field0a[3], map_obj ? map_obj[0x0A] : 0, map_obj ? map_obj[0x0B] : 0,
            map_obj ? map_obj[0x0C] : 0, map_obj ? map_obj[0x0D] : 0);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        PartyBattleWarn("Party battle: asset prepare faulted");
    }
}

bool TriggerPartyApply() {
    const std::uintptr_t base = grandia_ap::GetGrandiaModuleBase();
    if (base == 0) {
        return false;
    }
    RefreshAssetBinding();
    g_count_hook_logs_left = 0;
    g_char_hook_logs_left = 0;
    g_battle_ally_hook_logs_left = kPartyBattleLogs ? 16 : 0;
    g_battle_pdat_probe_logs_left = kPartyBattleLogs ? 48 : 0;

    uint8_t* map_obj = nullptr;
    uint8_t field0a[4] = {};
    __try {
        map_obj = MapObject(base);
        if (!map_obj) {
            return false;
        }
        for (int i = 0; i < 4; ++i) {
            field0a[i] = map_obj[0x0A + i];
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }

    // Field stays as-is: menu cache only. Char blocks / event bits are
    // applied at battle load (+12BDD0) so field movement is never disturbed.
    __try {
        RebuildMenuPartyCache(base, map_obj);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        grandia_ap::LogWarn("Party: menu apply (cache) faulted");
    }

    grandia_ap::LogInfo(
        "Custom party apply (menu only): field0A=%u,%u,%u,%u untouched | "
        "enabled=%d count=%u ids=%u,%u,%u,%u",
        field0a[0], field0a[1], field0a[2], field0a[3], g_enabled.load() ? 1 : 0, g_count.load(),
        g_ids[0], g_ids[1], g_ids[2], g_ids[3]);
    ToastParty("Applied (menu/battle):");
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

// Pre-baked P_DAT party archives (form-row w0/w2/w6). Each pack is a contiguous
// multi-character blob with a 0x10-byte header per character at the start.
// Terminator after the last char is usually hdr[count] with all-equal end offset;
// 4-char packs often lack that — exclusive end falls back to pack byte size.
struct PdatDonorPack {
    uint8_t count;
    uint8_t ids[4];
    uint16_t w0;
    uint16_t w2;
    uint16_t w6;
};

constexpr PdatDonorPack kPdatDonors[] = {
    {2, {1, 3, 0, 0}, 30, 38, 8},     // Justin+Sue
    {3, {1, 3, 2, 0}, 76, 54, 11},    // Justin+Sue+Feena
    {2, {1, 2, 0, 0}, 141, 42, 7},    // Justin+Feena
    {4, {1, 3, 2, 4}, 255, 64, 28},   // Justin+Sue+Feena+Gadwin (fp+MGDAT size)
    {3, {1, 2, 4, 0}, 408, 69, 9},    // Justin+Feena+Gadwin
    {3, {1, 2, 5, 0}, 486, 53, 10},   // Justin+Feena+Rapp
    {4, {1, 2, 5, 6}, 549, 73, 12},   // +Milda
    {4, {1, 2, 5, 7}, 664, 66, 12},   // +Guido
    {4, {1, 2, 5, 8}, 833, 71, 12},   // +Liete
};
constexpr int kPdatDonorCount = sizeof(kPdatDonors) / sizeof(kPdatDonors[0]);

bool ResolvePdatPath(char* out, size_t out_size) {
    const std::uintptr_t base = grandia_ap::GetGrandiaModuleBase();
    if (base == 0) {
        return false;
    }
    char exe_path[MAX_PATH]{};
    if (!GetModuleFileNameA(reinterpret_cast<HMODULE>(base), exe_path, MAX_PATH)) {
        return false;
    }
    const std::string exe_dir = Dirname(exe_path);
    const char* rels[] = {
        "\\content\\BATLE\\P_DAT.BIN",
        "\\content\\batle\\P_DAT.BIN",
        "\\content\\BATLE\\p_dat.bin",
    };
    for (const char* rel : rels) {
        if (std::snprintf(out, out_size, "%s%s", exe_dir.c_str(), rel) <= 0) {
            continue;
        }
        if (FileExists(out)) {
            return true;
        }
    }
    return false;
}

// Pre-sliced playable blobs (tools/build_pdat_charpack.py). Preferred over live P_DAT.
#pragma pack(push, 1)
struct Gpd1FileHeader {
    char magic[4];  // "GPD1"
    uint32_t version;
    uint32_t count;
    uint32_t reserved;
};
struct Gpd1Entry {
    uint8_t char_id;
    uint8_t variant;
    uint8_t flags;  // bit0 = preferred
    uint8_t pad;
    uint32_t hdr[4];  // blob-relative
    uint32_t blob_size;
    uint32_t blob_off;
};
#pragma pack(pop)
constexpr uint32_t kGpd1Version = 1u;
constexpr uint8_t kGpd1FlagPreferred = 1u;

bool ResolveCharpackPath(char* out, size_t out_size) {
    const std::string dll_dir = ModuleDirectory();
    if (!dll_dir.empty()) {
        if (std::snprintf(out, out_size, "%s\\pdat_charpack.bin", dll_dir.c_str()) > 0 &&
            FileExists(out)) {
            return true;
        }
    }
    const std::uintptr_t base = grandia_ap::GetGrandiaModuleBase();
    if (base != 0) {
        char exe_path[MAX_PATH]{};
        if (GetModuleFileNameA(reinterpret_cast<HMODULE>(base), exe_path, MAX_PATH)) {
            const std::string exe_dir = Dirname(exe_path);
            const char* rels[] = {
                "\\pdat_charpack.bin",
                "\\content\\BATLE\\pdat_charpack.bin",
                "\\content\\batle\\pdat_charpack.bin",
            };
            for (const char* rel : rels) {
                if (std::snprintf(out, out_size, "%s%s", exe_dir.c_str(), rel) > 0 &&
                    FileExists(out)) {
                    return true;
                }
            }
        }
    }
    return false;
}

// Prefer a donor where the char is not last (clean next-header end), then smaller packs.
const PdatDonorPack* FindPdatDonorForChar(uint8_t char_id, uint8_t* out_index) {
    const PdatDonorPack* best = nullptr;
    uint8_t best_index = 0;
    uint32_t best_score = 0xFFFFFFFFu;
    for (int i = 0; i < kPdatDonorCount; ++i) {
        const auto& d = kPdatDonors[i];
        for (uint8_t s = 0; s < d.count; ++s) {
            if (d.ids[s] != char_id) {
                continue;
            }
            const uint32_t pack_bytes =
                static_cast<uint32_t>(d.w2 + d.w6) * 0x800u;
            const bool is_last = (s + 1u) >= d.count;
            const uint32_t score = pack_bytes + (is_last ? 0x01000000u : 0u);
            if (score < best_score) {
                best_score = score;
                best = &d;
                best_index = s;
            }
        }
    }
    if (best && out_index) {
        *out_index = best_index;
    }
    return best;
}

const PdatDonorPack* FindPdatDonorByW0(uint16_t w0) {
    for (int i = 0; i < kPdatDonorCount; ++i) {
        if (kPdatDonors[i].w0 == w0) {
            return &kPdatDonors[i];
        }
    }
    return nullptr;
}

bool ReadFileAll(const char* path, std::vector<uint8_t>* out) {
    HANDLE h = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        return false;
    }
    LARGE_INTEGER li{};
    if (!GetFileSizeEx(h, &li) || li.QuadPart <= 0 || li.QuadPart > 0x4000000) {
        CloseHandle(h);
        return false;
    }
    out->resize(static_cast<size_t>(li.QuadPart));
    DWORD got = 0;
    const BOOL ok = ReadFile(h, out->data(), static_cast<DWORD>(out->size()), &got, nullptr);
    CloseHandle(h);
    return ok && got == out->size();
}

const std::vector<uint8_t>* GetCachedPdat() {
    static std::vector<uint8_t> cached;
    static int state = 0;  // 0=unset, 1=ok, -1=fail
    if (state == 0) {
        char pdat_path[MAX_PATH]{};
        if (!ResolvePdatPath(pdat_path, sizeof(pdat_path)) || !ReadFileAll(pdat_path, &cached)) {
            cached.clear();
            state = -1;
            return nullptr;
        }
        state = 1;
        PartyBattleLog("Party battle P_DAT: cached %u bytes from %s",
                            static_cast<unsigned>(cached.size()), pdat_path);
    }
    return state == 1 ? &cached : nullptr;
}

const std::vector<uint8_t>* GetCachedCharpack() {
    static std::vector<uint8_t> cached;
    static int state = 0;  // 0=unset, 1=ok, -1=fail
    if (state == 0) {
        char path[MAX_PATH]{};
        if (!ResolveCharpackPath(path, sizeof(path)) || !ReadFileAll(path, &cached)) {
            cached.clear();
            state = -1;
            return nullptr;
        }
        if (cached.size() < sizeof(Gpd1FileHeader)) {
            cached.clear();
            state = -1;
            return nullptr;
        }
        const auto* hdr = reinterpret_cast<const Gpd1FileHeader*>(cached.data());
        if (std::memcmp(hdr->magic, "GPD1", 4) != 0 || hdr->version != kGpd1Version) {
            PartyBattleWarn("Party battle charpack: bad header (need GPD1 v%u)", kGpd1Version);
            cached.clear();
            state = -1;
            return nullptr;
        }
        const uint32_t need =
            static_cast<uint32_t>(sizeof(Gpd1FileHeader) + hdr->count * sizeof(Gpd1Entry));
        if (cached.size() < need) {
            PartyBattleWarn("Party battle charpack: truncated index");
            cached.clear();
            state = -1;
            return nullptr;
        }
        state = 1;
        grandia_ap::LogInfo("Party battle: loaded charpack %s (%u entries, %u bytes)", path,
                            hdr->count, static_cast<unsigned>(cached.size()));
    }
    return state == 1 ? &cached : nullptr;
}

const Gpd1Entry* FindCharpackEntry(const std::vector<uint8_t>& pack, uint8_t char_id) {
    if (pack.size() < sizeof(Gpd1FileHeader)) {
        return nullptr;
    }
    const auto* hdr = reinterpret_cast<const Gpd1FileHeader*>(pack.data());
    const auto* ents = reinterpret_cast<const Gpd1Entry*>(pack.data() + sizeof(Gpd1FileHeader));
    const Gpd1Entry* fallback = nullptr;
    for (uint32_t i = 0; i < hdr->count; ++i) {
        if (ents[i].char_id != char_id) {
            continue;
        }
        if (static_cast<size_t>(ents[i].blob_off) + ents[i].blob_size > pack.size()) {
            continue;
        }
        if ((ents[i].flags & kGpd1FlagPreferred) != 0 || ents[i].variant == 0) {
            return &ents[i];
        }
        if (!fallback) {
            fallback = &ents[i];
        }
    }
    return fallback;
}

// Extend span past hdr[3] anim index table when entries reference bytes beyond hdr max.
// Entries >= 0x8000 are bank/sentinel flags (e.g. 0xFFFF), not byte offsets — treating
// them as offsets used to inflate mid-char spans to the entire donor pack.
uint32_t PdatAnimTableReach(const uint8_t* pack, uint32_t pack_size, const uint32_t* hdr) {
    if (!pack || !hdr || hdr[3] == 0 || hdr[3] + 0x40u > pack_size) {
        return 0;
    }
    const uint32_t table_off = hdr[3];
    uint32_t reach = table_off + 0x100u;
    for (unsigned i = 0; i < 64u; ++i) {
        const uint32_t ent_off = table_off + i * 4u;
        if (ent_off + 4u > pack_size) {
            break;
        }
        const uint32_t entry = *reinterpret_cast<const uint32_t*>(pack + ent_off);
        if (entry != 0 && entry < 0x8000u) {
            const uint32_t end = table_off + entry + 0x200u;
            if (end > reach) {
                reach = end;
            }
        }
    }
    return reach > pack_size ? pack_size : reach;
}

bool ExtractPdatCharSpan(const uint8_t* pack, uint32_t pack_size, uint8_t index,
                         uint8_t donor_char_count, uint32_t* out_start, uint32_t* out_end,
                         const uint32_t** out_hdr) {
    if (!pack || !out_start || !out_end || index >= 4u || donor_char_count == 0 ||
        index >= donor_char_count) {
        return false;
    }
    const uint32_t hdr_off = static_cast<uint32_t>(index) * 0x10u;
    if (hdr_off + 0x10u > pack_size) {
        return false;
    }
    const auto* hdr = reinterpret_cast<const uint32_t*>(pack + hdr_off);
    uint32_t data_start = hdr[0];
    uint32_t data_end = hdr[0];
    for (int i = 1; i < 4; ++i) {
        if (hdr[i] < data_start) {
            data_start = hdr[i];
        }
        if (hdr[i] > data_end) {
            data_end = hdr[i];
        }
    }
    // Primary exclusive end: next character's mesh start, or pack size for last char.
    // Stock packs overlap: anim-table entries (rel < 0x8000 from hdr[3]) often point
    // past the next character's mesh start. Measured max spill ~0x8118 (Rapp→Milda);
    // 0x4000 truncated mid-pack donors (Sue/Feena/Justin). Cap at 0x9000.
    constexpr uint32_t kAnimSpillCap = 0x9000u;
    uint32_t boundary = pack_size;
    const uint32_t next_off = (static_cast<uint32_t>(index) + 1u) * 0x10u;
    if (index + 1u < donor_char_count && next_off + 0x10u <= pack_size) {
        const auto* next_hdr = reinterpret_cast<const uint32_t*>(pack + next_off);
        if (next_hdr[0] > data_start && next_hdr[0] <= pack_size) {
            boundary = next_hdr[0];
        }
    } else if (index + 1u < donor_char_count) {
        // Fall through with pack_size if header row missing.
    } else if (next_off + 0x10u <= pack_size) {
        const auto* next_hdr = reinterpret_cast<const uint32_t*>(pack + next_off);
        // 3-char packs: all-equal terminator row. 4-char: bytes at +0x40 are mesh.
        if (next_hdr[0] == next_hdr[1] && next_hdr[1] == next_hdr[2] &&
            next_hdr[2] == next_hdr[3] && next_hdr[0] >= data_end && next_hdr[0] <= pack_size) {
            boundary = next_hdr[0];
        }
    }
    if (boundary > data_end) {
        data_end = boundary;
    }
    const uint32_t anim_reach = PdatAnimTableReach(pack, pack_size, hdr);
    if (anim_reach > data_end) {
        const uint32_t capped = boundary + kAnimSpillCap;
        data_end = anim_reach < capped ? anim_reach : capped;
        if (data_end > pack_size) {
            data_end = pack_size;
        }
    }
    if (data_end <= data_start || data_end > pack_size) {
        return false;
    }
    *out_start = data_start;
    *out_end = data_end;
    if (out_hdr) {
        *out_hdr = hdr;
    }
    return true;
}

void FreePdatSlotAllocs() {
    for (int i = 0; i < 4; ++i) {
        if (g_pdat_slot_alloc[i]) {
            VirtualFree(g_pdat_slot_alloc[i], 0, MEM_RELEASE);
            g_pdat_slot_alloc[i] = nullptr;
            g_pdat_slot_alloc_size[i] = 0;
        }
    }
    g_pdat_alloc_is_contiguous = false;
}

// Assemble stock-layout pack from charpack into one external buffer, then point
// c8a headers at it (offsets from c8a). Preserves inter-char anim spill.
bool TryInstallContiguousCharpack(std::uintptr_t base, uint8_t* c8a, uint32_t c8a_abs,
                                  uint8_t n, const char* tag) {
    const std::vector<uint8_t>* charpack = GetCachedCharpack();
    if (!charpack || n == 0 || n > 4) {
        return false;
    }

    const Gpd1Entry* ents[4]{};
    for (uint8_t slot = 0; slot < n; ++slot) {
        const uint8_t char_id = g_ids[slot];
        if (char_id < 1u || char_id > kMaxPlayableCharId) {
            return false;
        }
        ents[slot] = FindCharpackEntry(*charpack, char_id);
        if (!ents[slot]) {
            return false;
        }
    }

    // header rows + optional terminator
    const uint32_t header_bytes = static_cast<uint32_t>(n + (n < 4u ? 1u : 0u)) * 0x10u;
    uint32_t body = 0;
    for (uint8_t slot = 0; slot < n; ++slot) {
        body += (ents[slot]->blob_size + 3u) & ~3u;
    }
    const uint32_t pack_size = (header_bytes + body + 0xFFu) & ~0xFFu;
    void* mem = VirtualAlloc(nullptr, pack_size, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!mem) {
        PartyBattleWarn("Party battle P_DAT contiguous: VirtualAlloc failed size=%u", pack_size);
        return false;
    }
    std::memset(mem, 0, pack_size);
    auto* pack = static_cast<uint8_t*>(mem);
    uint32_t cursor = header_bytes;

    for (uint8_t slot = 0; slot < n; ++slot) {
        const Gpd1Entry* ent = ents[slot];
        auto* ph = reinterpret_cast<uint32_t*>(pack + static_cast<uint32_t>(slot) * 0x10u);
        for (int i = 0; i < 4; ++i) {
            ph[i] = cursor + ent->hdr[i];
        }
        std::memcpy(pack + cursor, charpack->data() + ent->blob_off, ent->blob_size);
        cursor += (ent->blob_size + 3u) & ~3u;
    }
    if (n < 4u) {
        auto* term = reinterpret_cast<uint32_t*>(pack + static_cast<uint32_t>(n) * 0x10u);
        term[0] = term[1] = term[2] = term[3] = cursor;
    }

    FreePdatSlotAllocs();
    g_pdat_slot_alloc[0] = mem;
    g_pdat_slot_alloc_size[0] = pack_size;
    g_pdat_alloc_is_contiguous = true;

    const uint32_t base_rel =
        static_cast<uint32_t>(reinterpret_cast<uintptr_t>(mem)) - c8a_abs;
    std::memset(c8a, 0, 0x40u);
    for (uint8_t slot = 0; slot < n; ++slot) {
        const auto* ph = reinterpret_cast<const uint32_t*>(pack + static_cast<uint32_t>(slot) * 0x10u);
        auto* dst = reinterpret_cast<uint32_t*>(c8a + static_cast<uint32_t>(slot) * 0x10u);
        for (int i = 0; i < 4; ++i) {
            dst[i] = base_rel + ph[i];
        }
        g_party_slot_to_c8a[slot] = slot;
        PartyBattleLog(
            "Party battle P_DAT contiguous [%s]: slot=%u char=%u (%s) pack_hdr=%u,%u,%u,%u "
            "c8a=%u,%u,%u,%u",
            tag ? tag : "?", slot, g_ids[slot], CharName(g_ids[slot]), ph[0], ph[1], ph[2], ph[3],
            dst[0], dst[1], dst[2], dst[3]);
    }
    if (n < 4u) {
        auto* term = reinterpret_cast<uint32_t*>(c8a + static_cast<uint32_t>(n) * 0x10u);
        const uint32_t end_rel = base_rel + cursor;
        term[0] = term[1] = term[2] = term[3] = end_rel;
    }
    for (uint8_t i = n; i < 4u; ++i) {
        g_party_slot_to_c8a[i] = i;
    }

    g_battle_pack_rebuilt = true;
    PartyBattleLog(
        "Party battle: contiguous P_DAT pack [%s] n=%u bytes=%u ext=%p base_rel=%u "
        "(form id still ctx+0x243 from setup+3)",
        tag ? tag : "?", static_cast<unsigned>(n), pack_size, mem, base_rel);
    LogPackedAllyModelSlots(base, tag ? tag : "post-pdat-contiguous");
    return true;
}

// Build playable headers at c8a00/10/20/30 for the custom roster. Prefer one
// contiguous external pack (stock layout). Fallback: per-char VirtualAlloc slices.
void TrySplicePdatPlayablesImpl(std::uintptr_t base, const char* tag) {
    if (!g_enabled.load() || g_battle_pack_rebuilt || base == 0) {
        return;
    }
    auto* ctx = *reinterpret_cast<uint8_t**>(base + kBattleCtxPtrRva);
    const uint8_t n = g_count.load();
    if (!ctx || n == 0 || n > 4 || !PtrReadable(ctx + 0xC8A00u, 0x40u)) {
        return;
    }

    uint8_t* c8a = ctx + 0xC8A00u;
    const uint32_t c8a_abs =
        static_cast<uint32_t>(reinterpret_cast<uintptr_t>(c8a));

    if (TryInstallContiguousCharpack(base, c8a, c8a_abs, n, tag)) {
        return;
    }

    const std::vector<uint8_t>* charpack = GetCachedCharpack();
    const std::vector<uint8_t>* pdat = GetCachedPdat();
    if (!charpack && !pdat) {
        PartyBattleWarn("Party battle P_DAT rebuild: no charpack and P_DAT.BIN missing");
        return;
    }

    for (uint8_t i = 0; i < 4u; ++i) {
        g_party_slot_to_c8a[i] = i;
    }

    FreePdatSlotAllocs();

    g_battle_pack_rebuilt = true;
    std::memset(c8a, 0, 0x40u);
    int built = 0;
    uint32_t total_bytes = 0x40u;

    for (uint8_t slot = 0; slot < n; ++slot) {
        const uint8_t char_id = g_ids[slot];
        if (char_id < 1u || char_id > kMaxPlayableCharId) {
            continue;
        }

        const uint8_t* src = nullptr;
        uint32_t span = 0;
        uint32_t hdr_rel[4]{};
        const char* src_tag = nullptr;

        if (charpack) {
            const Gpd1Entry* ent = FindCharpackEntry(*charpack, char_id);
            if (ent) {
                src = charpack->data() + ent->blob_off;
                span = ent->blob_size;
                for (int i = 0; i < 4; ++i) {
                    hdr_rel[i] = ent->hdr[i];
                }
                src_tag = "charpack";
            }
        }
        if (!src && pdat) {
            uint8_t donor_index = 0;
            const PdatDonorPack* donor = FindPdatDonorForChar(char_id, &donor_index);
            if (!donor) {
                PartyBattleWarn("Party battle P_DAT rebuild: no donor for char=%u (%s)", char_id,
                                CharName(char_id));
                continue;
            }
            const uint32_t pack_off = static_cast<uint32_t>(donor->w0) * 0x800u;
            const uint32_t pack_size =
                static_cast<uint32_t>(donor->w2 + donor->w6) * 0x800u;
            if (pack_off + pack_size > pdat->size()) {
                PartyBattleWarn("Party battle P_DAT rebuild: donor pack OOB char=%u", char_id);
                continue;
            }
            const uint8_t* pack = pdat->data() + pack_off;
            uint32_t data_start = 0;
            uint32_t data_end = 0;
            const uint32_t* hdr = nullptr;
            if (!ExtractPdatCharSpan(pack, pack_size, donor_index, donor->count, &data_start,
                                     &data_end, &hdr)) {
                PartyBattleWarn("Party battle P_DAT rebuild: bad span char=%u", char_id);
                continue;
            }
            src = pack + data_start;
            span = data_end - data_start;
            for (int i = 0; i < 4; ++i) {
                hdr_rel[i] = hdr[i] - data_start;
            }
            src_tag = "P_DAT";
        }
        if (!src || span == 0) {
            PartyBattleWarn("Party battle P_DAT rebuild: no blob for char=%u (%s)", char_id,
                            CharName(char_id));
            continue;
        }

        const uint32_t alloc_size = (span + 0xFu) & ~0xFu;
        void* mem = VirtualAlloc(nullptr, alloc_size, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        if (!mem) {
            PartyBattleWarn("Party battle P_DAT rebuild: VirtualAlloc failed slot=%u size=%u",
                                slot, alloc_size);
            continue;
        }
        std::memset(mem, 0, alloc_size);
        std::memcpy(mem, src, span);
        g_pdat_slot_alloc[slot] = mem;
        g_pdat_slot_alloc_size[slot] = alloc_size;

        const uint32_t base_rel =
            static_cast<uint32_t>(reinterpret_cast<uintptr_t>(mem)) - c8a_abs;
        auto* dst = reinterpret_cast<uint32_t*>(c8a + static_cast<uint32_t>(slot) * 0x10u);
        for (int i = 0; i < 4; ++i) {
            dst[i] = base_rel + hdr_rel[i];
        }
        PartyBattleLog(
            "Party battle P_DAT rebuild [%s]: slot=%u char=%u (%s) via=%s "
            "span=%u ext=%p base_rel=%u hdr=%u,%u,%u,%u",
            tag ? tag : "?", slot, char_id, CharName(char_id), src_tag ? src_tag : "?", span, mem,
            base_rel, dst[0], dst[1], dst[2], dst[3]);
        total_bytes += alloc_size;
        ++built;
    }
    if (n < 4u) {
        auto* term = reinterpret_cast<uint32_t*>(c8a + static_cast<uint32_t>(n) * 0x10u);
        term[0] = term[1] = term[2] = term[3] = 0x40u;
    }
    PartyBattleLog(
        "Party battle P_DAT rebuild [%s]: external built=%d/%u bytes=%u (per-slot fallback)",
        tag ? tag : "?", built, static_cast<unsigned>(n), total_bytes);
    LogPackedAllyModelSlots(base, tag ? tag : "post-pdat-rebuild");
}

}  // namespace

// Bridge for extern "C" callers.
void TrySplicePdatPlayables(std::uintptr_t base, const char* tag) {
    TrySplicePdatPlayablesImpl(base, tag);
}

#if defined(_M_IX86)
extern "C" {

unsigned ApPartyResolveCount(unsigned group_index) {
    // Only override during battle — field roster/map loads must keep stock companions.
    if (g_enabled.load()) {
        const std::uintptr_t base = grandia_ap::GetGrandiaModuleBase();
        if (base != 0) {
            const uint8_t mode = *reinterpret_cast<uint8_t*>(base + kBattleModeRva);
            if (IsBattleMode(mode)) {
                const unsigned count = g_count.load();
                if (g_count_hook_logs_left > 0) {
                    --g_count_hook_logs_left;
                    PartyBattleLog("Party hook: resolve count group=%u -> %u (battle)",
                                        group_index, count);
                }
                return count;
            }
        }
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
    const std::uintptr_t base = grandia_ap::GetGrandiaModuleBase();
    if (base == 0) {
        return 0xFFu;
    }
    const uint8_t mode = *reinterpret_cast<uint8_t*>(base + kBattleModeRva);
    if (!IsBattleMode(mode)) {
        return 0xFFu;  // use stock preset table on field
    }
    if (slot >= g_count.load() || slot >= 4u) {
        if (g_char_hook_logs_left > 0) {
            --g_char_hook_logs_left;
            PartyBattleLog("Party hook: resolve char slot=%u -> 0 (battle)", slot);
        }
        return 0;
    }
    const unsigned id = g_ids[slot];
    if (g_char_hook_logs_left > 0) {
        --g_char_hook_logs_left;
        PartyBattleLog("Party hook: resolve char slot=%u -> %u (battle)", slot, id);
    }
    return id;
}

unsigned ApBattleAllyResolveCount(unsigned formation_index) {
    if (g_enabled.load()) {
        const std::uintptr_t base = grandia_ap::GetGrandiaModuleBase();
        if (base != 0 && !g_field0a_saved_for_battle) {
            SnapshotFieldPartyIds(base);
        }
        const unsigned count = g_count.load();
        if (g_battle_ally_hook_logs_left > 0) {
            --g_battle_ally_hook_logs_left;
            PartyBattleLog("Party battle spawn: count form=%u -> %u", formation_index, count);
        }
        return count;
    }
    const std::uintptr_t base = grandia_ap::GetGrandiaModuleBase();
    if (base == 0) {
        return 0;
    }
    auto* table = *reinterpret_cast<uint8_t**>(base + kBattleFormTablePtrRva);
    if (!table) {
        return 0;
    }
    return table[formation_index & 0xFFu];
}

int ApBattleAllyShouldForceSpawn() {
    if (!g_enabled.load()) {
        return 0;
    }
    if (g_battle_ally_hook_logs_left > 0) {
        --g_battle_ally_hook_logs_left;
        PartyBattleLog("Party battle spawn: force-spawn (bypass MapObj/formation match)");
    }
    return 1;
}

unsigned ApBattleAllyResolveCharId(unsigned slot0, unsigned table_index, unsigned table_base) {
    if (g_enabled.load() && slot0 < g_count.load() && slot0 < 4u) {
        const unsigned id = g_ids[slot0];
        if (g_battle_ally_hook_logs_left > 0) {
            --g_battle_ally_hook_logs_left;
            PartyBattleLog("Party battle spawn: char slot0=%u -> %u (%s)", slot0, id,
                                CharName(static_cast<uint8_t>(id)));
        }
        return id;
    }
    if (table_base == 0) {
        return 0;
    }
    return *reinterpret_cast<uint8_t*>(table_base + table_index);
}

unsigned ApBattleAllyResolveModelSlot(unsigned spawn_slot0, unsigned char_id) {
    const std::uintptr_t base = grandia_ap::GetGrandiaModuleBase();
    if (base != 0) {
        TrySplicePdatPlayables(base, "ally-spawn");
    }
    // Contiguous pack is built in party order — stock JT must use identity slots
    // so c8a00/10/20 match spawn slots and +0x164 is filled from those headers.
    (void)char_id;
    const unsigned visible = (spawn_slot0 < 4u) ? spawn_slot0 : 0u;
    if (g_battle_ally_hook_logs_left > 0) {
        --g_battle_ally_hook_logs_left;
        PartyBattleLog("Party battle spawn: model slot char=%u party=%u -> c8a=%u", char_id,
                            spawn_slot0, visible);
    }
    return visible;
}

#if defined(_M_IX86)
void CallBattleModelBind12D730(void* mesh, uint8_t bank) {
    const std::uintptr_t base = grandia_ap::GetGrandiaModuleBase();
    if (base == 0 || !mesh) {
        return;
    }
    void* fn = g_ap_battle_model_bind_tramp;
    if (!fn) {
        fn = reinterpret_cast<void*>(base + kBattleModelBindRva);
    }
    __asm {
        mov edx, mesh
        mov cl, bank
        push 0
        push 0
        call dword ptr [fn]
        add esp, 8
    }
}

#else
void CallBattleModelBind12D730(void* mesh, uint8_t bank) {
    (void)mesh;
    (void)bank;
}
#endif

// Recompute combatant anim/model absolute pointers from the contiguous c8a pack
// headers (same math as ally JT at +13BD97..). c8a_slot may be 0..3 from JT edx,
// or 0xFF to derive from combatant+0x15A.
bool ApBattleEnsureAllyAnimPointers(unsigned combatant, unsigned c8a_slot) {
    if (!g_enabled.load() || combatant == 0 || !g_battle_pack_rebuilt) {
        return false;
    }
    const std::uintptr_t base = grandia_ap::GetGrandiaModuleBase();
    if (base == 0) {
        return false;
    }
    auto* ctx = *reinterpret_cast<uint8_t**>(base + kBattleCtxPtrRva);
    auto* actor = reinterpret_cast<uint8_t*>(combatant);
    if (!ctx || !PtrReadable(actor, 0x170u) || !PtrReadable(ctx + 0xC8A00u, 0x40u)) {
        return false;
    }
    if (actor[0x02] != 6u) {
        return false;
    }
    uint8_t slot = 0xFFu;
    if (c8a_slot < 4u) {
        slot = static_cast<uint8_t>(c8a_slot);
    } else {
        const uint8_t slot1 = actor[0x15A];
        if (slot1 >= 1u && slot1 <= 4u) {
            slot = static_cast<uint8_t>(slot1 - 1u);
        }
    }
    if (slot >= 4u) {
        return false;
    }
    auto* hdr = reinterpret_cast<uint32_t*>(ctx + 0xC8A00u + static_cast<uint32_t>(slot) * 0x10u);
    if (hdr[0] == 0 || hdr[3] == 0) {
        PartyBattleWarn("Party battle anim fixup: empty hdr slot=%u char=%u", slot,
                            actor[0x10F]);
        return false;
    }
    const uint32_t c8a_abs =
        static_cast<uint32_t>(reinterpret_cast<uintptr_t>(ctx)) + 0xC8A00u;
    const uint32_t p98 = c8a_abs + hdr[1];
    const uint32_t p9c = c8a_abs + hdr[2];
    const uint32_t p164 = c8a_abs + hdr[3];
    if (!PtrReadable(reinterpret_cast<void*>(p164), 8)) {
        PartyBattleWarn("Party battle anim fixup: bad +0x164=%08X slot=%u hdr3=%u", p164, slot,
                            hdr[3]);
        return false;
    }
    const uint32_t cur98 = *reinterpret_cast<uint32_t*>(actor + 0x98);
    const uint32_t cur9c = *reinterpret_cast<uint32_t*>(actor + 0x9C);
    const uint32_t cur164 = *reinterpret_cast<uint32_t*>(actor + 0x164);
    const bool changed = (cur98 != p98) || (cur9c != p9c) || (cur164 != p164);
    if (changed) {
        *reinterpret_cast<uint32_t*>(actor + 0x98) = p98;
        *reinterpret_cast<uint32_t*>(actor + 0x9C) = p9c;
        *reinterpret_cast<uint32_t*>(actor + 0x164) = p164;
    }
    const uint8_t bank = actor[0x92];
    void* mesh = ctx + 0xC8A00u + hdr[0];
    if (bank > 0 && bank <= 0x40u && PtrReadable(mesh, 8) &&
        (changed || !PtrReadable(reinterpret_cast<void*>(cur164), 8))) {
        CallBattleModelBind12D730(mesh, bank);
    }
    if (changed) {
        PartyBattleLog(
            "Party battle anim fixup: slot=%u char=%u (%s) +98=%08X +9c=%08X +164=%08X bank=%u "
            "hdr0=%u",
            slot, actor[0x10F], CharName(actor[0x10F]), p98, p9c, p164, bank, hdr[0]);
    }
    return true;
}

void ApBattleFixupAllyAnimTables(unsigned combatant, unsigned c8a_slot) {
    ApBattleEnsureAllyAnimPointers(combatant, c8a_slot);
}

void ApBattleEnsureAllyAnimBeforeWalk(unsigned combatant) {
    ApBattleEnsureAllyAnimPointers(combatant, 0xFFu);
}

unsigned ApBattleResolvePlayablePackProbe(unsigned combatant, unsigned battle_ctx, unsigned selector) {
    auto* ctx = reinterpret_cast<uint8_t*>(battle_ctx);
    if (!ctx) {
        return 0;
    }
    auto* actor = reinterpret_cast<uint8_t*>(combatant);
    const uint8_t type = actor ? actor[0x02] : 0xFFu;
    const uint8_t char_id = actor ? actor[0x10F] : 0xFFu;
    const uint8_t slot = actor ? actor[0x15A] : 0xFFu;
    const uint8_t actor24 = actor ? actor[0x24] : 0xFFu;
    const uint8_t actor25 = actor ? actor[0x25] : 0xFFu;
    const uint8_t actor26 = actor ? actor[0x26] : 0xFFu;
    const unsigned stock_key = ctx[0x64A07u + selector];
    unsigned use_selector = selector;
    unsigned resolved_key = stock_key;
    // Pass stock key through. Per-slot keyed inject owns follower packing now;
    // remapping the single stock task into e6a0[0] only overwrote that buffer.
    if (g_enabled.load() && g_battle_pdat_probe_logs_left > 0) {
        --g_battle_pdat_probe_logs_left;
        PartyBattleLog(
            "Party battle P_DAT probe: type=%u slot=%u char=%u (%s) sel=%u use=%u -> key=%u "
            "ctx24=%u ctx25=%u ctx26=%u actor24=%u actor25=%u actor26=%u",
            type, slot, char_id, CharName(char_id), selector, use_selector, resolved_key, ctx[0x243], ctx[0x244],
            ctx[0x245], actor24, actor25, actor26);
    }
    return resolved_key;
}

void ApBattleLogKeyedTaskEnqueue(unsigned owner, unsigned task, unsigned free_slot,
                                 unsigned task_slot, unsigned selector) {
    if (!g_enabled.load() || g_battle_pdat_probe_logs_left <= 0) {
        return;
    }
    --g_battle_pdat_probe_logs_left;
    auto* task_ptr = reinterpret_cast<uint8_t*>(task);
    const unsigned type = (task_ptr && PtrReadable(task_ptr, 0x160u)) ? task_ptr[0x02] : 0xFFu;
    const unsigned a24 = (task_ptr && PtrReadable(task_ptr, 0x160u)) ? task_ptr[0x24] : 0xFFu;
    const unsigned a25 = (task_ptr && PtrReadable(task_ptr, 0x160u)) ? task_ptr[0x25] : 0xFFu;
    const unsigned a26 = (task_ptr && PtrReadable(task_ptr, 0x160u)) ? task_ptr[0x26] : 0xFFu;
    const unsigned a94 = (task_ptr && PtrReadable(task_ptr, 0x160u)) ? task_ptr[0x94] : 0xFFu;
    const unsigned a10f = (task_ptr && PtrReadable(task_ptr, 0x160u)) ? task_ptr[0x10F] : 0xFFu;
    const unsigned a15a = (task_ptr && PtrReadable(task_ptr, 0x160u)) ? task_ptr[0x15A] : 0xFFu;
    PartyBattleLog(
        "Party battle keyed enqueue: owner=%08X task=%08X free=%u slot=%u sel=%u "
        "type=%u a24=%u a25=%u a26=%u a94=%u a10f=%u a15a=%u",
        owner, task, free_slot, task_slot, selector, type, a24, a25, a26, a94, a10f, a15a);
}

#if defined(_M_IX86)
int FindFreeSchedulerSlot(std::uintptr_t base) {
    auto* table = reinterpret_cast<uint8_t*>(base + kSchedulerSlotTableRva);
    if (!PtrReadable(table, 0xF7u * 8u)) {
        return -1;
    }
    for (unsigned slot = 0x80u; slot < 0xF7u; ++slot) {
        if (table[slot * 8u] == 0) {
            return static_cast<int>(slot);
        }
    }
    return -1;
}

unsigned CallSchedulerEnqueueKeyed(void* owner, uint8_t free_slot) {
    void* sched = g_battle_scheduler_enqueue;
    void* cb = g_battle_keyed_task_callback;
    unsigned result = 0;
    __asm {
        push owner
        mov dl, free_slot
        mov cl, 2
        push dword ptr [cb]
        call dword ptr [sched]
        add esp, 8
        mov result, eax
    }
    return result;
}
#else
int FindFreeSchedulerSlot(std::uintptr_t base) {
    (void)base;
    return -1;
}

unsigned CallSchedulerEnqueueKeyed(void* owner, uint8_t free_slot) {
    (void)owner;
    (void)free_slot;
    return 0;
}
#endif

void FreeKeyedSlotBuffers() {
    for (int i = 0; i < 4; ++i) {
        if (g_e6a0_alloc[i]) {
            VirtualFree(g_e6a0_alloc[i], 0, MEM_RELEASE);
            g_e6a0_alloc[i] = nullptr;
            g_e6a0_alloc_size[i] = 0;
        }
        if (g_pdat_slot_alloc[i]) {
            VirtualFree(g_pdat_slot_alloc[i], 0, MEM_RELEASE);
            g_pdat_slot_alloc[i] = nullptr;
            g_pdat_slot_alloc_size[i] = 0;
        }
    }
    g_pdat_alloc_is_contiguous = false;
}

bool EnsureKeyedSlotBuffers(std::uintptr_t base, uint8_t count) {
    auto* ctx = *reinterpret_cast<uint8_t**>(base + kBattleCtxPtrRva);
    if (!ctx || !PtrReadable(ctx + 0xE6A0u, 0x10u) || !PtrReadable(ctx + 0xEC4Cu, 0x10u)) {
        return false;
    }
    auto* e6a0 = reinterpret_cast<uint32_t*>(ctx + 0xE6A0u);
    auto* ec4c = reinterpret_cast<uint32_t*>(ctx + 0xEC4Cu);
    uint32_t buf_size = ec4c[0];
    if (buf_size < 0x1000u || buf_size > 0x200000u) {
        buf_size = 0x11000u;  // observed stock size for slot 0
    }
    for (uint8_t slot = 1; slot < count && slot < 4u; ++slot) {
        if (e6a0[slot] != 0) {
            continue;
        }
        void* mem = VirtualAlloc(nullptr, buf_size, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        if (!mem) {
            PartyBattleWarn("Party battle keyed buf: VirtualAlloc failed slot=%u size=%u", slot,
                                buf_size);
            return false;
        }
        std::memset(mem, 0, buf_size);
        e6a0[slot] = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(mem));
        ec4c[slot] = buf_size;
        g_e6a0_alloc[slot] = mem;
        g_e6a0_alloc_size[slot] = buf_size;
        PartyBattleLog("Party battle keyed buf: allocated slot=%u ptr=%p size=%u", slot, mem,
                            buf_size);
    }
    return true;
}

void TryRemapC8aFromKeyedBuffers(std::uintptr_t base, const char* tag) {
    if (!g_enabled.load() || base == 0) {
        return;
    }
    auto* ctx = *reinterpret_cast<uint8_t**>(base + kBattleCtxPtrRva);
    const uint8_t n = g_count.load();
    if (!ctx || n <= 1 || !PtrReadable(ctx + 0xC8A00u, 0x40u) ||
        !PtrReadable(ctx + 0xE6A0u, 0x10u)) {
        return;
    }
    const uint32_t* e6a0 = reinterpret_cast<const uint32_t*>(ctx + 0xE6A0u);
    const uint32_t c8a_base = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(ctx + 0xC8A00u));
    __try {
        for (uint8_t slot = 1; slot < n && slot < 4u; ++slot) {
            const uint32_t buf = e6a0[slot];
            if (buf == 0 || !PtrReadable(reinterpret_cast<void*>(static_cast<uintptr_t>(buf)), 0x10u)) {
                continue;
            }
            const uint32_t* src = reinterpret_cast<const uint32_t*>(static_cast<uintptr_t>(buf));
            // Skip empty/unpacked buffers (first relative still zero).
            if (src[0] == 0) {
                continue;
            }
            auto* dst = reinterpret_cast<uint32_t*>(ctx + 0xC8A00u + static_cast<uint32_t>(slot) * 0x10u);
            const uint32_t base_rel = buf - c8a_base;
            const uint32_t old0 = dst[0];
            for (int i = 0; i < 4; ++i) {
                dst[i] = base_rel + src[i];
            }
            PartyBattleLog(
                "Party battle keyed remap [%s]: slot=%u e6a0=%08X rel0 %u->%u (base_rel=%u)",
                tag ? tag : "?", slot, buf, old0, dst[0], base_rel);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        PartyBattleWarn("Party battle keyed remap faulted (%s)", tag ? tag : "?");
    }
}

void TryInjectExtraKeyedTasks(unsigned owner) {
    if (!g_enabled.load() || g_battle_keyed_tasks_injected || owner == 0) {
        return;
    }
    const std::uintptr_t base = grandia_ap::GetGrandiaModuleBase();
    if (base == 0 || !g_battle_scheduler_enqueue || !g_battle_keyed_task_callback) {
        return;
    }
    const uint8_t n = g_count.load();
    if (n <= 1) {
        return;
    }

    g_battle_keyed_tasks_injected = true;
    void* owner_ptr = reinterpret_cast<void*>(static_cast<uintptr_t>(owner));

    __try {
        if (!EnsureKeyedSlotBuffers(base, n)) {
            PartyBattleWarn("Party battle keyed inject: buffer alloc failed");
            return;
        }
        for (uint8_t slot = 1; slot < n && slot < 4u; ++slot) {
            const uint8_t char_id = g_ids[slot];
            if (char_id < 1u || char_id > kMaxPlayableCharId) {
                continue;
            }
            const int free = FindFreeSchedulerSlot(base);
            if (free < 0) {
                PartyBattleWarn("Party battle keyed inject: no free scheduler slot (want slot=%u)",
                                    slot);
                break;
            }
            const uint8_t selector = BattlePlayableSelectorForChar(char_id);
            auto* task = reinterpret_cast<uint8_t*>(
                CallSchedulerEnqueueKeyed(owner_ptr, static_cast<uint8_t>(free)));
            if (!task || !PtrReadable(task, 0x160u)) {
                PartyBattleWarn("Party battle keyed inject: enqueue failed for slot=%u", slot);
                continue;
            }
            task[0x24] = 0u;
            task[0x25] = slot;
            task[0x26] = selector;
            PartyBattleLog(
                "Party battle keyed inject: owner=%08X task=%p free=%d visible=%u char=%u (%s) sel=%u",
                owner, task, free, slot, char_id, CharName(char_id), selector);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        PartyBattleWarn("Party battle keyed inject faulted");
    }
}

void __declspec(naked) ApBattleKeyedTaskEnqueueProbeDetour() {
    __asm {
        // Stock at +12C211: push callback; call enqueue; add esp,8
        // then +12C21E: mov edi,[ebp-0x28]; mov ecx,[ebp-0x10]; mov cl,[ecx+edi]
        // Do NOT touch [ecx+edi] before edi is reloaded — leftover edi was &malloc (AV).
        mov byte ptr [g_battle_keyed_task_last_free_slot], dl
        push dword ptr [g_battle_keyed_task_callback]
        call dword ptr [g_battle_scheduler_enqueue]
        add esp, 8
        mov dword ptr [g_battle_keyed_task_last_task], eax
        mov edx, dword ptr [ebp + 8]
        mov dword ptr [g_battle_keyed_task_last_owner], edx
        pushad
        mov edi, dword ptr [ebp - 0x28]
        mov ecx, dword ptr [ebp - 0x10]
        movzx ecx, byte ptr [ecx + edi]
        movzx edx, bl
        movzx eax, byte ptr [g_battle_keyed_task_last_free_slot]
        push edx
        push ecx
        push eax
        push dword ptr [g_battle_keyed_task_last_task]
        push dword ptr [g_battle_keyed_task_last_owner]
        call ApBattleLogKeyedTaskEnqueue
        add esp, 20
        popad
        mov eax, dword ptr [g_battle_keyed_task_last_task]
        jmp dword ptr [g_ap_battle_keyed_task_enqueue_probe_resume]
    }
}

#if defined(_M_IX86)
void CallBattleKeyedPackBuilder(std::uintptr_t fn, void* actor) {
    auto* target = reinterpret_cast<void*>(fn);
    __asm {
        push actor
        call dword ptr [target]
        add esp, 4
    }
}
#else
void CallBattleKeyedPackBuilder(std::uintptr_t fn, void* actor) {
    (void)fn;
    (void)actor;
}
#endif

#if defined(_M_IX86)
int CallPackPathBuilder16A0(std::uintptr_t builder_fn, void* template_buf, uint32_t src_off,
                            void* c8a_base, uint32_t dest_off_shifted) {
    int result = 0;
    __asm {
        push dest_off_shifted
        push c8a_base
        mov edx, src_off
        mov ecx, template_buf
        call builder_fn
        mov result, eax
        add esp, 8
    }
    return result;
}
#else
int CallPackPathBuilder16A0(std::uintptr_t, void*, uint32_t, void*, uint32_t) {
    return 0;
}
#endif

void TrySynthesizeC8aFromKeyedEntries(std::uintptr_t base, const char* tag) {
    (void)base;
    (void)tag;
    return;
#if 0
    if (!g_enabled.load() || g_battle_pack_rebuilt || base == 0) {
        return;
    }
    auto* ctx = *reinterpret_cast<uint8_t**>(base + kBattleCtxPtrRva);
    const uint8_t n = g_count.load();
    if (!ctx || n == 0 || n > 4 || !PtrReadable(ctx + 0xC8A00u, 0x40u) ||
        !PtrReadable(ctx + 0x7A204u, 4)) {
        return;
    }

    g_battle_pack_rebuilt = true;
    uint8_t template_buf[8]{};
    if (PtrReadable(reinterpret_cast<void*>(base + kPDatPathTemplateRva), 8)) {
        std::memcpy(template_buf, reinterpret_cast<void*>(base + kPDatPathTemplateRva), 8);
    }
    const uint32_t keyed_base_off = *reinterpret_cast<uint32_t*>(ctx + 0x7A204u);
#if defined(_M_IX86)
    const auto builder_fn = base + kPackPathBuilderRva;
#endif

    __try {
        for (uint8_t slot = 0; slot < n; ++slot) {
            const uint8_t char_id = g_ids[slot];
            if (char_id < 1u || char_id > kMaxPlayableCharId) {
                continue;
            }
            const uint8_t selector = BattlePlayableSelectorForChar(char_id);
            const uint8_t key = ctx[0x64A07u + selector];
            auto* ent = ctx + 0x7A200u + keyed_base_off + static_cast<uint32_t>(key) * 8u;
            if (!PtrReadable(ent, 8)) {
                PartyBattleWarn("Party battle keyed c8a: missing entry slot=%u char=%u sel=%u",
                                    slot, char_id, selector);
                continue;
            }
            const uint16_t w0 = *reinterpret_cast<uint16_t*>(ent + 0);
            const uint16_t w2 = *reinterpret_cast<uint16_t*>(ent + 2);
            const uint16_t w6 = *reinterpret_cast<uint16_t*>(ent + 6);
            const uint32_t old_rel =
                *reinterpret_cast<uint32_t*>(ctx + 0xC8A00u + static_cast<uint32_t>(slot) * 0x10u);
            const uint32_t src_off = static_cast<uint32_t>(w0) << 11u;
            const uint32_t dest_off_shifted =
                (static_cast<uint32_t>(w6) + static_cast<uint32_t>(w2)) << 11u;
#if defined(_M_IX86)
            const int build_rc = CallPackPathBuilder16A0(builder_fn, template_buf, src_off,
                                                       ctx + 0xC8A00u, dest_off_shifted);
#else
            const int build_rc = 0;
#endif
            const uint32_t new_rel =
                *reinterpret_cast<uint32_t*>(ctx + 0xC8A00u + static_cast<uint32_t>(slot) * 0x10u);
            PartyBattleLog(
                "Party battle keyed c8a [%s]: slot=%u char=%u (%s) sel=%u entry={%u,%u,%u} rc=%d "
                "rel0 %u->%u",
                tag ? tag : "?", slot, char_id, CharName(char_id), selector, w0, w2, w6, build_rc,
                old_rel, new_rel);
        }
        LogPackedAllyModelSlots(base, tag ? tag : "post-keyed-c8a");
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        PartyBattleWarn("Party battle keyed c8a synthesis faulted (%s)", tag ? tag : "?");
    }
#endif
}

void TryRebuildBattlePlayablePack(std::uintptr_t base, const char* tag) {
    (void)base;
    (void)tag;
}

void ApBattleLogPackWrite(unsigned combatant, unsigned dest_rel) {
    if (!g_enabled.load() || g_battle_pdat_probe_logs_left <= 0) {
        return;
    }
    auto* actor = reinterpret_cast<uint8_t*>(combatant);
    if (!actor) {
        return;
    }
    --g_battle_pdat_probe_logs_left;
    PartyBattleLog(
        "Party battle c8a write: a24=%u a25=%u a26=%u a94=%u -> rel=%u slot=%u", actor[0x24],
        actor[0x25], actor[0x26], actor[0x94], dest_rel,
        dest_rel >= 0xC8A00u ? ((dest_rel - 0xC8A00u) >> 11) : 0u);
}

void ApBattleLogPackPrep(unsigned combatant, unsigned rel18, unsigned rel1c, unsigned rel20) {
    if (!g_enabled.load() || g_battle_pdat_probe_logs_left <= 0) {
        return;
    }
    auto* actor = reinterpret_cast<uint8_t*>(combatant);
    if (!actor) {
        return;
    }
    --g_battle_pdat_probe_logs_left;
    PartyBattleLog(
        "Party battle c8a prep: a24=%u a25=%u a26=%u a94=%u rel18=%u rel1c=%u rel20=%u",
        actor[0x24], actor[0x25], actor[0x26], actor[0x94], rel18, rel1c, rel20);
}

void ApBattleLogSpawnState(unsigned combatant, unsigned char_id) {
    auto* actor = reinterpret_cast<uint8_t*>(combatant);
    if (actor) {
        const uint8_t slot1 = actor[0x15A];
        if (slot1 >= 1u && slot1 <= 4u) {
            g_ally_combatants[slot1 - 1u] = combatant;
        }
    }
    if (!g_enabled.load() || g_battle_pdat_probe_logs_left <= 0) {
        return;
    }
    if (!actor) {
        return;
    }
    --g_battle_pdat_probe_logs_left;
    PartyBattleLog(
        "Party battle spawn probe: type=%u slot=%u char=%u (%s) "
        "a24=%u a25=%u a26=%u a94=%u afd=%u",
        actor[0x02], actor[0x15A], char_id, CharName(static_cast<uint8_t>(char_id)), actor[0x24],
        actor[0x25], actor[0x26], actor[0x94], actor[0xFD]);
}

bool PtrReadable(const void* p, size_t bytes) {
    if (!p || bytes == 0) {
        return false;
    }
    MEMORY_BASIC_INFORMATION mbi{};
    if (VirtualQuery(p, &mbi, sizeof(mbi)) == 0) {
        return false;
    }
    if (mbi.State != MEM_COMMIT) {
        return false;
    }
    const DWORD prot = mbi.Protect & 0xFFu;
    if (prot == PAGE_NOACCESS || prot == PAGE_EXECUTE || (mbi.Protect & PAGE_GUARD) != 0) {
        return false;
    }
    const auto start = reinterpret_cast<const uint8_t*>(mbi.BaseAddress);
    const auto end = start + mbi.RegionSize;
    const auto ptr = reinterpret_cast<const uint8_t*>(p);
    return ptr >= start && ptr + bytes <= end;
}

bool ModelHeaderReadable(void* header) {
    if (!header || !PtrReadable(header, 8)) {
        return false;
    }
    // +12D760: mov edx,[edi]; add edx,edi — first dword is relative to header.
    const uint32_t rel = *reinterpret_cast<uint32_t*>(header);
    if (rel == 0 || rel > 0x200000u) {
        return false;
    }
    return PtrReadable(static_cast<uint8_t*>(header) + rel, 4);
}

void* LeaderModelHeader() {
    const std::uintptr_t base = grandia_ap::GetGrandiaModuleBase();
    if (base == 0) {
        return nullptr;
    }
    auto* ctx = *reinterpret_cast<uint8_t**>(base + kBattleCtxPtrRva);
    if (!ctx || !PtrReadable(ctx + 0xC8A00u, 4)) {
        return nullptr;
    }
    const uint32_t rel = *reinterpret_cast<uint32_t*>(ctx + 0xC8A00u);
    if (rel == 0) {
        return nullptr;  // P_DAT not packed into c8a00 yet
    }
    auto* header = ctx + 0xC8A00u + rel;
    if (!ModelHeaderReadable(header)) {
        return nullptr;
    }
    return header;
}

void LogPackedAllyModelSlots(std::uintptr_t base, const char* tag) {
    auto* ctx = *reinterpret_cast<uint8_t**>(base + kBattleCtxPtrRva);
    if (!ctx || !PtrReadable(ctx + 0xC8A00u, 0x40u)) {
        PartyBattleLog("Party battle [%s]: c8a pack unreadable", tag ? tag : "?");
        return;
    }
    // Slot N header words live at ctx+0xc8a00 + N*0x10 (relative into the P_DAT blob).
    uint32_t rel[4]{};
    for (int i = 0; i < 4; ++i) {
        rel[i] = *reinterpret_cast<uint32_t*>(ctx + 0xC8A00u + static_cast<unsigned>(i) * 0x10u);
    }
    const uint8_t form = ctx[0x243];
    uint32_t tab0 = 0, tab2 = 0, tab6 = 0;
    if (PtrReadable(ctx + 0x7A200u, 4)) {
        const uint32_t base_off = *reinterpret_cast<uint32_t*>(ctx + 0x7A200u);
        auto* ent = ctx + 0x7A200u + base_off + static_cast<uint32_t>(form) * 8u;
        if (PtrReadable(ent, 8)) {
            tab0 = *reinterpret_cast<uint16_t*>(ent);
            tab2 = *reinterpret_cast<uint16_t*>(ent + 2);
            tab6 = *reinterpret_cast<uint16_t*>(ent + 6);
        }
    }
    PartyBattleLog(
        "Party battle [%s]: P_DAT pack form=%u entry={%u,%u,?,%u} sectors "
        "c8a rel=[%u,%u,%u,%u] (0=empty slot)",
        tag ? tag : "?", form, tab0, tab2, tab6, rel[0], rel[1], rel[2], rel[3]);
    if (PtrReadable(ctx + 0xE6A0u, 0x10u) && PtrReadable(ctx + 0xEC4Cu, 0x10u)) {
        const uint32_t* e6a0 = reinterpret_cast<const uint32_t*>(ctx + 0xE6A0u);
        const uint32_t* ec4c = reinterpret_cast<const uint32_t*>(ctx + 0xEC4Cu);
        PartyBattleLog(
            "Party battle [%s]: keyed buf e6a0=[%08X,%08X,%08X,%08X] "
            "ec4c=[%08X,%08X,%08X,%08X]",
            tag ? tag : "?", e6a0[0], e6a0[1], e6a0[2], e6a0[3], ec4c[0], ec4c[1], ec4c[2],
            ec4c[3]);
    }
}

void* ApBattleModelBindResolve(void* header) {
    if (g_enabled.load() && g_battle_pack_rebuilt && header != nullptr) {
        return header;
    }
    static bool logged_pack = false;
    const std::uintptr_t base = grandia_ap::GetGrandiaModuleBase();
    if (!logged_pack) {
        logged_pack = true;
        if (base != 0) {
            LogPackedAllyModelSlots(base, "first-model-bind");
        }
    }
    if (ModelHeaderReadable(header)) {
        return header;
    }
    void* fallback = LeaderModelHeader();
    if (fallback) {
        if (g_battle_ally_hook_logs_left > 0) {
            --g_battle_ally_hook_logs_left;
            PartyBattleWarn(
                "Party battle: model bind %p invalid — using leader header %p", header, fallback);
        }
        return fallback;
    }
    if (g_battle_ally_hook_logs_left > 0) {
        --g_battle_ally_hook_logs_left;
        PartyBattleWarn("Party battle: skip model bind — no packed header (%p)", header);
    }
    return nullptr;
}

int ApBattleAnimBindResolve(unsigned bank) {
    const std::uintptr_t base = grandia_ap::GetGrandiaModuleBase();
    if (base == 0) {
        return bank;
    }
    if (g_enabled.load() && g_battle_pack_rebuilt) {
        return static_cast<int>(bank);
    }
    auto* ctx = *reinterpret_cast<uint8_t**>(base + kBattleCtxPtrRva);
    if (!ctx) {
        return bank;
    }
    auto bank_ok = [&](unsigned b) -> bool {
        if (b > 0x40u) {
            return false;
        }
        auto* slot =
            reinterpret_cast<uint32_t*>(ctx + static_cast<std::uintptr_t>(b) * 36u + 0xa87cu);
        if (!PtrReadable(slot, 4)) {
            return false;
        }
        const uint32_t ptr = *slot;
        return ptr >= 0x10000u && PtrReadable(reinterpret_cast<void*>(ptr), 2);
    };
    if (bank_ok(bank)) {
        return static_cast<int>(bank);
    }
    // Justin/slot0 init uses bank 5 (low byte of word 0x605 at combatant+0xfd).
    constexpr unsigned kLeaderBank = 5u;
    if (bank_ok(kLeaderBank)) {
        if (g_battle_ally_hook_logs_left > 0) {
            --g_battle_ally_hook_logs_left;
            PartyBattleWarn("Party battle: anim bank %u invalid — using leader bank %u", bank,
                                kLeaderBank);
        }
        return static_cast<int>(kLeaderBank);
    }
    if (g_battle_ally_hook_logs_left > 0) {
        --g_battle_ally_hook_logs_left;
        PartyBattleWarn("Party battle: skip anim bind — bad bank %u", bank);
    }
    return -1;
}

void ApOnBattleLoadEntry() {
    const std::uintptr_t base = grandia_ap::GetGrandiaModuleBase();
    PartyBattleLog("Party battle: +12BDD0 load entry (enabled=%d)", g_enabled.load() ? 1 : 0);
    if (base == 0) {
        return;
    }
    if (!g_enabled.load()) {
        auto* ctx = *reinterpret_cast<uint8_t**>(base + kBattleCtxPtrRva);
        auto* map_obj = MapObject(base);
        if (ctx && map_obj && PtrReadable(ctx + 0x7A200u, 4)) {
            const uint8_t form = ctx[0x243];
            const uint32_t form_base_off = *reinterpret_cast<uint32_t*>(ctx + 0x7A200u);
            auto* ent = ctx + 0x7A200u + form_base_off + static_cast<uint32_t>(form) * 8u;
            if (PtrReadable(ent, 8)) {
                PartyBattleLog(
                    "Party battle stock capture: map0A=%u,%u,%u,%u form=%u entry={%u,%u,?,%u}",
                    map_obj[0x0A], map_obj[0x0B], map_obj[0x0C], map_obj[0x0D], form,
                    static_cast<unsigned>(*reinterpret_cast<uint16_t*>(ent + 0)),
                    static_cast<unsigned>(*reinterpret_cast<uint16_t*>(ent + 2)),
                    static_cast<unsigned>(*reinterpret_cast<uint16_t*>(ent + 6)));
            }
        }
        return;
    }
    if (g_battle_pack_rebuilt) {
        PartyBattleLog(
            "Party battle: +12BDD0 re-entry mid-fight (pack stable, skip asset prep)");
        return;
    }
    g_battle_assets_prepared = true;
    g_battle_keyed_tasks_injected = false;
    g_battle_ally_hook_logs_left = kPartyBattleLogs ? 16 : 0;
    g_battle_pdat_probe_logs_left = kPartyBattleLogs ? 48 : 0;
    for (uint8_t i = 0; i < 4u; ++i) {
        g_party_slot_to_c8a[i] = i;
    }
    // Re-apply right before asset pack/spawn inside +12BDD0.
    PrepareBattlePartyAssets(base, "battle-load");
    LogPackedAllyModelSlots(base, "battle-load-pre-pack");
    auto* ctx = *reinterpret_cast<uint8_t**>(base + kBattleCtxPtrRva);
    if (ctx) {
        const int stock_group = FindStockGroup(g_ids, g_count.load());
        PartyBattleLog(
            "Party battle P_DAT map: desired_group=%d sel[0..11]=%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u",
            stock_group, ctx[0x64A07], ctx[0x64A08], ctx[0x64A09], ctx[0x64A0A], ctx[0x64A0B],
            ctx[0x64A0C], ctx[0x64A0D], ctx[0x64A0E], ctx[0x64A0F], ctx[0x64A10], ctx[0x64A11],
            ctx[0x64A12]);
        if (PtrReadable(ctx + 0x7A204u, 4)) {
            const uint32_t base_off = *reinterpret_cast<uint32_t*>(ctx + 0x7A204u);
            auto entry_word = [&](uint8_t sel, unsigned off) -> unsigned {
                const uint8_t key = ctx[0x64A07u + sel];
                auto* ent = ctx + 0x7A200u + base_off + static_cast<uint32_t>(key) * 8u;
                if (!PtrReadable(ent, 8)) {
                    return 0xFFFFu;
                }
                return off == 6 ? static_cast<unsigned>(*reinterpret_cast<uint16_t*>(ent + 6))
                                : static_cast<unsigned>(*reinterpret_cast<uint16_t*>(ent + off));
            };
            PartyBattleLog(
                "Party battle P_DAT entries: s1={%u,%u,%u} s2={%u,%u,%u} s3={%u,%u,%u} "
                "s4={%u,%u,%u} s5={%u,%u,%u}",
                entry_word(1, 0), entry_word(1, 2), entry_word(1, 6), entry_word(2, 0),
                entry_word(2, 2), entry_word(2, 6), entry_word(3, 0), entry_word(3, 2),
                entry_word(3, 6), entry_word(4, 0), entry_word(4, 2), entry_word(4, 6),
                entry_word(5, 0), entry_word(5, 2), entry_word(5, 6));
        }
        auto* table = *reinterpret_cast<uint8_t**>(base + kBattleFormTablePtrRva);
        if (table && PtrReadable(ctx + 0x7A200u, 4)) {
            const uint32_t form_base_off = *reinterpret_cast<uint32_t*>(ctx + 0x7A200u);
            for (unsigned f = 0; f < 0x100u; ++f) {
                const uint8_t count = table[f];
                if (count != 3u) {
                    continue;
                }
                const uint32_t row = static_cast<uint32_t>(f) * 4u + 0x10u;
                if (!PtrReadable(table + row + 3, 1)) {
                    continue;
                }
                if (table[row + 0] == 1u && table[row + 1] == 2u && table[row + 2] == 4u) {
                    auto* ent = ctx + 0x7A200u + form_base_off + static_cast<uint32_t>(f) * 8u;
                    if (PtrReadable(ent, 8)) {
                        PartyBattleLog(
                            "Party battle stock form match: form=%u entry={%u,%u,?,%u}", f,
                            static_cast<unsigned>(*reinterpret_cast<uint16_t*>(ent + 0)),
                            static_cast<unsigned>(*reinterpret_cast<uint16_t*>(ent + 2)),
                            static_cast<unsigned>(*reinterpret_cast<uint16_t*>(ent + 6)));
                    }
                }
            }
        }
    }
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
        // [edx+ecx+relocated_disp] — never hardcode 0x6015A1 (ASLR).
        push eax
        mov eax, dword ptr [g_ap_party_char_table_abs]
        add eax, edx
        add eax, ecx
        mov bl, byte ptr [eax]
        pop eax
        jmp dword ptr [g_ap_party_char_resume]
    }
}

void __declspec(naked) ApBattleAllyCountDetour() {
    __asm {
        // ecx = formation index (from [battle_ctx+0x243])
        push ecx
        push edx
        push ecx
        call ApBattleAllyResolveCount
        add esp, 4
        pop edx
        pop ecx
        // eax = party count → stock resumes at cmp edi, eax
        jmp dword ptr [g_ap_battle_ally_count_resume]
    }
}

void __declspec(naked) ApBattleAllySpawnGateDetour() {
    __asm {
        pushad
        call ApBattleAllyShouldForceSpawn
        test eax, eax
        popad
        jnz force_spawn
        // Stolen: lea eax,[ebx-1]; cmp eax,3
        lea eax, [ebx - 1]
        cmp eax, 3
        jmp dword ptr [g_ap_battle_ally_gate_resume]

    force_spawn:
        jmp dword ptr [g_ap_battle_ally_spawn]
    }
}

void __declspec(naked) ApBattleAllyCharIdDetour() {
    __asm {
        // eax=table, ecx=index, edx=slot0, esi=combatant
        push edx
        push eax
        push ecx
        push edx
        call ApBattleAllyResolveCharId
        add esp, 12
        mov byte ptr [esi + 0x10F], al
        push eax
        push eax
        push esi
        call ApBattleLogSpawnState
        add esp, 8
        pop eax
        // [esp] = spawn slot0; remap edx to a packed model slot before jump-table
        push eax
        push dword ptr [esp + 4]
        call ApBattleAllyResolveModelSlot
        add esp, 8
        mov edx, eax
        add esp, 4
        jmp dword ptr [g_ap_battle_ally_char_resume]
    }
}

void __declspec(naked) ApBattlePlayablePackProbeDetour() {
    __asm {
        // Hook +12C8F5: stock `mov al, [ecx+esi+0x64A07]`
        // ebx = combatant, esi = battle_ctx, ecx = selector.
        pushad
        movzx eax, cl
        push eax
        push esi
        push ebx
        call ApBattleResolvePlayablePackProbe
        add esp, 12
        mov byte ptr [esp + 28], al  // pushad frame saved eax
        popad
        jmp dword ptr [g_ap_battle_playable_pack_probe_resume]
    }
}

void __declspec(naked) ApBattlePackWriteProbeDetour() {
    __asm {
        // Hook +12C773:
        //   add ecx, 0xC8A00
        //   add eax, ecx
        //   lea ecx, [ebp-0x14]
        pushad
        push ecx
        push ebx
        call ApBattleLogPackWrite
        add esp, 8
        popad
        add ecx, 0xC8A00
        add eax, ecx
        lea ecx, [ebp - 0x14]
        jmp dword ptr [g_ap_battle_pack_write_probe_resume]
    }
}

void __declspec(naked) ApBattlePackPrepProbeDetour() {
    __asm {
        // Hook +12C860:
        //   mov ecx, [ebp-0x18]
        //   lea eax, [esi+0xC8A00]
        pushad
        push dword ptr [ebp - 0x20]
        push dword ptr [ebp - 0x1C]
        push dword ptr [ebp - 0x18]
        push ebx
        call ApBattleLogPackPrep
        add esp, 16
        popad
        mov ecx, dword ptr [ebp - 0x18]
        lea eax, [esi + 0xC8A00]
        jmp dword ptr [g_ap_battle_pack_prep_probe_resume]
    }
}

void __declspec(naked) ApBattleLoadDetour() {
    __asm {
        pushad
        call ApOnBattleLoadEntry
        popad
        jmp dword ptr [g_ap_battle_load_tramp]
    }
}

static bool HdContentNameIsWin(const char* s) {
    return s && s[0] == 'w' && s[1] == 'i' && s[2] == 'n' && s[3] == '\0';
}

int ApHdLoadContentShouldSkip(const char* a1, const char* a2, const char* a3) {
    if constexpr (kSkipHdLoadContentMode <= 0) {
        return 0;
    }
    const bool is_win =
        HdContentNameIsWin(a1) || HdContentNameIsWin(a2) || HdContentNameIsWin(a3);
    const bool skip = (kSkipHdLoadContentMode >= 2) || is_win;
    // Avoid fopen/fflush on every LoadContent — only log actual skips.
    if (skip) {
        static int skip_logs = 8;
        if (skip_logs > 0) {
            --skip_logs;
            grandia_ap::LogInfo("Party: skipped LoadContent(%s, %s, %s)", a1 ? a1 : "(null)",
                                a2 ? a2 : "(null)", a3 ? a3 : "(null)");
        }
    }
    return skip ? 1 : 0;
}

void __declspec(naked) ApHdLoadContentDetour() {
    __asm {
        // thiscall: ecx=this. Helper is cdecl and must not clobber ecx on passthrough.
        pushad
        // After pushad: [esp+0x20]=ret, +0x24=a1, +0x28=a2, +0x2C=a3.
        push dword ptr [esp + 0x2C]
        push dword ptr [esp + 0x2C]
        push dword ptr [esp + 0x2C]
        call ApHdLoadContentShouldSkip
        add esp, 12
        test eax, eax
        jz hd_load_passthru
        popad
        xor eax, eax
        ret 0x18
    hd_load_passthru:
        popad
        jmp dword ptr [g_ap_hd_load_content_tramp]
    }
}

void __declspec(naked) ApBattleModelBindDetour() {
    __asm {
        cmp byte ptr [g_battle_assets_prepared], 0
        je model_bind_passthru
        pushad
        push edx
        call ApBattleModelBindResolve
        add esp, 4
        test eax, eax
        jz model_bind_skip
        mov dword ptr [esp + 8], eax  // pushad: [esp+8]=saved edx
        popad
        jmp dword ptr [g_ap_battle_model_bind_tramp]
    model_bind_skip:
        popad
        ret
    model_bind_passthru:
        jmp dword ptr [g_ap_battle_model_bind_tramp]
    }
}

void __declspec(naked) ApBattleAnimBindDetour() {
    __asm {
        cmp byte ptr [g_battle_assets_prepared], 0
        je anim_bind_passthru
        pushad
        movzx eax, cl
        push eax
        call ApBattleAnimBindResolve
        add esp, 4
        cmp eax, -1
        je anim_bind_skip
        mov byte ptr [esp + 4], al  // pushad: [esp+4]=ecx → patch cl
        popad
        jmp dword ptr [g_ap_battle_anim_bind_tramp]
    anim_bind_skip:
        popad
        ret
    anim_bind_passthru:
        jmp dword ptr [g_ap_battle_anim_bind_tramp]
    }
}

void ApOnBattlePartyInitEntry() {
    if (!g_enabled.load()) {
        return;
    }
    const std::uintptr_t base = grandia_ap::GetGrandiaModuleBase();
    if (base == 0) {
        return;
    }
    const uint8_t mode = *reinterpret_cast<uint8_t*>(base + kBattleModeRva);
    if (!IsBattleMode(mode)) {
        g_battle_party_init_synced = false;
        return;
    }

    SetBattleAllyGatePatch(base, true);
    SetBattleFa90EarlyExitPatch(base, true);

    // Keep the early battle-init hook as passive as possible. The actual roster /
    // formation mutation happens at +12BDD0 immediately before asset pack + spawn.
    // This avoids destabilizing pre-load setup before the ally P_DAT path runs.
    if (!g_battle_party_init_synced) {
        g_battle_party_init_synced = true;
        LogBattlePartyState(base, "battle prep");
        LogBattleUnitList(base);
    }
}

int ApFillUiPartyCache(std::uintptr_t cache_rva) {
    if (!g_enabled.load()) {
        return 0;
    }
    const std::uintptr_t base = grandia_ap::GetGrandiaModuleBase();
    if (base == 0) {
        return 0;
    }
    WriteUiPartyCache(base, cache_rva);
    // Seed unrecruited custom members so stash/shop equip targets exist (MapObj blocks,
    // not MapObj+0A roster). Pack bags so mid-list zeros (common after story dump +
    // equip-to-custom-member) cannot hide trailing items in the merged party list.
    if (uint8_t* map_obj = MapObject(base)) {
        SeedMissingCharacterBlocks(map_obj);
        SanitizeCustomPartyInventories(map_obj);
    }
    return 1;
}

// First nonzero costume entry per char in status table VA 0x601D2F (row*8+char_id).
struct PreferredFace {
    uint8_t fc_row;
    uint8_t face_i;
};
constexpr PreferredFace kPreferredFace[9] = {
    {0, 0},
    {1, 0},    // Justin
    {4, 24},   // Feena
    {1, 11},   // Sue
    {10, 37},  // Gadwin
    {15, 22},  // Rapp
    {15, 29},  // Milda
    {15, 34},  // Guido
    {15, 39},  // Liete
};

bool ResolveFcDatPath(uint8_t fc_row, char* out, size_t out_size) {
    const std::uintptr_t base = grandia_ap::GetGrandiaModuleBase();
    if (base == 0 || fc_row == 0 || !out || out_size == 0) {
        return false;
    }
    char exe_path[MAX_PATH]{};
    if (!GetModuleFileNameA(reinterpret_cast<HMODULE>(base), exe_path, MAX_PATH)) {
        return false;
    }
    const std::string exe_dir = Dirname(exe_path);
    char rel_upper[64]{};
    char rel_mix[64]{};
    char rel_lower[64]{};
    std::snprintf(rel_upper, sizeof(rel_upper), "\\content\\FIELD\\FC%02u.DAT", fc_row);
    std::snprintf(rel_mix, sizeof(rel_mix), "\\content\\FIELD\\fc%02u.dat", fc_row);
    std::snprintf(rel_lower, sizeof(rel_lower), "\\content\\field\\fc%02u.dat", fc_row);
    const char* all_rels[] = {rel_upper, rel_mix, rel_lower};
    for (const char* rel : all_rels) {
        if (std::snprintf(out, out_size, "%s%s", exe_dir.c_str(), rel) <= 0) {
            continue;
        }
        if (FileExists(out)) {
            return true;
        }
    }
    return false;
}

uint8_t* EnsureFcBank(uint8_t fc_row) {
    if (fc_row == 0 || fc_row >= kMaxFcRow) {
        return nullptr;
    }
    if (g_fc_arenas[fc_row]) {
        return g_fc_arenas[fc_row];
    }
    char path[MAX_PATH]{};
    if (!ResolveFcDatPath(fc_row, path, sizeof(path))) {
        grandia_ap::LogWarn("Party face: FC%02u.DAT not found under content/FIELD", fc_row);
        return nullptr;
    }
    std::vector<uint8_t> file;
    if (!ReadFileAll(path, &file) || file.empty()) {
        grandia_ap::LogWarn("Party face: failed to read %s", path);
        return nullptr;
    }
    const size_t alloc_size = kFcPayloadOff + file.size() + 0x1000u;
    auto* arena = static_cast<uint8_t*>(
        VirtualAlloc(nullptr, alloc_size, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    if (!arena) {
        grandia_ap::LogWarn("Party face: VirtualAlloc failed for FC%02u (%u bytes)", fc_row,
                            static_cast<unsigned>(alloc_size));
        return nullptr;
    }
    std::memcpy(arena + kFcPayloadOff, file.data(), file.size());
    g_fc_arenas[fc_row] = arena;
    grandia_ap::LogInfo("Party face: private FC%02u bank %p (%u file bytes)", fc_row,
                        reinterpret_cast<void*>(arena), static_cast<unsigned>(file.size()));
    return arena;
}

void FreeFcBanks() {
    for (unsigned i = 0; i < kMaxFcRow; ++i) {
        if (g_fc_arenas[i]) {
            VirtualFree(g_fc_arenas[i], 0, MEM_RELEASE);
            g_fc_arenas[i] = nullptr;
        }
    }
}

void* EnsureFaceDest(std::uintptr_t base) {
    auto** slot = reinterpret_cast<uint8_t**>(base + kFaceDestPtrRva);
    if (*slot) {
        return *slot + 0x30000;
    }
    void* mem = VirtualAlloc(nullptr, 0x140000, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!mem) {
        return nullptr;
    }
    *slot = reinterpret_cast<uint8_t*>(mem) + 0x40000;
    return *slot + 0x30000;
}

bool PickFace(std::uintptr_t base, uint8_t char_id, uint8_t* out_fc, uint8_t* out_face_i) {
    if (!out_fc || !out_face_i || char_id < 1 || char_id > 8) {
        return false;
    }
    if (uint8_t* map_obj = MapObject(base)) {
        const uint8_t row = map_obj[0x51];
        if (row > 0 && row < kMaxFcRow) {
            const auto* table = reinterpret_cast<const uint8_t*>(base + kCostumeStatusTableRva);
            const uint8_t variant = table[static_cast<size_t>(char_id) + static_cast<size_t>(row) * 8u];
            if (variant != 0) {
                *out_fc = row;
                *out_face_i = static_cast<uint8_t>(variant - 1u);
                return true;
            }
        }
    }
    *out_fc = kPreferredFace[char_id].fc_row;
    *out_face_i = kPreferredFace[char_id].face_i;
    return kPreferredFace[char_id].fc_row != 0;
}

void CallFaceLoad(void* face_fn, unsigned handle, unsigned face_i, void* dest) {
    if (!face_fn || !dest) {
        return;
    }
#if defined(_M_IX86)
    __asm {
        push ebx
        push esi
        push edi
        mov ecx, handle
        mov edx, face_i
        mov eax, face_fn
        push dest
        call eax
        add esp, 4
        pop edi
        pop esi
        pop ebx
    }
#else
    (void)handle;
    (void)face_i;
#endif
}

void ApRunUiFaces(std::uintptr_t cache_rva, std::uintptr_t handle_table_rva) {
    const std::uintptr_t base = grandia_ap::GetGrandiaModuleBase();
    if (base == 0) {
        return;
    }
    auto* cache = reinterpret_cast<uint8_t*>(base + cache_rva);
    auto* handles = reinterpret_cast<uint16_t*>(base + handle_table_rva);
    void* dest = EnsureFaceDest(base);
    if (!dest) {
        grandia_ap::LogWarn("Party face: dest buffer unavailable");
        return;
    }
    void* face_fn = reinterpret_cast<void*>(base + kFaceLoadRva);
    for (unsigned slot = 0; slot < 4u; ++slot) {
        const uint8_t char_id = cache[slot];
        if (char_id == 0 || char_id > 8) {
            continue;
        }
        uint8_t fc_row = 0;
        uint8_t face_i = 0;
        if (!PickFace(base, char_id, &fc_row, &face_i)) {
            continue;
        }
        uint8_t* bank = EnsureFcBank(fc_row);
        if (!bank) {
            continue;
        }
        // Field caller +72288 keeps using [640E6C]; override is menu-only and cleared after.
        g_ap_face_bank_override = bank;
        const unsigned handle = slot | 0x8000u;
        CallFaceLoad(face_fn, handle, face_i, dest);
        g_ap_face_bank_override = nullptr;
        handles[char_id] = static_cast<uint16_t>(handle);
    }
}

void ApStatusCustomFaces() { ApRunUiFaces(kMenuPartyCacheRva, kStatusFaceHandleRva); }

void ApStashCustomFaces() { ApRunUiFaces(kStashPartyCacheRva, kStashFaceHandleRva); }

void ApItemCustomFaces() { ApRunUiFaces(kItemPartyCacheRva, kItemFaceHandleRva); }

int ApMenuFillTryCustom() { return ApFillUiPartyCache(kMenuPartyCacheRva); }

int ApStashFillTryCustom() { return ApFillUiPartyCache(kStashPartyCacheRva); }

int ApItemFillTryCustom() { return ApFillUiPartyCache(kItemPartyCacheRva); }

void ApSanitizePartyInventoriesFromHook() {
    if (!g_enabled.load()) {
        return;
    }
    const std::uintptr_t base = grandia_ap::GetGrandiaModuleBase();
    if (base == 0) {
        return;
    }
    if (uint8_t* map_obj = MapObject(base)) {
        SanitizeCustomPartyInventories(map_obj);
    }
}

void ApLogHookHit(const char* tag) {
    grandia_ap::LogInfo("Party: hook-hit %s (custom_party=%d)", tag ? tag : "?",
                        g_enabled.load() ? 1 : 0);
}

void ApRebuildItemUiAfterEquipRemove() {
    grandia_ap::LogInfo("Party: post-remove ITEM UI rebuild (custom_party=%d)",
                        g_enabled.load() ? 1 : 0);
    if (!g_enabled.load()) {
        return;
    }
    const std::uintptr_t base = grandia_ap::GetGrandiaModuleBase();
    if (base == 0) {
        grandia_ap::LogWarn("Party: post-remove rebuild skipped (no module base)");
        return;
    }
    if (uint8_t* map_obj = MapObject(base)) {
        SanitizeCustomPartyInventories(map_obj);
    }
    RebuildItemAssignList(base);
}

void ApOnInvAddDone() {
    ApLogHookHit("inv-add");
    ApSanitizePartyInventoriesFromHook();
    ApRebuildItemUiAfterEquipRemove();
}

void ApOnInvRemoveDone() {
    ApLogHookHit("inv-remove");
    ApRebuildItemUiAfterEquipRemove();
}

void ApOnItemGiveDone(int which) {
    char tag[32]{};
    std::snprintf(tag, sizeof(tag), "item-give-%c", static_cast<char>('A' + which));
    ApLogHookHit(tag);
    ApRebuildItemUiAfterEquipRemove();
}

void ApOnItemPaintDone() {
    if (g_item_paint_hit_logs_left > 0) {
        --g_item_paint_hit_logs_left;
        ApLogHookHit("item-paint");
    }
    if (!g_enabled.load() || g_in_item_ui_fix) {
        return;
    }
    const std::uintptr_t base = grandia_ap::GetGrandiaModuleBase();
    if (base == 0) {
        return;
    }
    auto* widget_a = reinterpret_cast<uint8_t*>(base + kItemWidgetARva);
    auto* widget_b = reinterpret_cast<uint8_t*>(base + kItemWidgetBRva);
    bool active = false;
    __try {
        active = (*reinterpret_cast<uint16_t*>(widget_a) != 0) ||
                 (*reinterpret_cast<uint16_t*>(widget_b) != 0);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return;
    }
    if (!active) {
        return;
    }
    const DWORD now = GetTickCount();
    // Debounce — paint can run often; leave/re-enter equivalent only needs to land once
    // after the inventory-mutating action.
    if (now - g_last_item_ui_rebuild_tick < 100u) {
        return;
    }
    g_last_item_ui_rebuild_tick = now;
    g_in_item_ui_fix = true;
    grandia_ap::LogInfo("Party: ITEM paint-triggered UI rebuild");
    if (uint8_t* map_obj = MapObject(base)) {
        SanitizeCustomPartyInventories(map_obj);
    }
    RebuildItemAssignList(base);
    g_in_item_ui_fix = false;
}

void __declspec(naked) ApInvAddDetour() {
    __asm {
        pop dword ptr [g_ap_inv_add_ret]
        call dword ptr [g_ap_inv_add_tramp]
        pushad
        call ApOnInvAddDone
        popad
        jmp dword ptr [g_ap_inv_add_ret]
    }
}

void __declspec(naked) ApInvRemoveDetour() {
    __asm {
        pop dword ptr [g_ap_inv_remove_ret]
        call dword ptr [g_ap_inv_remove_tramp]
        pushad
        call ApOnInvRemoveDone
        popad
        jmp dword ptr [g_ap_inv_remove_ret]
    }
}

void __declspec(naked) ApEquipAfterRemoveDetour() {
    __asm {
        pushad
        call ApOnInvRemoveDone
        popad
        mov ecx, 5
        jmp dword ptr [g_ap_equip_after_remove_exit]
    }
}

void __declspec(naked) ApItemGiveEquipADetour() {
    __asm {
        pop dword ptr [g_ap_item_give_a_ret]
        call dword ptr [g_ap_item_give_a_tramp]
        pushad
        push 0
        call ApOnItemGiveDone
        add esp, 4
        popad
        jmp dword ptr [g_ap_item_give_a_ret]
    }
}

void __declspec(naked) ApItemGiveEquipBDetour() {
    __asm {
        pop dword ptr [g_ap_item_give_b_ret]
        call dword ptr [g_ap_item_give_b_tramp]
        pushad
        push 1
        call ApOnItemGiveDone
        add esp, 4
        popad
        jmp dword ptr [g_ap_item_give_b_ret]
    }
}

void __declspec(naked) ApItemGiveEquipCDetour() {
    __asm {
        pop dword ptr [g_ap_item_give_c_ret]
        call dword ptr [g_ap_item_give_c_tramp]
        pushad
        push 2
        call ApOnItemGiveDone
        add esp, 4
        popad
        jmp dword ptr [g_ap_item_give_c_ret]
    }
}

void __declspec(naked) ApItemGiveEquipDDetour() {
    __asm {
        pop dword ptr [g_ap_item_give_d_ret]
        call dword ptr [g_ap_item_give_d_tramp]
        pushad
        push 3
        call ApOnItemGiveDone
        add esp, 4
        popad
        jmp dword ptr [g_ap_item_give_d_ret]
    }
}

void __declspec(naked) ApItemGiveEquipEDetour() {
    __asm {
        pop dword ptr [g_ap_item_give_e_ret]
        call dword ptr [g_ap_item_give_e_tramp]
        pushad
        push 4
        call ApOnItemGiveDone
        add esp, 4
        popad
        jmp dword ptr [g_ap_item_give_e_ret]
    }
}

// thiscall paint helpers — no stack args; do not pop the return address.
void __declspec(naked) ApItemPaintDetour() {
    __asm {
        call dword ptr [g_ap_item_paint_tramp]
        pushad
        call ApOnItemPaintDone
        popad
        ret
    }
}

void __declspec(naked) ApItemPaint2Detour() {
    __asm {
        call dword ptr [g_ap_item_paint2_tramp]
        pushad
        call ApOnItemPaintDone
        popad
        ret
    }
}

void __declspec(naked) ApBattlePartyInitDetour() {
    __asm {
        pushad
        call ApOnBattlePartyInitEntry
        popad
        jmp dword ptr [g_ap_battle_party_init_tramp]
    }
}

void __declspec(naked) ApMenuFillDetour() {
    __asm {
        pushad
        call ApMenuFillTryCustom
        test eax, eax
        popad
        jnz menu_filled
        jmp dword ptr [g_ap_menu_fill_tramp]

    menu_filled:
        // Original path did: call [...]; mov esi,[70cf90]; add esp,4
        // We replaced only the mov — still must balance the prior call.
        add esp, 4
        pushad
        call ApStatusCustomFaces
        popad
        jmp dword ptr [g_ap_menu_fill_skip]
    }
}

void __declspec(naked) ApStashFillDetour() {
    __asm {
        pushad
        call ApStashFillTryCustom
        test eax, eax
        popad
        jnz stash_filled
        jmp dword ptr [g_ap_stash_fill_tramp]

    stash_filled:
        add esp, 4
        pushad
        call ApStashCustomFaces
        popad
        jmp dword ptr [g_ap_stash_fill_skip]
    }
}

void __declspec(naked) ApItemFillDetour() {
    __asm {
        pushad
        call ApItemFillTryCustom
        test eax, eax
        popad
        jnz item_filled
        jmp dword ptr [g_ap_item_fill_tramp]

    item_filled:
        add esp, 4
        pushad
        call ApItemCustomFaces
        popad
        jmp dword ptr [g_ap_item_fill_skip]
    }
}

void __declspec(naked) ApFaceBankLoadDetour() {
    __asm {
        // +56FF0 still needs eax = handle after this site (stored to [ebp-0x90]).
        // Menu override path never touched eax (hence menus worked); stock path must not clobber it.
        push eax
        mov esi, dword ptr [g_ap_face_bank_override]
        test esi, esi
        jnz face_bank_done
        mov eax, dword ptr [g_ap_face_bank_slot_abs]
        mov esi, dword ptr [eax]
    face_bank_done:
        pop eax
        jmp dword ptr [g_ap_face_bank_resume]
    }
}

}  // extern "C"
#endif

namespace grandia_ap {

bool IsPartyCustomEnabled() { return g_enabled.load(); }

void SetPartyCustomEnabled(bool enabled) { g_enabled.store(enabled); }

bool IsPartyApMode() { return g_party_mode.load() == kCustomPartyUnlocks; }

bool IsPartyUnlocksMode() { return g_party_mode.load() == kCustomPartyUnlocks; }

bool IsPartyRouletteMode() { return g_party_mode.load() == kCustomPartyRoulette; }

bool IsPartyOverlayEnabled() { return g_party_mode.load() == kCustomPartyUnlocks; }

unsigned GetCustomPartyMode() { return g_party_mode.load(); }

bool IsCharacterUnlocked(uint8_t char_id) { return CharacterUnlocked(char_id); }

uint8_t UnlockedCharacterCount() { return CountUnlockedCharacters(); }

uint8_t PartyMaxSlots() { return MaxPartySlots(); }

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

void ApplyCustomPartyConfig(unsigned mode) {
    if (mode > kCustomPartyUnlocks) {
        mode = kCustomPartyVanilla;
    }
    g_party_mode.store(mode);

    if (mode == kCustomPartyVanilla) {
        g_enabled.store(false);
        g_unlock_mask.store(0);
        g_ap_party_initialized.store(false);
        LogInfo("CONFIG custom_party=vanilla (stock parties, no overlay)");
        return;
    }

    if (mode == kCustomPartyRoulette) {
        // Roster arrives via CONFIG custom_party_roster — enable override once set.
        g_enabled.store(true);
        LogInfo("CONFIG custom_party=roulette (waiting for roster; no overlay)");
        return;
    }

    // Unlocks: Justin-only start + unlock gating + Save Party overlay.
    g_enabled.store(true);
    g_unlock_mask.fetch_or(1u << 1);
    if (!g_ap_party_initialized.load()) {
        const uint8_t justin = 1;
        SetCustomParty(&justin, 1);
        g_ap_party_initialized.store(true);
        LogInfo("CONFIG custom_party=unlocks — Justin only");
    } else {
        LogInfo("CONFIG custom_party=unlocks — roster preserved");
    }
}

void ApplyCustomPartyRoster(const uint8_t* ids, uint8_t count) {
    if (g_party_mode.load() != kCustomPartyRoulette) {
        LogWarn("custom_party_roster ignored (mode is not roulette)");
        return;
    }
    if (!ids || count != 4) {
        LogWarn("custom_party_roster requires exactly 4 character ids");
        return;
    }
    for (uint8_t i = 0; i < count; ++i) {
        if (ids[i] < 1 || ids[i] > kMaxPlayableCharId) {
            LogWarn("custom_party_roster invalid id %u", static_cast<unsigned>(ids[i]));
            return;
        }
    }
    uint32_t mask = 0;
    for (uint8_t i = 0; i < count; ++i) {
        mask |= (1u << ids[i]);
    }
    g_unlock_mask.store(mask);
    SetCustomParty(ids, count);
    g_enabled.store(true);
    g_ap_party_initialized.store(true);
    LogInfo("Roulette party locked: %s %s %s %s", CharName(ids[0]), CharName(ids[1]),
            CharName(ids[2]), CharName(ids[3]));
}

void ClearCharacterUnlockState() {
    if (g_party_mode.load() != kCustomPartyUnlocks) {
        return;
    }
    // Keep Justin; wipe other unlocks so bridge can re-apply from items_received.
    g_unlock_mask.store(1u << 1);
    LogInfo("Cleared character unlock state for save SYNC (Justin retained)");
}

bool TryHandleCharacterUnlockItem(unsigned ap_item_id) {
    if (ap_item_id <= kCharacterItemBase || ap_item_id > kCharacterItemBase + kMaxPlayableCharId) {
        return false;
    }
    const uint8_t char_id = static_cast<uint8_t>(ap_item_id - kCharacterItemBase);
    if (char_id < 1 || char_id > kMaxPlayableCharId) {
        return false;
    }
    // Swallow character-band ids in all modes so they never hit gold/stash delivery.
    if (g_party_mode.load() != kCustomPartyUnlocks) {
        LogInfo("Ignored character unlock item id=%u (custom_party mode=%u)",
                static_cast<unsigned>(char_id), g_party_mode.load());
        return true;
    }
    const uint32_t bit = 1u << char_id;
    const uint32_t prev = g_unlock_mask.fetch_or(bit);
    if ((prev & bit) == 0) {
        LogInfo("Character unlocked: %s (id=%u)", CharName(char_id), static_cast<unsigned>(char_id));
        char toast[96];
        std::snprintf(toast, sizeof(toast), "%s can join your party", CharName(char_id));
        ShowD3dOverlayToast(toast, 4000, 0x6A5ACDu);  // SlateBlue — Useful
    }
    return true;
}

void ApplySavedCustomParty(const uint8_t* ids, uint8_t count) {
    if (g_party_mode.load() == kCustomPartyVanilla) {
        return;
    }
    if (!ids || count < 1 || count > 4) {
        return;
    }
    for (uint8_t i = 0; i < count; ++i) {
        if (ids[i] < 1 || ids[i] > kMaxPlayableCharId) {
            LogWarn("GAP1 party restore skipped — invalid char id %u", static_cast<unsigned>(ids[i]));
            return;
        }
    }
    // Roulette: CONFIG roster is authoritative; GAP1 restore only if not yet applied.
    if (g_party_mode.load() == kCustomPartyRoulette && g_ap_party_initialized.load()) {
        return;
    }
    SetCustomParty(ids, count);
    g_enabled.store(true);
    g_ap_party_initialized.store(true);
    if (g_party_mode.load() == kCustomPartyUnlocks) {
        // Ensure members present in the saved roster stay usable until items re-apply.
        uint32_t mask = g_unlock_mask.load() | (1u << 1);
        for (uint8_t i = 0; i < count; ++i) {
            mask |= (1u << ids[i]);
        }
        g_unlock_mask.store(mask);
    }
    LogInfo("Restored custom party from GAP1 (%u): %s %s %s %s", static_cast<unsigned>(count),
            CharName(ids[0]), count > 1 ? CharName(ids[1]) : "-", count > 2 ? CharName(ids[2]) : "-",
            count > 3 ? CharName(ids[3]) : "-");
}

bool TryPartyAssetOverlay(const char* original_path, char* out_path, size_t out_size) {
    if constexpr (!kPartyCustomEnabled || !kPartyAssetRemapEnabled) {
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
    if constexpr (!kPartyCustomEnabled) {
        LogInfo("Party custom: parked");
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

    // Capture ASLR-relocated table address from the live instruction.
    std::memcpy(&g_ap_party_char_table_abs, char_site + 3, sizeof(std::uint32_t));

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

    auto* init_site = reinterpret_cast<uint8_t*>(base + kBattlePartyInitRva);
    if (init_site[0] != 0x55 || init_site[1] != 0x8B || init_site[2] != 0xEC ||
        init_site[3] != 0xA0) {
        RestoreBytes(g_count_site, g_count_original, 8);
        RestoreBytes(g_char_site, g_char_original, 7);
        g_count_site = nullptr;
        g_char_site = nullptr;
        LogWarn("Party custom: battle init site mismatch at +0x%X",
                static_cast<unsigned>(kBattlePartyInitRva));
        return false;
    }
    g_battle_party_init_trampoline_mem = MakeTrampoline(
        init_site, kBattlePartyInitPatchSize, init_site + kBattlePartyInitPatchSize);
    if (!g_battle_party_init_trampoline_mem) {
        RestoreBytes(g_count_site, g_count_original, 8);
        RestoreBytes(g_char_site, g_char_original, 7);
        g_count_site = nullptr;
        g_char_site = nullptr;
        LogWarn("Party custom: battle init trampoline alloc failed");
        return false;
    }
    g_ap_battle_party_init_tramp = g_battle_party_init_trampoline_mem;
    if (!WriteJump(init_site, reinterpret_cast<void*>(&ApBattlePartyInitDetour),
                   g_battle_party_init_original, kBattlePartyInitPatchSize)) {
        VirtualFree(g_battle_party_init_trampoline_mem, 0, MEM_RELEASE);
        g_battle_party_init_trampoline_mem = nullptr;
        g_ap_battle_party_init_tramp = nullptr;
        RestoreBytes(g_count_site, g_count_original, 8);
        RestoreBytes(g_char_site, g_char_original, 7);
        g_count_site = nullptr;
        g_char_site = nullptr;
        LogWarn("Party custom: failed to patch battle party init");
        return false;
    }
    g_battle_party_init_site = init_site;

    // Real battle ally spawn: count / force-spawn / char-id (+12C4CD / +12C4DE / +13BD7E).
    auto* ally_count_site = reinterpret_cast<uint8_t*>(base + kBattleAllyCountRva);
    auto* ally_gate_site = reinterpret_cast<uint8_t*>(base + kBattleAllySpawnGateRva);
    auto* ally_char_site = reinterpret_cast<uint8_t*>(base + kBattleAllyCharIdRva);
    if (ally_count_site[0] != 0xA1 || ally_gate_site[0] != 0x8D || ally_gate_site[1] != 0x43 ||
        ally_gate_site[2] != 0xFF || ally_char_site[0] != 0x8A || ally_char_site[1] != 0x04 ||
        ally_char_site[2] != 0x01) {
        RestoreBytes(g_battle_party_init_site, g_battle_party_init_original,
                     kBattlePartyInitPatchSize);
        VirtualFree(g_battle_party_init_trampoline_mem, 0, MEM_RELEASE);
        g_battle_party_init_trampoline_mem = nullptr;
        g_ap_battle_party_init_tramp = nullptr;
        g_battle_party_init_site = nullptr;
        RestoreBytes(g_count_site, g_count_original, 8);
        RestoreBytes(g_char_site, g_char_original, 7);
        g_count_site = nullptr;
        g_char_site = nullptr;
        LogWarn("Party custom: battle ally spawn site mismatch (+0x%X / +0x%X / +0x%X)",
                static_cast<unsigned>(kBattleAllyCountRva),
                static_cast<unsigned>(kBattleAllySpawnGateRva),
                static_cast<unsigned>(kBattleAllyCharIdRva));
        return false;
    }

    g_ap_battle_ally_count_resume = reinterpret_cast<void*>(base + kBattleAllyCountResumeRva);
    g_ap_battle_ally_gate_resume = reinterpret_cast<void*>(base + kBattleAllySpawnGateResumeRva);
    g_ap_battle_ally_spawn = reinterpret_cast<void*>(base + kBattleAllySpawnRva);
    g_ap_battle_ally_char_resume = reinterpret_cast<void*>(base + kBattleAllyCharIdResumeRva);
    g_ap_battle_playable_pack_probe_resume =
        reinterpret_cast<void*>(base + kBattlePlayablePackProbeResumeRva);
    g_ap_battle_keyed_task_enqueue_probe_resume =
        reinterpret_cast<void*>(base + kBattleKeyedTaskEnqueueProbeResumeRva);
    g_ap_battle_pack_write_probe_resume =
        reinterpret_cast<void*>(base + kBattlePackWriteProbeResumeRva);
    g_ap_battle_pack_prep_probe_resume =
        reinterpret_cast<void*>(base + kBattlePackPrepProbeResumeRva);
    g_battle_scheduler_enqueue = reinterpret_cast<void*>(base + kBattleSchedulerEnqueueRva);
    g_battle_keyed_task_callback = reinterpret_cast<void*>(base + 0x12C8B0u);
    if (!WriteJump(ally_count_site, reinterpret_cast<void*>(&ApBattleAllyCountDetour),
                   g_battle_ally_count_original, kBattleAllyCountPatchSize)) {
        RestoreBytes(g_battle_party_init_site, g_battle_party_init_original,
                     kBattlePartyInitPatchSize);
        VirtualFree(g_battle_party_init_trampoline_mem, 0, MEM_RELEASE);
        g_battle_party_init_trampoline_mem = nullptr;
        g_ap_battle_party_init_tramp = nullptr;
        g_battle_party_init_site = nullptr;
        RestoreBytes(g_count_site, g_count_original, 8);
        RestoreBytes(g_char_site, g_char_original, 7);
        g_count_site = nullptr;
        g_char_site = nullptr;
        LogWarn("Party custom: failed to patch battle ally count");
        return false;
    }
    g_battle_ally_count_site = ally_count_site;

    if (!WriteJump(ally_gate_site, reinterpret_cast<void*>(&ApBattleAllySpawnGateDetour),
                   g_battle_ally_spawn_gate_original, kBattleAllySpawnGatePatchSize)) {
        RestoreBytes(g_battle_ally_count_site, g_battle_ally_count_original,
                     kBattleAllyCountPatchSize);
        g_battle_ally_count_site = nullptr;
        RestoreBytes(g_battle_party_init_site, g_battle_party_init_original,
                     kBattlePartyInitPatchSize);
        VirtualFree(g_battle_party_init_trampoline_mem, 0, MEM_RELEASE);
        g_battle_party_init_trampoline_mem = nullptr;
        g_ap_battle_party_init_tramp = nullptr;
        g_battle_party_init_site = nullptr;
        RestoreBytes(g_count_site, g_count_original, 8);
        RestoreBytes(g_char_site, g_char_original, 7);
        g_count_site = nullptr;
        g_char_site = nullptr;
        LogWarn("Party custom: failed to patch battle ally spawn gate");
        return false;
    }
    g_battle_ally_spawn_gate_site = ally_gate_site;

    if (!WriteJump(ally_char_site, reinterpret_cast<void*>(&ApBattleAllyCharIdDetour),
                   g_battle_ally_char_original, kBattleAllyCharIdPatchSize)) {
        RestoreBytes(g_battle_ally_spawn_gate_site, g_battle_ally_spawn_gate_original,
                     kBattleAllySpawnGatePatchSize);
        g_battle_ally_spawn_gate_site = nullptr;
        RestoreBytes(g_battle_ally_count_site, g_battle_ally_count_original,
                     kBattleAllyCountPatchSize);
        g_battle_ally_count_site = nullptr;
        RestoreBytes(g_battle_party_init_site, g_battle_party_init_original,
                     kBattlePartyInitPatchSize);
        VirtualFree(g_battle_party_init_trampoline_mem, 0, MEM_RELEASE);
        g_battle_party_init_trampoline_mem = nullptr;
        g_ap_battle_party_init_tramp = nullptr;
        g_battle_party_init_site = nullptr;
        RestoreBytes(g_count_site, g_count_original, 8);
        RestoreBytes(g_char_site, g_char_original, 7);
        g_count_site = nullptr;
        g_char_site = nullptr;
        LogWarn("Party custom: failed to patch battle ally char-id");
        return false;
    }
    g_battle_ally_char_site = ally_char_site;

    auto* keyed_enqueue_probe_site =
        reinterpret_cast<uint8_t*>(base + kBattleKeyedTaskEnqueueProbeRva);
    if (keyed_enqueue_probe_site[0] != 0x68 || keyed_enqueue_probe_site[5] != 0xE8 ||
        keyed_enqueue_probe_site[10] != 0x83 || keyed_enqueue_probe_site[11] != 0xC4 ||
        keyed_enqueue_probe_site[12] != 0x08) {
        RestoreBytes(g_battle_ally_char_site, g_battle_ally_char_original,
                     kBattleAllyCharIdPatchSize);
        g_battle_ally_char_site = nullptr;
        RestoreBytes(g_battle_ally_spawn_gate_site, g_battle_ally_spawn_gate_original,
                     kBattleAllySpawnGatePatchSize);
        g_battle_ally_spawn_gate_site = nullptr;
        RestoreBytes(g_battle_ally_count_site, g_battle_ally_count_original,
                     kBattleAllyCountPatchSize);
        g_battle_ally_count_site = nullptr;
        RestoreBytes(g_battle_party_init_site, g_battle_party_init_original,
                     kBattlePartyInitPatchSize);
        VirtualFree(g_battle_party_init_trampoline_mem, 0, MEM_RELEASE);
        g_battle_party_init_trampoline_mem = nullptr;
        g_ap_battle_party_init_tramp = nullptr;
        g_battle_party_init_site = nullptr;
        RestoreBytes(g_count_site, g_count_original, 8);
        RestoreBytes(g_char_site, g_char_original, 7);
        g_count_site = nullptr;
        g_char_site = nullptr;
        LogWarn("Party custom: battle keyed-task enqueue probe site mismatch at +0x%X",
                static_cast<unsigned>(kBattleKeyedTaskEnqueueProbeRva));
        return false;
    }
    if (!WriteJump(keyed_enqueue_probe_site,
                   reinterpret_cast<void*>(&ApBattleKeyedTaskEnqueueProbeDetour),
                   g_battle_keyed_task_enqueue_probe_original,
                   kBattleKeyedTaskEnqueueProbePatchSize)) {
        RestoreBytes(g_battle_ally_char_site, g_battle_ally_char_original,
                     kBattleAllyCharIdPatchSize);
        g_battle_ally_char_site = nullptr;
        RestoreBytes(g_battle_ally_spawn_gate_site, g_battle_ally_spawn_gate_original,
                     kBattleAllySpawnGatePatchSize);
        g_battle_ally_spawn_gate_site = nullptr;
        RestoreBytes(g_battle_ally_count_site, g_battle_ally_count_original,
                     kBattleAllyCountPatchSize);
        g_battle_ally_count_site = nullptr;
        RestoreBytes(g_battle_party_init_site, g_battle_party_init_original,
                     kBattlePartyInitPatchSize);
        VirtualFree(g_battle_party_init_trampoline_mem, 0, MEM_RELEASE);
        g_battle_party_init_trampoline_mem = nullptr;
        g_ap_battle_party_init_tramp = nullptr;
        g_battle_party_init_site = nullptr;
        RestoreBytes(g_count_site, g_count_original, 8);
        RestoreBytes(g_char_site, g_char_original, 7);
        g_count_site = nullptr;
        g_char_site = nullptr;
        LogWarn("Party custom: failed to patch battle keyed-task enqueue probe");
        return false;
    }
    g_battle_keyed_task_enqueue_probe_site = keyed_enqueue_probe_site;
    auto* pack_probe_site = reinterpret_cast<uint8_t*>(base + kBattlePlayablePackProbeRva);
    if (pack_probe_site[0] != 0x8A || pack_probe_site[1] != 0x84 || pack_probe_site[2] != 0x31) {
        RestoreBytes(g_battle_ally_char_site, g_battle_ally_char_original,
                     kBattleAllyCharIdPatchSize);
        g_battle_ally_char_site = nullptr;
        RestoreBytes(g_battle_ally_spawn_gate_site, g_battle_ally_spawn_gate_original,
                     kBattleAllySpawnGatePatchSize);
        g_battle_ally_spawn_gate_site = nullptr;
        RestoreBytes(g_battle_ally_count_site, g_battle_ally_count_original,
                     kBattleAllyCountPatchSize);
        g_battle_ally_count_site = nullptr;
        RestoreBytes(g_battle_party_init_site, g_battle_party_init_original,
                     kBattlePartyInitPatchSize);
        VirtualFree(g_battle_party_init_trampoline_mem, 0, MEM_RELEASE);
        g_battle_party_init_trampoline_mem = nullptr;
        g_ap_battle_party_init_tramp = nullptr;
        g_battle_party_init_site = nullptr;
        RestoreBytes(g_count_site, g_count_original, 8);
        RestoreBytes(g_char_site, g_char_original, 7);
        g_count_site = nullptr;
        g_char_site = nullptr;
        LogWarn("Party custom: battle playable-pack probe site mismatch at +0x%X",
                static_cast<unsigned>(kBattlePlayablePackProbeRva));
        return false;
    }
    if (!WriteJump(pack_probe_site, reinterpret_cast<void*>(&ApBattlePlayablePackProbeDetour),
                   g_battle_playable_pack_probe_original, kBattlePlayablePackProbePatchSize)) {
        RestoreBytes(g_battle_ally_char_site, g_battle_ally_char_original,
                     kBattleAllyCharIdPatchSize);
        g_battle_ally_char_site = nullptr;
        RestoreBytes(g_battle_ally_spawn_gate_site, g_battle_ally_spawn_gate_original,
                     kBattleAllySpawnGatePatchSize);
        g_battle_ally_spawn_gate_site = nullptr;
        RestoreBytes(g_battle_ally_count_site, g_battle_ally_count_original,
                     kBattleAllyCountPatchSize);
        g_battle_ally_count_site = nullptr;
        RestoreBytes(g_battle_party_init_site, g_battle_party_init_original,
                     kBattlePartyInitPatchSize);
        VirtualFree(g_battle_party_init_trampoline_mem, 0, MEM_RELEASE);
        g_battle_party_init_trampoline_mem = nullptr;
        g_ap_battle_party_init_tramp = nullptr;
        g_battle_party_init_site = nullptr;
        RestoreBytes(g_count_site, g_count_original, 8);
        RestoreBytes(g_char_site, g_char_original, 7);
        g_count_site = nullptr;
        g_char_site = nullptr;
        LogWarn("Party custom: failed to patch battle playable-pack probe");
        return false;
    }
    g_battle_playable_pack_probe_site = pack_probe_site;
    auto* pack_write_site = reinterpret_cast<uint8_t*>(base + kBattlePackWriteProbeRva);
    if (pack_write_site[0] != 0x81 || pack_write_site[1] != 0xC1) {
        LogWarn("Party custom: battle c8a write probe site mismatch at +0x%X",
                static_cast<unsigned>(kBattlePackWriteProbeRva));
    } else if (!WriteJump(pack_write_site, reinterpret_cast<void*>(&ApBattlePackWriteProbeDetour),
                          g_battle_pack_write_probe_original, kBattlePackWriteProbePatchSize)) {
        LogWarn("Party custom: failed to patch battle c8a write probe");
    } else {
        g_battle_pack_write_probe_site = pack_write_site;
    }
    auto* pack_prep_site = reinterpret_cast<uint8_t*>(base + kBattlePackPrepProbeRva);
    if (pack_prep_site[0] != 0x8B || pack_prep_site[1] != 0x4D || pack_prep_site[2] != 0xE8) {
        LogWarn("Party custom: battle c8a prep probe site mismatch at +0x%X",
                static_cast<unsigned>(kBattlePackPrepProbeRva));
    } else if (!WriteJump(pack_prep_site, reinterpret_cast<void*>(&ApBattlePackPrepProbeDetour),
                          g_battle_pack_prep_probe_original, kBattlePackPrepProbePatchSize)) {
        LogWarn("Party custom: failed to patch battle c8a prep probe");
    } else {
        g_battle_pack_prep_probe_site = pack_prep_site;
    }
    LogInfo("Party battle spawn hooks @ +0x%X / +0x%X / +0x%X / +0x%X / +0x%X / +0x%X",
            static_cast<unsigned>(kBattleAllyCountRva),
            static_cast<unsigned>(kBattleAllySpawnGateRva),
            static_cast<unsigned>(kBattleAllyCharIdRva),
            static_cast<unsigned>(kBattlePlayablePackProbeRva),
            static_cast<unsigned>(kBattlePackWriteProbeRva),
            static_cast<unsigned>(kBattlePackPrepProbeRva));

    // Battle load (+12BDD0): prepare MapObj/formation before asset pack + ally spawn.
    auto* load_site = reinterpret_cast<uint8_t*>(base + kBattleLoadRva);
    if (load_site[0] == 0x55 && load_site[1] == 0x8B && load_site[2] == 0xEC) {
        g_battle_load_trampoline_mem =
            MakeTrampoline(load_site, kBattleLoadPatchSize, load_site + kBattleLoadPatchSize);
        if (g_battle_load_trampoline_mem &&
            WriteJump(load_site, reinterpret_cast<void*>(&ApBattleLoadDetour),
                      g_battle_load_original, kBattleLoadPatchSize)) {
            g_ap_battle_load_tramp = g_battle_load_trampoline_mem;
            g_battle_load_site = load_site;
            LogInfo("Party battle load hook @ +0x%X (MapObj/formation before asset pack)",
                    static_cast<unsigned>(kBattleLoadRva));
        } else {
            if (g_battle_load_trampoline_mem) {
                VirtualFree(g_battle_load_trampoline_mem, 0, MEM_RELEASE);
                g_battle_load_trampoline_mem = nullptr;
            }
            LogWarn("Party custom: failed to patch battle load");
        }
    } else {
        LogWarn("Party custom: battle load site mismatch at +0x%X",
                static_cast<unsigned>(kBattleLoadRva));
    }

    // Skip HdTextureManager::LoadContent (mode 1='win' only, 2=all).
    if constexpr (kSkipHdLoadContentMode > 0) {
        auto* hd_site = reinterpret_cast<uint8_t*>(base + kHdLoadContentRva);
        if (hd_site[0] == 0x55 && hd_site[1] == 0x8B && hd_site[2] == 0xEC &&
            hd_site[3] == 0x6A && hd_site[4] == 0xFF) {
            g_hd_load_content_trampoline_mem = MakeTrampoline(
                hd_site, kHdLoadContentPatchSize, hd_site + kHdLoadContentPatchSize);
            if (g_hd_load_content_trampoline_mem &&
                WriteJump(hd_site, reinterpret_cast<void*>(&ApHdLoadContentDetour),
                          g_hd_load_content_original, kHdLoadContentPatchSize)) {
                g_ap_hd_load_content_tramp = g_hd_load_content_trampoline_mem;
                g_hd_load_content_site = hd_site;
                LogInfo("Party: HdTextureManager::LoadContent hook @ +0x%X (skip mode=%d)",
                        static_cast<unsigned>(kHdLoadContentRva), kSkipHdLoadContentMode);
            } else {
                if (g_hd_load_content_trampoline_mem) {
                    VirtualFree(g_hd_load_content_trampoline_mem, 0, MEM_RELEASE);
                    g_hd_load_content_trampoline_mem = nullptr;
                }
                LogWarn("Party custom: failed to patch HdTextureManager::LoadContent");
            }
        } else {
            LogWarn("Party custom: HdTextureManager::LoadContent site mismatch at +0x%X",
                    static_cast<unsigned>(kHdLoadContentRva));
        }
    }

    // Model bind guard — must steal 8 bytes (full mov eax,[imm32]).
    auto* bind_site = reinterpret_cast<uint8_t*>(base + kBattleModelBindRva);
    if (bind_site[0] == 0x55 && bind_site[1] == 0x8B && bind_site[2] == 0xEC &&
        bind_site[3] == 0xA1) {
        g_battle_model_bind_trampoline_mem = MakeTrampoline(
            bind_site, kBattleModelBindPatchSize, bind_site + kBattleModelBindPatchSize);
        if (g_battle_model_bind_trampoline_mem &&
            WriteJump(bind_site, reinterpret_cast<void*>(&ApBattleModelBindDetour),
                      g_battle_model_bind_original, kBattleModelBindPatchSize)) {
            g_ap_battle_model_bind_tramp = g_battle_model_bind_trampoline_mem;
            g_battle_model_bind_site = bind_site;
            LogInfo("Party battle model-bind guard @ +0x%X (8-byte steal)",
                    static_cast<unsigned>(kBattleModelBindRva));
        } else {
            if (g_battle_model_bind_trampoline_mem) {
                VirtualFree(g_battle_model_bind_trampoline_mem, 0, MEM_RELEASE);
                g_battle_model_bind_trampoline_mem = nullptr;
            }
            LogWarn("Party custom: failed to patch model bind");
        }
    } else {
        LogWarn("Party custom: model bind site mismatch at +0x%X",
                static_cast<unsigned>(kBattleModelBindRva));
    }

    // Anim bind guard — skip when a87c bank pointer is invalid (custom char actions).
    auto* anim_site = reinterpret_cast<uint8_t*>(base + kBattleAnimBindRva);
    if (anim_site[0] == 0x55 && anim_site[1] == 0x8B && anim_site[2] == 0xEC &&
        anim_site[3] == 0x51 && anim_site[4] == 0xA1) {
        g_battle_anim_bind_trampoline_mem = MakeTrampoline(
            anim_site, kBattleAnimBindPatchSize, anim_site + kBattleAnimBindPatchSize);
        if (g_battle_anim_bind_trampoline_mem &&
            WriteJump(anim_site, reinterpret_cast<void*>(&ApBattleAnimBindDetour),
                      g_battle_anim_bind_original, kBattleAnimBindPatchSize)) {
            g_ap_battle_anim_bind_tramp = g_battle_anim_bind_trampoline_mem;
            g_battle_anim_bind_site = anim_site;
            LogInfo("Party battle anim-bind guard @ +0x%X",
                    static_cast<unsigned>(kBattleAnimBindRva));
        } else {
            if (g_battle_anim_bind_trampoline_mem) {
                VirtualFree(g_battle_anim_bind_trampoline_mem, 0, MEM_RELEASE);
                g_battle_anim_bind_trampoline_mem = nullptr;
            }
            LogWarn("Party custom: failed to patch anim bind");
        }
    } else {
        LogWarn("Party custom: anim bind site mismatch at +0x%X",
                static_cast<unsigned>(kBattleAnimBindRva));
    }

    // Menu fill: skip MapObj-derived loop and write g_ids into 70CFF8 when override ON.
    auto* menu_site = reinterpret_cast<uint8_t*>(base + kMenuFillLoopRva);
    // 8B 35 xx xx xx xx = mov esi, [70CF90]
    if (menu_site[0] == 0x8B && menu_site[1] == 0x35) {
        g_menu_fill_trampoline_mem =
            MakeTrampoline(menu_site, kMenuFillPatchSize, menu_site + kMenuFillPatchSize);
        if (g_menu_fill_trampoline_mem) {
            g_ap_menu_fill_tramp = g_menu_fill_trampoline_mem;
            g_ap_menu_fill_continue = reinterpret_cast<void*>(base + kMenuFillContinueRva);
            g_ap_menu_fill_skip = reinterpret_cast<void*>(base + kMenuFaceFinalizeRva);
            if (WriteJump(menu_site, reinterpret_cast<void*>(&ApMenuFillDetour),
                          g_menu_fill_original, kMenuFillPatchSize)) {
                g_menu_fill_site = menu_site;
                LogInfo("Party menu fill hook @ +0x%X (custom faces → +0x%X)",
                        static_cast<unsigned>(kMenuFillLoopRva),
                        static_cast<unsigned>(kMenuFaceFinalizeRva));
            } else {
                VirtualFree(g_menu_fill_trampoline_mem, 0, MEM_RELEASE);
                g_menu_fill_trampoline_mem = nullptr;
                g_ap_menu_fill_tramp = nullptr;
                LogWarn("Party custom: failed to patch menu fill");
            }
        }
    } else {
        LogWarn("Party custom: menu fill site mismatch at +0x%X",
                static_cast<unsigned>(kMenuFillLoopRva));
    }

    // Stash/shop fill twin → 707FD0.
    auto* stash_site = reinterpret_cast<uint8_t*>(base + kStashFillLoopRva);
    if (stash_site[0] == 0x8B && stash_site[1] == 0x35) {
        g_stash_fill_trampoline_mem =
            MakeTrampoline(stash_site, kMenuFillPatchSize, stash_site + kMenuFillPatchSize);
        if (g_stash_fill_trampoline_mem) {
            g_ap_stash_fill_tramp = g_stash_fill_trampoline_mem;
            g_ap_stash_fill_skip = reinterpret_cast<void*>(base + kStashFaceFinalizeRva);
            if (WriteJump(stash_site, reinterpret_cast<void*>(&ApStashFillDetour),
                          g_stash_fill_original, kMenuFillPatchSize)) {
                g_stash_fill_site = stash_site;
                LogInfo("Party stash/shop fill hook @ +0x%X → 707FD0 (faces → +0x%X)",
                        static_cast<unsigned>(kStashFillLoopRva),
                        static_cast<unsigned>(kStashFaceFinalizeRva));
            } else {
                VirtualFree(g_stash_fill_trampoline_mem, 0, MEM_RELEASE);
                g_stash_fill_trampoline_mem = nullptr;
                g_ap_stash_fill_tramp = nullptr;
                LogWarn("Party custom: failed to patch stash fill");
            }
        }
    } else {
        LogWarn("Party custom: stash fill site mismatch at +0x%X",
                static_cast<unsigned>(kStashFillLoopRva));
    }

    // ITEM assign fill twin → 709C68.
    auto* item_site = reinterpret_cast<uint8_t*>(base + kItemFillLoopRva);
    if (item_site[0] == 0x8B && item_site[1] == 0x35) {
        g_item_fill_trampoline_mem =
            MakeTrampoline(item_site, kMenuFillPatchSize, item_site + kMenuFillPatchSize);
        if (g_item_fill_trampoline_mem) {
            g_ap_item_fill_tramp = g_item_fill_trampoline_mem;
            g_ap_item_fill_skip = reinterpret_cast<void*>(base + kItemFaceFinalizeRva);
            if (WriteJump(item_site, reinterpret_cast<void*>(&ApItemFillDetour),
                          g_item_fill_original, kMenuFillPatchSize)) {
                g_item_fill_site = item_site;
                LogInfo("Party ITEM-assign fill hook @ +0x%X → 709C68 (faces → +0x%X)",
                        static_cast<unsigned>(kItemFillLoopRva),
                        static_cast<unsigned>(kItemFaceFinalizeRva));
            } else {
                VirtualFree(g_item_fill_trampoline_mem, 0, MEM_RELEASE);
                g_item_fill_trampoline_mem = nullptr;
                g_ap_item_fill_tramp = nullptr;
                LogWarn("Party custom: failed to patch ITEM fill");
            }
        }
    } else {
        LogWarn("Party custom: ITEM fill site mismatch at +0x%X",
                static_cast<unsigned>(kItemFillLoopRva));
    }

    // Bag add/remove — re-pack custom party inventories after stock ops (equip-to-Sue
    // can leave mid-list zeros that hide trailing slots in the source bag).
    auto* inv_add_site = reinterpret_cast<uint8_t*>(base + kInvAddRva);
    if (inv_add_site[0] == 0x55 && inv_add_site[1] == 0x8B && inv_add_site[2] == 0xEC) {
        g_inv_add_trampoline_mem =
            MakeTrampoline(inv_add_site, kInvOpPatchSize, inv_add_site + kInvOpPatchSize);
        if (g_inv_add_trampoline_mem) {
            g_ap_inv_add_tramp = g_inv_add_trampoline_mem;
            if (WriteJump(inv_add_site, reinterpret_cast<void*>(&ApInvAddDetour),
                          g_inv_add_original, kInvOpPatchSize)) {
                g_inv_add_site = inv_add_site;
                LogInfo("Party inv-add sanitize hook @ +0x%X", static_cast<unsigned>(kInvAddRva));
            } else {
                VirtualFree(g_inv_add_trampoline_mem, 0, MEM_RELEASE);
                g_inv_add_trampoline_mem = nullptr;
                g_ap_inv_add_tramp = nullptr;
                LogWarn("Party custom: failed to patch inv-add");
            }
        }
    } else {
        LogWarn("Party custom: inv-add site mismatch at +0x%X", static_cast<unsigned>(kInvAddRva));
    }

    auto* inv_remove_site = reinterpret_cast<uint8_t*>(base + kInvRemoveRva);
    if (inv_remove_site[0] == 0x55 && inv_remove_site[1] == 0x8B &&
        inv_remove_site[2] == 0xEC) {
        g_inv_remove_trampoline_mem =
            MakeTrampoline(inv_remove_site, kInvOpPatchSize, inv_remove_site + kInvOpPatchSize);
        if (g_inv_remove_trampoline_mem) {
            g_ap_inv_remove_tramp = g_inv_remove_trampoline_mem;
            if (WriteJump(inv_remove_site, reinterpret_cast<void*>(&ApInvRemoveDetour),
                          g_inv_remove_original, kInvOpPatchSize)) {
                g_inv_remove_site = inv_remove_site;
                LogInfo("Party inv-remove sanitize hook @ +0x%X",
                        static_cast<unsigned>(kInvRemoveRva));
            } else {
                VirtualFree(g_inv_remove_trampoline_mem, 0, MEM_RELEASE);
                g_inv_remove_trampoline_mem = nullptr;
                g_ap_inv_remove_tramp = nullptr;
                LogWarn("Party custom: failed to patch inv-remove");
            }
        }
    } else {
        LogWarn("Party custom: inv-remove site mismatch at +0x%X",
                static_cast<unsigned>(kInvRemoveRva));
    }

    // After equip-from-party remove+paint: rebuild ITEM rows before the function returns.
    auto* equip_after_remove = reinterpret_cast<uint8_t*>(base + kEquipAfterRemoveRva);
    // Expected: mov ecx, 5; jmp +1DDE97
    if (equip_after_remove[0] == 0xB9 && equip_after_remove[1] == 0x05 &&
        equip_after_remove[2] == 0x00 && equip_after_remove[3] == 0x00 &&
        equip_after_remove[4] == 0x00 && equip_after_remove[5] == 0xE9) {
        g_ap_equip_after_remove_exit = reinterpret_cast<void*>(base + kEquipAfterRemoveExitRva);
        if (WriteJump(equip_after_remove, reinterpret_cast<void*>(&ApEquipAfterRemoveDetour),
                      g_equip_after_remove_original, kEquipAfterRemovePatchSize)) {
            g_equip_after_remove_site = equip_after_remove;
            LogInfo("Party equip-after-remove rebuild hook @ +0x%X → exit +0x%X",
                    static_cast<unsigned>(kEquipAfterRemoveRva),
                    static_cast<unsigned>(kEquipAfterRemoveExitRva));
        } else {
            g_ap_equip_after_remove_exit = nullptr;
            LogWarn("Party custom: failed to patch equip-after-remove");
        }
    } else {
        LogWarn("Party custom: equip-after-remove site mismatch at +0x%X",
                static_cast<unsigned>(kEquipAfterRemoveRva));
    }

    auto install_give_wrap = [&](std::uintptr_t rva, void* detour, void** tramp_out,
                                 void** site_out, uint8_t* original_out, void** tramp_mem_out,
                                 const char* tag) {
        auto* site = reinterpret_cast<uint8_t*>(base + rva);
        if (!(site[0] == 0x55 && site[1] == 0x8B && site[2] == 0xEC)) {
            LogWarn("Party custom: %s site mismatch at +0x%X", tag, static_cast<unsigned>(rva));
            return;
        }
        void* mem = MakeTrampoline(site, kItemGiveEquipPatchSize, site + kItemGiveEquipPatchSize);
        if (!mem) {
            return;
        }
        *tramp_out = mem;
        *tramp_mem_out = mem;
        if (WriteJump(site, detour, original_out, kItemGiveEquipPatchSize)) {
            *site_out = site;
            LogInfo("Party %s wrap @ +0x%X (rebuild after return)", tag,
                    static_cast<unsigned>(rva));
        } else {
            VirtualFree(mem, 0, MEM_RELEASE);
            *tramp_out = nullptr;
            *tramp_mem_out = nullptr;
            LogWarn("Party custom: failed to patch %s", tag);
        }
    };
    install_give_wrap(kItemGiveEquipARva, reinterpret_cast<void*>(&ApItemGiveEquipADetour),
                      &g_ap_item_give_a_tramp, &g_item_give_a_site, g_item_give_a_original,
                      &g_item_give_a_trampoline_mem, "item-give-A");
    install_give_wrap(kItemGiveEquipBRva, reinterpret_cast<void*>(&ApItemGiveEquipBDetour),
                      &g_ap_item_give_b_tramp, &g_item_give_b_site, g_item_give_b_original,
                      &g_item_give_b_trampoline_mem, "item-give-B");
    install_give_wrap(kItemGiveEquipCRva, reinterpret_cast<void*>(&ApItemGiveEquipCDetour),
                      &g_ap_item_give_c_tramp, &g_item_give_c_site, g_item_give_c_original,
                      &g_item_give_c_trampoline_mem, "item-give-C");
    install_give_wrap(kItemGiveEquipDRva, reinterpret_cast<void*>(&ApItemGiveEquipDDetour),
                      &g_ap_item_give_d_tramp, &g_item_give_d_site, g_item_give_d_original,
                      &g_item_give_d_trampoline_mem, "item-give-D");
    install_give_wrap(kItemGiveEquipERva, reinterpret_cast<void*>(&ApItemGiveEquipEDetour),
                      &g_ap_item_give_e_tramp, &g_item_give_e_site, g_item_give_e_original,
                      &g_item_give_e_trampoline_mem, "item-give-E");

    // ITEM widget paint — unknown equip paths still redraw through these.
    auto* paint_site = reinterpret_cast<uint8_t*>(base + kItemPaintRva);
    if (paint_site[0] == 0x56 && paint_site[1] == 0x8B && paint_site[2] == 0xF1) {
        g_item_paint_trampoline_mem =
            MakeTrampoline(paint_site, kItemPaintPatchSize, paint_site + kItemPaintPatchSize);
        if (g_item_paint_trampoline_mem) {
            g_ap_item_paint_tramp = g_item_paint_trampoline_mem;
            if (WriteJump(paint_site, reinterpret_cast<void*>(&ApItemPaintDetour),
                          g_item_paint_original, kItemPaintPatchSize)) {
                g_item_paint_site = paint_site;
                LogInfo("Party ITEM paint rebuild hook @ +0x%X",
                        static_cast<unsigned>(kItemPaintRva));
            } else {
                VirtualFree(g_item_paint_trampoline_mem, 0, MEM_RELEASE);
                g_item_paint_trampoline_mem = nullptr;
                g_ap_item_paint_tramp = nullptr;
                LogWarn("Party custom: failed to patch ITEM paint");
            }
        }
    } else {
        LogWarn("Party custom: ITEM paint site mismatch at +0x%X",
                static_cast<unsigned>(kItemPaintRva));
    }

    auto* paint2_site = reinterpret_cast<uint8_t*>(base + kItemPaint2Rva);
    if (paint2_site[0] == 0x57 && paint2_site[1] == 0x33 && paint2_site[2] == 0xFF) {
        g_item_paint2_trampoline_mem =
            MakeTrampoline(paint2_site, kItemPaint2PatchSize, paint2_site + kItemPaint2PatchSize);
        if (g_item_paint2_trampoline_mem) {
            g_ap_item_paint2_tramp = g_item_paint2_trampoline_mem;
            if (WriteJump(paint2_site, reinterpret_cast<void*>(&ApItemPaint2Detour),
                          g_item_paint2_original, kItemPaint2PatchSize)) {
                g_item_paint2_site = paint2_site;
                LogInfo("Party ITEM paint2 rebuild hook @ +0x%X",
                        static_cast<unsigned>(kItemPaint2Rva));
            } else {
                VirtualFree(g_item_paint2_trampoline_mem, 0, MEM_RELEASE);
                g_item_paint2_trampoline_mem = nullptr;
                g_ap_item_paint2_tramp = nullptr;
                LogWarn("Party custom: failed to patch ITEM paint2");
            }
        }
    } else {
        LogWarn("Party custom: ITEM paint2 site mismatch at +0x%X",
                static_cast<unsigned>(kItemPaint2Rva));
    }

    // Face bank override: when g_ap_face_bank_override is set, +56FF0 uses private FC arena.
    // Field caller +72288 leaves override null → stock [640E6C]. Never uses +6900/+72C0.
    auto* face_bank_site = reinterpret_cast<uint8_t*>(base + kFaceBankLoadRva);
    if (face_bank_site[0] == 0x8B && face_bank_site[1] == 0x35) {
        g_ap_face_bank_slot_abs = base + kFaceBankPtrRva;
        g_ap_face_bank_resume = reinterpret_cast<void*>(base + kFaceBankLoadResumeRva);
        if (WriteJump(face_bank_site, reinterpret_cast<void*>(&ApFaceBankLoadDetour),
                      g_face_bank_original, kFaceBankLoadPatchSize)) {
            g_face_bank_site = face_bank_site;
            LogInfo("Party face bank override @ +0x%X (private FC##.DAT arenas)",
                    static_cast<unsigned>(kFaceBankLoadRva));
        } else {
            g_ap_face_bank_resume = nullptr;
            g_ap_face_bank_slot_abs = 0;
            LogWarn("Party custom: failed to patch face bank load");
        }
    } else {
        LogWarn("Party custom: face bank site mismatch at +0x%X",
                static_cast<unsigned>(kFaceBankLoadRva));
    }

    if (!g_crash_veh) {
        g_crash_veh = AddVectoredExceptionHandler(1, ApPartyCrashVeh);
    }

    g_installed = true;
    LogInfo(
        "Party hooks active — menu+stash+ITEM caches/faces + battle spawn "
        "@ +0x%X / +0x%X / +0x%X / +0x%X / +0x%X / +0x%X",
        static_cast<unsigned>(kCountLoadRva), static_cast<unsigned>(kCharIdLoadRva),
        static_cast<unsigned>(kBattleAllyCountRva), static_cast<unsigned>(kBattleAllyCharIdRva),
        static_cast<unsigned>(kMenuFillLoopRva), static_cast<unsigned>(kStashFillLoopRva));
    return true;
#endif
}

void RemovePartyCustomHook() {
    if (g_crash_veh) {
        RemoveVectoredExceptionHandler(g_crash_veh);
        g_crash_veh = nullptr;
    }
    if (g_menu_fill_site) {
        RestoreBytes(g_menu_fill_site, g_menu_fill_original, kMenuFillPatchSize);
        g_menu_fill_site = nullptr;
    }
    if (g_menu_fill_trampoline_mem) {
        VirtualFree(g_menu_fill_trampoline_mem, 0, MEM_RELEASE);
        g_menu_fill_trampoline_mem = nullptr;
    }
    if (g_stash_fill_site) {
        RestoreBytes(g_stash_fill_site, g_stash_fill_original, kMenuFillPatchSize);
        g_stash_fill_site = nullptr;
    }
    if (g_stash_fill_trampoline_mem) {
        VirtualFree(g_stash_fill_trampoline_mem, 0, MEM_RELEASE);
        g_stash_fill_trampoline_mem = nullptr;
    }
    if (g_item_fill_site) {
        RestoreBytes(g_item_fill_site, g_item_fill_original, kMenuFillPatchSize);
        g_item_fill_site = nullptr;
    }
    if (g_item_fill_trampoline_mem) {
        VirtualFree(g_item_fill_trampoline_mem, 0, MEM_RELEASE);
        g_item_fill_trampoline_mem = nullptr;
    }
    if (g_inv_add_site) {
        RestoreBytes(g_inv_add_site, g_inv_add_original, kInvOpPatchSize);
        g_inv_add_site = nullptr;
    }
    if (g_inv_add_trampoline_mem) {
        VirtualFree(g_inv_add_trampoline_mem, 0, MEM_RELEASE);
        g_inv_add_trampoline_mem = nullptr;
    }
    if (g_inv_remove_site) {
        RestoreBytes(g_inv_remove_site, g_inv_remove_original, kInvOpPatchSize);
        g_inv_remove_site = nullptr;
    }
    if (g_inv_remove_trampoline_mem) {
        VirtualFree(g_inv_remove_trampoline_mem, 0, MEM_RELEASE);
        g_inv_remove_trampoline_mem = nullptr;
    }
    if (g_equip_after_remove_site) {
        RestoreBytes(g_equip_after_remove_site, g_equip_after_remove_original,
                     kEquipAfterRemovePatchSize);
        g_equip_after_remove_site = nullptr;
    }
    if (g_item_give_a_site) {
        RestoreBytes(g_item_give_a_site, g_item_give_a_original, kItemGiveEquipPatchSize);
        g_item_give_a_site = nullptr;
    }
    if (g_item_give_a_trampoline_mem) {
        VirtualFree(g_item_give_a_trampoline_mem, 0, MEM_RELEASE);
        g_item_give_a_trampoline_mem = nullptr;
    }
    if (g_item_give_b_site) {
        RestoreBytes(g_item_give_b_site, g_item_give_b_original, kItemGiveEquipPatchSize);
        g_item_give_b_site = nullptr;
    }
    if (g_item_give_b_trampoline_mem) {
        VirtualFree(g_item_give_b_trampoline_mem, 0, MEM_RELEASE);
        g_item_give_b_trampoline_mem = nullptr;
    }
    if (g_item_give_c_site) {
        RestoreBytes(g_item_give_c_site, g_item_give_c_original, kItemGiveEquipPatchSize);
        g_item_give_c_site = nullptr;
    }
    if (g_item_give_c_trampoline_mem) {
        VirtualFree(g_item_give_c_trampoline_mem, 0, MEM_RELEASE);
        g_item_give_c_trampoline_mem = nullptr;
    }
    if (g_item_give_d_site) {
        RestoreBytes(g_item_give_d_site, g_item_give_d_original, kItemGiveEquipPatchSize);
        g_item_give_d_site = nullptr;
    }
    if (g_item_give_d_trampoline_mem) {
        VirtualFree(g_item_give_d_trampoline_mem, 0, MEM_RELEASE);
        g_item_give_d_trampoline_mem = nullptr;
    }
    if (g_item_give_e_site) {
        RestoreBytes(g_item_give_e_site, g_item_give_e_original, kItemGiveEquipPatchSize);
        g_item_give_e_site = nullptr;
    }
    if (g_item_give_e_trampoline_mem) {
        VirtualFree(g_item_give_e_trampoline_mem, 0, MEM_RELEASE);
        g_item_give_e_trampoline_mem = nullptr;
    }
    if (g_item_paint_site) {
        RestoreBytes(g_item_paint_site, g_item_paint_original, kItemPaintPatchSize);
        g_item_paint_site = nullptr;
    }
    if (g_item_paint_trampoline_mem) {
        VirtualFree(g_item_paint_trampoline_mem, 0, MEM_RELEASE);
        g_item_paint_trampoline_mem = nullptr;
    }
    if (g_item_paint2_site) {
        RestoreBytes(g_item_paint2_site, g_item_paint2_original, kItemPaint2PatchSize);
        g_item_paint2_site = nullptr;
    }
    if (g_item_paint2_trampoline_mem) {
        VirtualFree(g_item_paint2_trampoline_mem, 0, MEM_RELEASE);
        g_item_paint2_trampoline_mem = nullptr;
    }
    if (g_face_bank_site) {
        RestoreBytes(g_face_bank_site, g_face_bank_original, kFaceBankLoadPatchSize);
        g_face_bank_site = nullptr;
    }
    FreeFcBanks();
#if defined(_M_IX86)
    g_ap_face_bank_override = nullptr;
#endif
    if (g_battle_ally_char_site) {
        RestoreBytes(g_battle_ally_char_site, g_battle_ally_char_original,
                     kBattleAllyCharIdPatchSize);
        g_battle_ally_char_site = nullptr;
    }
    if (g_battle_playable_pack_probe_site) {
        RestoreBytes(g_battle_playable_pack_probe_site, g_battle_playable_pack_probe_original,
                     kBattlePlayablePackProbePatchSize);
        g_battle_playable_pack_probe_site = nullptr;
    }
    if (g_battle_pack_write_probe_site) {
        RestoreBytes(g_battle_pack_write_probe_site, g_battle_pack_write_probe_original,
                     kBattlePackWriteProbePatchSize);
        g_battle_pack_write_probe_site = nullptr;
    }
    if (g_battle_pack_prep_probe_site) {
        RestoreBytes(g_battle_pack_prep_probe_site, g_battle_pack_prep_probe_original,
                     kBattlePackPrepProbePatchSize);
        g_battle_pack_prep_probe_site = nullptr;
    }
    if (g_battle_ally_spawn_gate_site) {
        RestoreBytes(g_battle_ally_spawn_gate_site, g_battle_ally_spawn_gate_original,
                     kBattleAllySpawnGatePatchSize);
        g_battle_ally_spawn_gate_site = nullptr;
    }
    if (g_battle_ally_count_site) {
        RestoreBytes(g_battle_ally_count_site, g_battle_ally_count_original,
                     kBattleAllyCountPatchSize);
        g_battle_ally_count_site = nullptr;
    }
    if (g_battle_load_site) {
        RestoreBytes(g_battle_load_site, g_battle_load_original, kBattleLoadPatchSize);
        g_battle_load_site = nullptr;
    }
    if (g_battle_load_trampoline_mem) {
        VirtualFree(g_battle_load_trampoline_mem, 0, MEM_RELEASE);
        g_battle_load_trampoline_mem = nullptr;
    }
    if (g_hd_load_content_site) {
        RestoreBytes(g_hd_load_content_site, g_hd_load_content_original, kHdLoadContentPatchSize);
        g_hd_load_content_site = nullptr;
    }
    if (g_hd_load_content_trampoline_mem) {
        VirtualFree(g_hd_load_content_trampoline_mem, 0, MEM_RELEASE);
        g_hd_load_content_trampoline_mem = nullptr;
    }
    if (g_battle_model_bind_site) {
        RestoreBytes(g_battle_model_bind_site, g_battle_model_bind_original,
                     kBattleModelBindPatchSize);
        g_battle_model_bind_site = nullptr;
    }
    if (g_battle_model_bind_trampoline_mem) {
        VirtualFree(g_battle_model_bind_trampoline_mem, 0, MEM_RELEASE);
        g_battle_model_bind_trampoline_mem = nullptr;
    }
    if (g_battle_anim_bind_site) {
        RestoreBytes(g_battle_anim_bind_site, g_battle_anim_bind_original,
                     kBattleAnimBindPatchSize);
        g_battle_anim_bind_site = nullptr;
    }
    if (g_battle_anim_bind_trampoline_mem) {
        VirtualFree(g_battle_anim_bind_trampoline_mem, 0, MEM_RELEASE);
        g_battle_anim_bind_trampoline_mem = nullptr;
    }
    if (g_battle_fa90_js_site) {
        RestoreBytes(g_battle_fa90_js_site, g_battle_fa90_js_original, kBattleFa90EarlyExitJsSize);
        g_battle_fa90_js_site = nullptr;
    }
    if (g_battle_ally_gate_site) {
        RestoreBytes(g_battle_ally_gate_site, g_battle_ally_gate_original,
                     sizeof(g_battle_ally_gate_original));
        g_battle_ally_gate_site = nullptr;
    }
    if (g_battle_party_init_site) {
        RestoreBytes(g_battle_party_init_site, g_battle_party_init_original,
                     kBattlePartyInitPatchSize);
        g_battle_party_init_site = nullptr;
    }
    if (g_battle_party_init_trampoline_mem) {
        VirtualFree(g_battle_party_init_trampoline_mem, 0, MEM_RELEASE);
        g_battle_party_init_trampoline_mem = nullptr;
    }
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
    g_ap_party_char_table_abs = 0;
    g_ap_battle_party_init_tramp = nullptr;
    g_ap_menu_fill_tramp = nullptr;
    g_ap_menu_fill_continue = nullptr;
    g_ap_menu_fill_skip = nullptr;
    g_ap_stash_fill_tramp = nullptr;
    g_ap_stash_fill_skip = nullptr;
    g_ap_item_fill_tramp = nullptr;
    g_ap_item_fill_skip = nullptr;
    g_ap_inv_add_tramp = nullptr;
    g_ap_inv_remove_tramp = nullptr;
    g_ap_inv_add_ret = nullptr;
    g_ap_inv_remove_ret = nullptr;
    g_ap_equip_after_remove_exit = nullptr;
    g_ap_item_give_a_tramp = nullptr;
    g_ap_item_give_b_tramp = nullptr;
    g_ap_item_give_c_tramp = nullptr;
    g_ap_item_give_d_tramp = nullptr;
    g_ap_item_give_e_tramp = nullptr;
    g_ap_item_give_a_ret = nullptr;
    g_ap_item_give_b_ret = nullptr;
    g_ap_item_give_c_ret = nullptr;
    g_ap_item_give_d_ret = nullptr;
    g_ap_item_give_e_ret = nullptr;
    g_ap_item_paint_tramp = nullptr;
    g_ap_item_paint2_tramp = nullptr;
    g_ap_face_bank_override = nullptr;
    g_ap_face_bank_resume = nullptr;
    g_ap_face_bank_slot_abs = 0;
    g_ap_battle_ally_count_resume = nullptr;
    g_ap_battle_ally_gate_resume = nullptr;
    g_ap_battle_ally_spawn = nullptr;
    g_ap_battle_ally_char_resume = nullptr;
    g_ap_battle_playable_pack_probe_resume = nullptr;
    g_ap_battle_pack_write_probe_resume = nullptr;
    g_ap_battle_pack_prep_probe_resume = nullptr;
    g_ap_battle_load_tramp = nullptr;
    g_ap_battle_model_bind_tramp = nullptr;
    g_ap_battle_anim_bind_tramp = nullptr;
    g_ap_hd_load_content_tramp = nullptr;
#endif
    g_battle_custom_spawn_done = false;
    g_battle_party_init_synced = false;
    g_field0a_saved_for_battle = false;
    g_field_actors_saved = false;
    g_battle_roster_applied = false;
    g_battle_assets_prepared = false;
    g_last_battle_mode = 0xFFu;
    g_enabled.store(false);
    g_asset_group.store(0xFFu);
    g_installed = false;
}

bool IsPartyCustomHookInstalled() { return g_installed; }

void FormatPartyRosterLine(char* buf, size_t buf_size) {
    if (!buf || buf_size == 0) {
        return;
    }
    FormatRoster(buf, buf_size, "Roster");
    const uint8_t n = g_count.load();
    if (n > 0 && g_selected_slot < n && buf_size > 8) {
        char extra[96];
        std::snprintf(extra, sizeof(extra), "  [slot %u=%s]",
                      static_cast<unsigned>(g_selected_slot + 1), CharName(g_ids[g_selected_slot]));
        const size_t used = std::strlen(buf);
        if (used + std::strlen(extra) + 1 < buf_size) {
            std::memcpy(buf + used, extra, std::strlen(extra) + 1);
        }
    }
}

const char* PartyCharName(uint8_t id) { return CharName(id); }

uint8_t PartyEditCount() { return g_count.load(); }

uint8_t PartyEditIdAt(uint8_t slot) {
    if (slot >= g_count.load() || slot >= 4) {
        return 0;
    }
    return g_ids[slot];
}

uint8_t PartyEditSelectedSlot() { return g_selected_slot; }

void PartyEditSetSelectedSlot(uint8_t slot) {
    g_selected_slot = slot;
    ClampSelectedSlot();
}

void PartyUiEnsureEnabled() {
    if (!IsPartyOverlayEnabled()) {
        return;
    }
    if (!g_enabled.load()) {
        SetPartyCustomEnabled(true);
    }
}

void PartyUiCycleSlot() {
    PartyUiEnsureEnabled();
    CycleSelectedSlot();
    ToastParty("Party");
}

void PartyUiCycleChar(int dir) {
    PartyUiEnsureEnabled();
    CycleCharInSelectedSlot(dir, true);
}

void PartyUiAddMember() {
    PartyUiEnsureEnabled();
    AddPartyMember(true);
}

void PartyUiRemoveMember() {
    PartyUiEnsureEnabled();
    RemovePartyMember(true);
}

bool PartyUiApply() {
    PartyUiEnsureEnabled();
    if (!TriggerPartyApply()) {
        ShowD3dOverlayToast("Apply failed (need field map loaded)", 2500, 0xFA8072u);
        return false;
    }
    ToastParty("Applied");
    return true;
}

void PartyUiCycleCharQuiet(int dir) {
    PartyUiEnsureEnabled();
    CycleCharInSelectedSlot(dir, false);
}

void PartyUiAddMemberQuiet() {
    PartyUiEnsureEnabled();
    AddPartyMember(false);
}

void PartyUiRemoveMemberQuiet() {
    PartyUiEnsureEnabled();
    RemovePartyMember(false);
}

bool PartyUiApplyQuiet() {
    PartyUiEnsureEnabled();
    return TriggerPartyApply();
}

void PollPartyCustomHotkey() {
    if constexpr (!kPartyCustomEnabled) {
        return;
    }
    if (!g_installed) {
        return;
    }

    const std::uintptr_t base = grandia_ap::GetGrandiaModuleBase();
    if (base != 0) {
        PollBattleMode(base);
    }
    // F9/F10/F11 removed — edit via Save MC Party tab.
}

void PollPartyInventoryUiFix() {
    if constexpr (!kPartyCustomEnabled) {
        return;
    }
    if (!g_installed || !g_enabled.load() || g_in_item_ui_fix) {
        return;
    }

    static DWORD last_tick = 0;
    const DWORD now = GetTickCount();
    // ~5 Hz bag pack only — do NOT call +1DD6B0 from Present. Field "who gets this
    // item" UIs share widget words with menu ITEM and crash if we rebuild there.
    if (now - last_tick < 200u) {
        return;
    }

    const std::uintptr_t base = GetGrandiaModuleBase();
    if (base == 0) {
        return;
    }
    uint8_t* map_obj = MapObject(base);
    if (!map_obj) {
        return;
    }

    last_tick = now;
    const int holes = SanitizeCustomPartyInventories(map_obj);
    if (holes > 0) {
        static int hole_logs_left = 8;
        if (hole_logs_left > 0) {
            --hole_logs_left;
            LogInfo("Party: Present-poll compacted %d bag(s)", holes);
        }
    }
}

}  // namespace grandia_ap
