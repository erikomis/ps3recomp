/* The SPURS kernel's workload selection (SPURS_RPCS3_SCHED=1) through the real
 * cellSpurs.c, on a synthetic SPURS instance in guest memory: spurs_select_wid
 * and the answer a policy module's poll gets (spurs_poll_select). */
#include "libs/spurs/cellSpurs.c"
#include <assert.h>
uint8_t* vm_base;
uint32_t ppu_vm_size;
/* No SPU runs in this fixture; observer-only services are inert. */
int g_resv_store_active;
uint32_t g_ww_lo, g_ww_hi;
void ppu_resv_break_store(uint64_t ea) {(void)ea;}
int spu_coh_is_reserved(uint32_t ea) {(void)ea; return 0;}
void spu_lockline_lock(void) {}
void spu_lockline_unlock(void) {}
void spu_coh_notify_write(uint32_t ea) {(void)ea;}
void ps3_ww_report_inline(uint32_t ea,uint64_t value,int width) {(void)ea;(void)value;(void)width;}

static _Alignas(128) uint8_t memory[0x4000];
static const u32 EA = 0x1000;
static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; fprintf(stderr, "FAIL line %d: %s: ", __LINE__, #c); \
    fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); } } while (0)

static void reset(void) { memset(memory + EA, 0, 0x1000); }
/* A runnable workload: enabled, state 2 (running), the given priority on
 * every SPU (0 = never on this SPU), max contention 8. */
static void wkl(u32 wid, u32 prio)
{
    vm_write32(EA + SPURS_WKL_ENABLED, vm_read32(EA + SPURS_WKL_ENABLED) | (0x80000000u >> wid));
    memory[EA + SPURS_WKL_STATE1 + wid] = 2;
    memory[EA + SPURS_WKL_MAXCONT + wid] = 8;
    for (u32 sn = 0; sn < 8; sn++) memory[EA + SPURS_WKL_INFO1 + wid * SPURS_WKL_INFO_SZ + 0x18 + sn] = (u8)prio;
}
static void ready(u32 wid, u32 n) { memory[EA + SPURS_WKL_READY1 + wid] = (u8)n; }
static void cont(u32 wid, u32 n)  { memory[EA + SPURS_WKL_CURCONT + wid] = (u8)n; }
static void signal_wkl(u32 wid)      { vm_write16(EA + SPURS_WKL_SIGNAL1, (u16)(vm_read16(EA + SPURS_WKL_SIGNAL1) | (0x8000u >> wid))); }

int main(void)
{
    vm_base = memory; ppu_vm_size = sizeof(memory);
    int w;

    /* Nothing wants an SPU: no selection, and a poll is told 0x20, not wid 0. */
    reset(); wkl(0, 1); wkl(3, 1);
    CHECK((w = spurs_select_wid(EA, 0, -1)) == -1, "got %d", w);
    CHECK(spurs_poll_select(EA, 0, 3) == 0x20, "poll answer %u", spurs_poll_select(EA, 0, 3));

    /* readyCount above contention makes a workload a candidate; readyCount is
     * not consumed by selecting it. */
    reset(); wkl(0, 1); wkl(3, 1); ready(3, 1);
    CHECK((w = spurs_select_wid(EA, 0, -1)) == 3, "got %d", w);
    CHECK(memory[EA + SPURS_WKL_READY1 + 3] == 1, "readyCount changed");
    cont(3, 1);                                   /* its one SPU is taken */
    CHECK((w = spurs_select_wid(EA, 1, -1)) == -1, "got %d", w);
    /* ... but the SPU it runs on does not count against it when it polls. */
    CHECK((w = spurs_select_wid(EA, 0, 3)) == 3, "got %d", w);
    CHECK(spurs_poll_select(EA, 0, 3) == 3, "poll answer %u", spurs_poll_select(EA, 0, 3));

    /* A signal makes it a candidate with readyCount 0. */
    reset(); wkl(5, 4); signal_wkl(5);
    CHECK((w = spurs_select_wid(EA, 0, -1)) == 5, "got %d", w);

    /* Priority: 1 is highest, 15 lowest; 0 means not on this SPU. */
    reset(); wkl(1, 9); wkl(2, 2); ready(1, 2); ready(2, 2);
    CHECK((w = spurs_select_wid(EA, 0, -1)) == 2, "got %d", w);
    memory[EA + SPURS_WKL_INFO1 + 2 * SPURS_WKL_INFO_SZ + 0x18 + 1] = 0;   /* wid 2 not on SPU 1 */
    CHECK((w = spurs_select_wid(EA, 1, -1)) == 1, "got %d", w);
    /* ... and a better-priority workload makes the polling one yield. */
    CHECK(spurs_poll_select(EA, 0, 1) == 2, "poll answer %u", spurs_poll_select(EA, 0, 1));

    /* Max contention reached: not a candidate. */
    reset(); wkl(4, 1); ready(4, 8); cont(4, 2); memory[EA + SPURS_WKL_MAXCONT + 4] = 2;
    CHECK((w = spurs_select_wid(EA, 0, -1)) == -1, "got %d", w);

    /* Disabled, or not in the running state: not a candidate. */
    reset(); wkl(6, 1); ready(6, 1); memory[EA + SPURS_WKL_STATE1 + 6] = 1;
    CHECK((w = spurs_select_wid(EA, 0, -1)) == -1, "got %d", w);
    reset(); wkl(6, 1); ready(6, 1); vm_write32(EA + SPURS_WKL_ENABLED, 0);
    CHECK((w = spurs_select_wid(EA, 0, -1)) == -1, "got %d", w);

    /* Equal priority: the polling workload keeps its SPU ... */
    reset(); wkl(7, 3); wkl(8, 3); ready(7, 4); ready(8, 4); cont(7, 1);
    CHECK((w = spurs_select_wid(EA, 0, 7)) == 7, "got %d", w);
    CHECK(spurs_poll_select(EA, 0, 7) == 7, "poll answer %u", spurs_poll_select(EA, 0, 7));
    /* ... a free SPU takes the one below its minimum contention ... */
    reset(); wkl(7, 3); wkl(8, 3); ready(7, 4); ready(8, 4); cont(7, 1); cont(8, 1);
    memory[EA + SPURS_WKL_MINCONT + 8] = 2;
    CHECK((w = spurs_select_wid(EA, 0, -1)) == 8, "got %d", w);
    /* ... and otherwise the one on fewer SPUs. */
    reset(); wkl(7, 3); wkl(8, 3); ready(7, 4); ready(8, 4); cont(7, 2); cont(8, 1);
    CHECK((w = spurs_select_wid(EA, 0, -1)) == 8, "got %d", w);

    if (fails) { fprintf(stderr, "%d check(s) failed\n", fails); return 1; }
    puts("PASS: SPURS kernel workload selection on a synthetic instance");
    return 0;
}
