/* clang -std=gnu17 -I include libs/network/tests/test_trophy_registration.c
 * libs/system/cellSysutil.c -Wl,-dead_strip -pthread -o /tmp/test_trophy_registration
 * (macOS: add -framework CoreFoundation for cellSysutil's CFLocale use)
 */
#include "../sceNpTrophy.c"
#include "../trophy_files.c"
#include <assert.h>
#include <unistd.h>
uint8_t* vm_base;
uint32_t ppu_vm_size;
int g_resv_store_active;
uint32_t g_ww_lo, g_ww_hi;
void ppu_resv_break_store(uint64_t ea) { (void)ea; }
void ps3_ww_report_inline(uint32_t a, uint64_t v, int w) { (void)a; (void)v; (void)w; }
int spu_coh_is_reserved(uint32_t a) { (void)a; return 0; }
void spu_coh_notify_write(uint32_t a) { (void)a; }
void spu_lockline_lock(void) {}
void spu_lockline_unlock(void) {}
void cellMsgDialog_tick(void) {}
void np_psnr_pump(void) {}
void ps3_np_eula_tick(void) {}
#include "trophy_fixture.h"

static unsigned sync_calls;
static u32 sync_status;
static s32 sync_result;
int ppu_guest_call_ct_checked(uint32_t code, uint32_t toc, uint64_t a0, uint64_t a1, uint64_t a2,
                              uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6, uint64_t a7,
                              uint64_t* result)
{
    assert(code == 0x1234 && toc == 0x5678 && a0 == 1 && a4 == 0xCAFE);
    assert(!a2 && !a3 && !a5 && !a6 && !a7);
    sync_status = (u32)a1;
    sync_calls++;
    *result = (uint64_t)(int64_t)sync_result;
    return 1;
}

static unsigned events;
static u32 last_status, last_completed, last_total;
static void callback(u32 opd, u64 context, u64 status, u64 completed, u64 total,
                     u64 arg, u64 a, u64 b, u64 c)
{
    assert(opd == 0x300 && context == 1 && arg == 0xCAFE && !a && !b && !c);
    assert(status >= last_status);
    last_status = (u32)status; last_completed = (u32)completed; last_total = (u32)total;
    events++;
}

