/*
 * rsx_blend_log.h -- RSX_BLEND_LOG: log-only instrumentation, no effect on rendering.
 *
 * Question it answers: does the title ever write SET_BLEND_COLOR or use the
 * CONSTANT_COLOR / CONSTANT_ALPHA blend factors (0x8001-0x8004)? The Metal
 * backend maps those factors to One and never receives the colour, so a draw
 * that uses them is blended wrongly (the Folklore port fixed this in 25c4701).
 *
 * RSX_BLEND_LOG is ON when set to anything but "" or "0". The helpers are shared
 * by threads, so they use real atomics; with a compiler that has no __atomic the
 * whole facility is compiled out (always off) rather than racing on integers.
 * Budgets are saturating: a counter never passes its limit, so it cannot wrap and
 * reopen the log, and once the budget is spent a call is one load.
 */
#ifndef RSX_BLEND_LOG_H
#define RSX_BLEND_LOG_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#if defined(__GNUC__) || defined(__clang__)
#  define RSX_BLEND_LOG_AVAILABLE 1

static inline int rsx_blend_log_on(void)
{
    static int s_on = -1;                       /* -1 unread; a repeated first read is harmless */
    int v = __atomic_load_n(&s_on, __ATOMIC_RELAXED);
    if (v < 0) {
        const char* e = getenv("RSX_BLEND_LOG");
        v = (e && *e && strcmp(e, "0") != 0) ? 1 : 0;
        __atomic_store_n(&s_on, v, __ATOMIC_RELAXED);
    }
    return v;
}

/* True for the first `limit` calls on this counter (saturating CAS). */
static inline int rsx_blend_log_budget(int* ctr, int limit)
{
    int v = __atomic_load_n(ctr, __ATOMIC_RELAXED);
    while (v < limit) {
        if (__atomic_compare_exchange_n(ctr, &v, v + 1, 0, __ATOMIC_RELAXED, __ATOMIC_RELAXED))
            return 1;
    }
    return 0;
}

/* True at most once per `period_ms` (monotonic clock, CAS on the last time). */
static inline int rsx_blend_log_tick(long long* last_ms, long long period_ms)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    const long long now = (long long)ts.tv_sec * 1000LL + ts.tv_nsec / 1000000LL;
    long long prev = __atomic_load_n(last_ms, __ATOMIC_RELAXED);
    if (prev != 0 && now - prev < period_ms) return 0;
    return __atomic_compare_exchange_n(last_ms, &prev, now, 0, __ATOMIC_RELAXED, __ATOMIC_RELAXED);
}

#define RSX_BLEND_LOG_ADD(p, v) __atomic_add_fetch((p), (v), __ATOMIC_RELAXED)
#define RSX_BLEND_LOG_LOAD(p)   __atomic_load_n((p), __ATOMIC_RELAXED)

#else  /* no __atomic: compiled out */
#  define RSX_BLEND_LOG_AVAILABLE 0
static inline int rsx_blend_log_on(void) { return 0; }
#endif

#endif /* RSX_BLEND_LOG_H */
