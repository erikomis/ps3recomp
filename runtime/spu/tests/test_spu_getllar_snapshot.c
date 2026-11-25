#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint8_t g_guest_mem[1u << 20];
uint8_t* vm_base = g_guest_mem;
uint32_t ppu_vm_size = 0;

#include "../spu_channels.c"
#include "../../ppu/ppu_memory.h"

#define LINE        0x00020000u
#define LSA         0x00001000u
#define READERS     3
#define READS       400000
#define PPU_WRITERS 2
#define RUN         6

static spu_context g_reader[READERS];
static spu_context g_spu_writer;
static atomic_int g_stop;
static atomic_ulong g_torn, g_reads, g_spu_commits;

static void wrch(spu_context* ctx, uint32_t channel, uint32_t v)
{
    spu_wrch(ctx, channel, spu_make_preferred_u32(v));
}

static void spu_atomic(spu_context* ctx, uint32_t ea, uint32_t cmd)
{
    wrch(ctx, MFC_LSA, LSA);
    wrch(ctx, MFC_EAH, 0);
    wrch(ctx, MFC_EAL, ea);
    wrch(ctx, MFC_Cmd, cmd);
}

static uint64_t be64(const uint8_t* p)
{
    uint64_t v;
    memcpy(&v, p, 8);
    return __builtin_bswap64(v);
}

static void put_be64(uint8_t* p, uint64_t v)
{
    v = __builtin_bswap64(v);
    memcpy(p, &v, 8);
}

static int run_consistent(const uint8_t* line, unsigned first)
{
    const uint64_t head = be64(line + first * 8u);
    uint64_t prev = head;
    for (unsigned i = first + 1; i < first + RUN; i++) {
        const uint64_t v = be64(line + i * 8u);
        if (v > prev) return 0;
        prev = v;
    }
    return head - prev <= 1;
}

static int snapshot_consistent(const uint8_t* line)
{
    for (unsigned w = 0; w < PPU_WRITERS; w++)
        if (!run_consistent(line, w * RUN)) return 0;
    for (unsigned i = PPU_WRITERS * RUN; i < 16u; i++)
        if (be64(line + i * 8u) != be64(line + PPU_WRITERS * RUN * 8u)) return 0;
    return 1;
}

static void* ppu_writer(void* arg)
{
    const unsigned first = (unsigned)(uintptr_t)arg * RUN;
    for (uint64_t n = 1; !atomic_load_explicit(&g_stop, memory_order_relaxed); n++)
        for (unsigned i = first; i < first + RUN; i++)
            vm_write64(LINE + i * 8u, n);
    return NULL;
}

static void* spu_writer(void* arg)
{
    (void)arg;
    spu_context* ctx = &g_spu_writer;
    while (!atomic_load_explicit(&g_stop, memory_order_relaxed)) {
        spu_atomic(ctx, LINE, MFC_GETLLAR_CMD);
        uint8_t* ls = &ctx->ls[LSA];
        const uint64_t n = be64(ls + PPU_WRITERS * RUN * 8u) + 1;
        for (unsigned i = PPU_WRITERS * RUN; i < 16u; i++) put_be64(ls + i * 8u, n);
        spu_atomic(ctx, LINE, MFC_PUTLLC_CMD);
        if (ctx->atomic_stat == 0) atomic_fetch_add(&g_spu_commits, 1);
    }
    return NULL;
}

static void* reader(void* arg)
{
    spu_context* ctx = (spu_context*)arg;
    unsigned long torn = 0;
    for (int k = 0; k < READS; k++) {
        spu_atomic(ctx, LINE, MFC_GETLLAR_CMD);
        if (!snapshot_consistent(&ctx->ls[LSA])) torn++;
    }
    atomic_fetch_add(&g_torn, torn);
    atomic_fetch_add(&g_reads, READS);
    return NULL;
}

int main(void)
{
    memset(g_guest_mem + LINE, 0, 128);
    spu_context_init(&g_spu_writer, 0);
    g_spu_writer.spu_id = READERS;
    spu_atomic(&g_spu_writer, LINE, MFC_GETLLAR_CMD);
    for (int i = 0; i < READERS; i++) {
        spu_context_init(&g_reader[i], 0);
        g_reader[i].spu_id = (uint32_t)i;
        spu_atomic(&g_reader[i], LINE, MFC_GETLLAR_CMD);
    }

    pthread_t w[PPU_WRITERS + 1], r[READERS];
    for (int i = 0; i < PPU_WRITERS; i++)
        if (pthread_create(&w[i], NULL, ppu_writer, (void*)(uintptr_t)i)) return 2;
    if (pthread_create(&w[PPU_WRITERS], NULL, spu_writer, NULL)) return 2;
    for (int i = 0; i < READERS; i++)
        if (pthread_create(&r[i], NULL, reader, &g_reader[i])) return 2;
    for (int i = 0; i < READERS; i++) pthread_join(r[i], NULL);
    atomic_store(&g_stop, 1);
    for (int i = 0; i <= PPU_WRITERS; i++) pthread_join(w[i], NULL);

    const unsigned long torn = atomic_load(&g_torn);
    const int ok = torn == 0 && snapshot_consistent(g_guest_mem + LINE) &&
                   be64(g_guest_mem + LINE) > 0 && atomic_load(&g_spu_commits) > 0;
    printf("GETLLAR snapshot atomicity: %lu reads, %lu torn, %lu SPU commits, line %llu/%llu/%llu: %s\n",
           atomic_load(&g_reads), torn, atomic_load(&g_spu_commits),
           (unsigned long long)be64(g_guest_mem + LINE),
           (unsigned long long)be64(g_guest_mem + LINE + RUN * 8u),
           (unsigned long long)be64(g_guest_mem + LINE + PPU_WRITERS * RUN * 8u),
           ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}

void ppu_resv_break_store(uint64_t ea)                 { (void)ea; }
void ps3_ww_report_inline(uint32_t a, uint64_t v, int w)
{
    (void)a; (void)v; (void)w;
}
unsigned long long ps3_ms_now(void)                    { return 0; }
uint32_t g_ww_lo = 0xFFFFFFFFu, g_ww_hi = 0;
int      g_resv_store_active = 0;
uint32_t g_barrier_sync_watch = 0;

void (*g_spurs_kernel_hook)(uint32_t) = 0;
void spurs_ef_set_from_spu(uint32_t ea, uint16_t bits) { (void)ea; (void)bits; }

uint32_t g_spu_image_src_ea, g_spu_image_ls_start, g_spu_image_span;
void spu_thread_publish_ctx(uint32_t tid, void* ctx) { (void)tid; (void)ctx; }