int main(void)
{
    snprintf(g_fixture_root, sizeof g_fixture_root, "/tmp/ps3recomp-trophy-reg-XXXXXX");
    assert(mkdtemp(g_fixture_root));
    vm_base = calloc(1, 65536); assert(vm_base);
    strcpy((char*)vm_base + 0x400, g_fixture_root);
    sceNpTrophySetStoragePath((void*)0x400);
    memcpy(vm_base + 0x100, "NPWR00000", 9);
    fixture_guest_sign(0x180);
    vm_write32(0x300, 0x1234); vm_write32(0x304, 0x5678);
    g_ps3_guest_caller = callback;

    assert(sceNpTrophyInit(NULL, 0, 0, 0) == CELL_OK);
    assert(sceNpTrophyCreateContext((void*)0x200, (void*)0x100, (void*)0x180, 0) == CELL_OK);
    assert(sceNpTrophyCreateHandle((void*)0x204) == CELL_OK);
    assert(sceNpTrophyRegisterContext(1, 1, 0x300, 0xCAFE, 0) == SCE_NP_TROPHY_ERROR_CONF_DOES_NOT_EXIST);
    assert(!sync_calls);

    fixture_write_pack();
    sync_result = -1;
    assert(sceNpTrophyRegisterContext(1, 1, 0x300, 0xCAFE, 0) == SCE_NP_TROPHY_ERROR_PROCESSING_ABORTED);
    assert(sync_calls == 1 && sync_status == SCE_NP_TROPHY_STATUS_NOT_INSTALLED && !s_contexts[1].registered);

    sync_result = 0;
    assert(sceNpTrophyRegisterContext(1, 1, 0x300, 0xCAFE, 0) == CELL_OK);
    assert(sync_calls == 2 && sync_status == SCE_NP_TROPHY_STATUS_NOT_INSTALLED && s_contexts[1].registered);
    cellSysutilCheckCallback();
    assert(events == 4 + 3 + 5 + 1);
    assert(last_status == SCE_NP_TROPHY_STATUS_PROCESSING_COMPLETE && !last_completed && !last_total);

    assert(sceNpTrophyGetGameInfo(1, 1, (void*)0x1000, (void*)0x2000) == CELL_OK);
    assert(vm_read32(0x1000) == 3 && vm_read32(0x1004) == 1 && vm_read32(0x1008) == 1);
    assert(vm_read32(0x100C) == 0 && vm_read32(0x1010) == 1);
    assert(!strcmp((char*)vm_base + 0x1014, "Fixture & Co"));
    assert(!strcmp((char*)vm_base + 0x1014 + SCE_NP_TROPHY_GAME_TITLE_MAX_SIZE, "Trophies for the fixture."));
    assert(vm_read32(0x2000) == 0);

    assert(sceNpTrophyGetTrophyInfo(1, 1, 2, (void*)0x3000, NULL) == SCE_NP_TROPHY_ERROR_HIDDEN);
    assert(sceNpTrophyGetTrophyInfo(1, 1, 3, (void*)0x3000, NULL) == SCE_NP_TROPHY_ERROR_INVALID_TROPHY_ID);
    assert(sceNpTrophyGetTrophyInfo(1, 1, 1, (void*)0x3000, (void*)0x3800) == CELL_OK);
    assert(vm_read32(0x3000) == 1 && vm_read32(0x3004) == SCE_NP_TROPHY_GRADE_BRONZE);
    assert(!strcmp((char*)vm_base + 0x3008, "First"));
    assert(!strcmp((char*)vm_base + 0x3008 + SCE_NP_TROPHY_NAME_MAX_SIZE, "Start."));
    assert(vm_read8(0x3008 + SCE_NP_TROPHY_NAME_MAX_SIZE + SCE_NP_TROPHY_DESC_MAX_SIZE) == 0);
    assert(vm_read64(0x3800) == 0 && vm_read32(0x3808) == 1 && vm_read8(0x380C) == 0);

    assert(sceNpTrophyUnlockTrophy(1, 1, 0, (void*)0x4000) == SCE_NP_TROPHY_ERROR_CANNOT_UNLOCK_PLATINUM);
    assert(sceNpTrophyUnlockTrophy(1, 1, 1, (void*)0x4000) == CELL_OK);
    assert(vm_read32(0x4000) == 0xFFFFFFFFu);
    assert(sceNpTrophyUnlockTrophy(1, 1, 1, (void*)0x4000) == SCE_NP_TROPHY_ERROR_ALREADY_UNLOCKED);
    assert(sceNpTrophyUnlockTrophy(1, 1, 2, (void*)0x4000) == CELL_OK);
    assert(vm_read32(0x4000) == 0);
    assert(sceNpTrophyGetTrophyUnlockState(1, 1, (void*)0x5000, (void*)0x5010) == CELL_OK);
    assert(vm_read32(0x5010) == 3 && vm_read32(0x5000) == 7);
    assert(sceNpTrophyGetGameProgress(1, 1, (void*)0x5020) == CELL_OK && vm_read32(0x5020) == 100);
    assert(sceNpTrophyGetTrophyInfo(1, 1, 2, NULL, (void*)0x3800) == CELL_OK);
    assert(vm_read64(0x3800) > 62135596800000000ull && vm_read8(0x380C) == 1);

    assert(sceNpTrophyTerm() == CELL_OK);
    assert(sceNpTrophyInit(NULL, 0, 0, 0) == CELL_OK);
    assert(sceNpTrophyCreateContext((void*)0x200, (void*)0x100, (void*)0x180, 0) == CELL_OK);
    assert(sceNpTrophyCreateHandle((void*)0x204) == CELL_OK);
    assert(sceNpTrophyRegisterContext(1, 1, 0x300, 0xCAFE, 0) == CELL_OK);
    assert(sync_status == SCE_NP_TROPHY_STATUS_INSTALLED);
    assert(sceNpTrophyGetGameInfo(1, 1, NULL, (void*)0x2000) == CELL_OK);
    assert(vm_read32(0x2000) == 3 && vm_read32(0x2004) == 1 && vm_read32(0x2008) == 1 && vm_read32(0x2010) == 1);
    assert(sceNpTrophyTerm() == CELL_OK);

    char file[512]; snprintf(file, sizeof(file), "%s/NPWR00000_00.json", g_fixture_root);
    assert(unlink(file) == 0);
    fixture_remove_pack();
    assert(rmdir(g_fixture_root) == 0);
    free(vm_base);
    puts("Trophy registration, game/trophy info, unlocks and platinum follow the pack");
}
