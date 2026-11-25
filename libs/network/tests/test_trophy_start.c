/* clang -std=gnu17 -I include libs/network/tests/test_trophy_start.c
 *   -Wl,-dead_strip -o /tmp/test_trophy_start && /tmp/test_trophy_start
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
ps3_guest_caller_fn g_ps3_guest_caller;
s32 ps3_sysutil_language(void) { return 1; }
s32 cellSysutilQueueGuestCallbackArgs(u32 opd, const u64 args[8]) { (void)opd; (void)args; return CELL_OK; }
int ppu_guest_call_ct_checked(uint32_t code, uint32_t toc, uint64_t a0, uint64_t a1, uint64_t a2,
                              uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6, uint64_t a7,
                              uint64_t* result)
{
    (void)code; (void)toc; (void)a0; (void)a1; (void)a2; (void)a3; (void)a4; (void)a5; (void)a6; (void)a7;
    *result = 0;
    return 1;
}
#include "trophy_fixture.h"
int main(void)
{
    snprintf(g_fixture_root, sizeof g_fixture_root, "/tmp/ps3recomp-trophy-start-XXXXXX");
    assert(mkdtemp(g_fixture_root));
    const uint64_t space = fixture_write_pack();
    vm_base = calloc(1, 65536);
    assert(vm_base);
    memcpy(vm_base + 0x100, "NPWR00000", 9);
    fixture_guest_sign(0x180);
    assert(sceNpTrophyCreateContext((void*)0x200, (void*)0x100, NULL, 0) ==
           SCE_NP_TROPHY_ERROR_INVALID_ARGUMENT);
    assert(sceNpTrophyCreateContext((void*)0x200, (void*)0x100, (void*)0x180, 0) ==
           SCE_NP_TROPHY_ERROR_NOT_INITIALIZED);
    assert(sceNpTrophyInit(NULL, 0, 0, 0) == CELL_OK);
    assert(sceNpTrophyCreateContext(NULL, (void*)0x100, (void*)0x180, 0) ==
           SCE_NP_TROPHY_ERROR_INVALID_ARGUMENT);
    vm_base[0x180] ^= 1;
    assert(sceNpTrophyCreateContext((void*)0x200, (void*)0x100, (void*)0x180, 0) ==
           SCE_NP_TROPHY_ERROR_INVALID_NP_COMM_ID);
    vm_base[0x180] ^= 1;
    for (int i = 0; i < SCE_NP_TROPHY_MAX_CONTEXTS; i++) {
        assert(sceNpTrophyCreateContext((void*)0x200, (void*)0x100, (void*)0x180, 0) == CELL_OK);
        assert(vm_read32(0x200) == (u32)(i + 1));
        assert(sceNpTrophyCreateHandle((void*)0x204) == CELL_OK);
        assert(vm_read32(0x204) == (u32)(i + 1));
    }
    assert(!strcmp(s_contexts[1].name, "NPWR00000_00"));
    assert(sceNpTrophyCreateContext((void*)0x200, (void*)0x100, (void*)0x180, 0) == SCE_NP_TROPHY_ERROR_EXCEEDS_MAX);
    assert(sceNpTrophyCreateHandle((void*)0x204) == SCE_NP_TROPHY_ERROR_EXCEEDS_MAX);
    assert(sceNpTrophyDestroyContext(0) == SCE_NP_TROPHY_ERROR_INVALID_ARGUMENT);
    assert(sceNpTrophyDestroyHandle(0) == SCE_NP_TROPHY_ERROR_INVALID_ARGUMENT);
    assert(sceNpTrophyInit(NULL, 0, 0, 0) == SCE_NP_TROPHY_ERROR_ALREADY_INITIALIZED);
    strcpy((char*)vm_base + 0x400, g_fixture_root);
    sceNpTrophySetStoragePath((void*)0x400);
    assert(sceNpTrophyGetRequiredDiskSpace(1, 1, (void*)0x208, 0) == CELL_OK);
    assert(vm_read64(0x208) == space);
    assert(sceNpTrophyGetGameInfo(1, 1, (void*)0x1000, NULL) == SCE_NP_TROPHY_ERROR_CONTEXT_NOT_REGISTERED);
    assert(sceNpTrophyDestroyContext(SCE_NP_TROPHY_MAX_CONTEXTS) == CELL_OK);
    assert(sceNpTrophyDestroyContext(SCE_NP_TROPHY_MAX_CONTEXTS) == SCE_NP_TROPHY_ERROR_UNKNOWN_CONTEXT);
    assert(sceNpTrophyDestroyHandle(SCE_NP_TROPHY_MAX_HANDLES) == CELL_OK);
    assert(sceNpTrophyDestroyHandle(SCE_NP_TROPHY_MAX_HANDLES) == SCE_NP_TROPHY_ERROR_UNKNOWN_HANDLE);
    assert(s_contexts[1].in_use && s_handles[1].in_use);
    assert(sceNpTrophyTerm() == CELL_OK);
    fixture_remove_pack();
    assert(rmdir(g_fixture_root) == 0);
    free(vm_base);
    puts("Trophy initialization, validation and required-space checks passed");
}
