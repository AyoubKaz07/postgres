/*-------------------------------------------------------------------------
 * perf_instrument.h
 *
 *   Debug-only infrastructure for hardware performance counter measurement
 *   at arbitrary code sections using perf_event_open(2).
 *
 *   NOT intended for upstream commit. Apply as a patch on top of master
 *   when doing performance investigation work.
 *
 *   USAGE OVERVIEW
 *   --------------
 *
 *   1. Single counter, quick:
 *
 *       PerfCapture cap;
 *       PERF_START(cap, "my-label", PERF_HW_INSTRUCTIONS);
 *       ... code ...
 *       PERF_STOP_AND_REPORT(cap);
 *
 *   2. Multiple counters for the same region:
 *
 *       PerfCapture caps[3];
 *       PERF_START(caps[0], "instructions",  PERF_HW_INSTRUCTIONS);
 *       PERF_START(caps[1], "cache-misses",  PERF_HW_CACHE_MISSES);
 *       PERF_START(caps[2], "branch-misses", PERF_HW_BRANCH_MISSES);
 *       ... code ...
 *       PERF_STOP_ALL(caps, 3);
 *       PERF_REPORT_ALL(caps, 3);
 *
 *   3. Accumulate across multiple calls (e.g. inside a loop):
 *
 *       PerfCapture cap;
 *       PERF_INIT(cap, "sort-inner-loop", PERF_HW_INSTRUCTIONS);
 *       for (...)
 *       {
 *           PERF_RESUME(cap);
 *           ... hot code ...
 *           PERF_PAUSE(cap);
 *       }
 *       PERF_REPORT(cap);
 *
 *   OUTPUT
 *   ------
 *   By default output goes to elog(LOG). If PERF_OUTPUT_FILE is defined
 *   at compile time, output is appended to $PGDATA/pg_perf.log in a
 *   tab-separated format suitable for analysis with awk/pandas.
 *
 *   BUILDING
 *   --------
 *   Add -DUSE_PERF_INSTRUMENTATION to CFLAGS (or pg_config --cflags).
 *   Only activates on Linux; silently compiles to nothing elsewhere.
 *
 *   PERMISSIONS
 *   -----------
 *   Requires /proc/sys/kernel/perf_event_paranoid <= 1, or CAP_PERFMON.
 *   If perf_event_open(2) fails the macros silently no-op after one
 *   WARNING so they never crash a running backend.
 *
 * Copyright (c) 2024, PostgreSQL Global Development Group
 * (debug/investigation patch - not for upstream)
 *-------------------------------------------------------------------------
 */
#ifndef PERF_INSTRUMENT_H
#define PERF_INSTRUMENT_H

#if defined(USE_PERF_INSTRUMENTATION) && defined(__linux__)

#include <linux/perf_event.h>
#include <sys/ioctl.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <stdint.h>
#include <string.h>

#include "postgres.h"   /* for elog, palloc, DataDir */


/* -------------------------------------------------------------------------
 * PerfCounterSpec - bundles type+config into one token so it can be passed
 * as a single macro argument without confusing the preprocessor's comma
 * counting. PERF_INIT/PERF_START take one COUNTER argument of this type.
 * ------------------------------------------------------------------------- */
typedef struct PerfCounterSpec
{
    uint32_t    type;
    uint64_t    config;
    const char *name;
} PerfCounterSpec;

/*
 * Counter alias table.
 *
 * Each alias is a compound literal of type PerfCounterSpec.  Because it is
 * wrapped in parentheses it is a single preprocessor token regardless of the
 * commas inside, solving the "macro requires 4 args but only 3 given" problem.
 *
 * Usage:  PERF_INIT(cap, "label", PERF_HW_INSTRUCTIONS);
 */

/* Hardware counters - backed by CPU PMU registers */
#define PERF_HW_INSTRUCTIONS \
    ((PerfCounterSpec){PERF_TYPE_HARDWARE, PERF_COUNT_HW_INSTRUCTIONS,         "hw/instructions"})
#define PERF_HW_CPU_CYCLES \
    ((PerfCounterSpec){PERF_TYPE_HARDWARE, PERF_COUNT_HW_CPU_CYCLES,            "hw/cpu-cycles"})
#define PERF_HW_CACHE_REFS \
    ((PerfCounterSpec){PERF_TYPE_HARDWARE, PERF_COUNT_HW_CACHE_REFERENCES,      "hw/cache-refs"})
#define PERF_HW_CACHE_MISSES \
    ((PerfCounterSpec){PERF_TYPE_HARDWARE, PERF_COUNT_HW_CACHE_MISSES,          "hw/cache-misses"})
#define PERF_HW_BRANCH_INSTRS \
    ((PerfCounterSpec){PERF_TYPE_HARDWARE, PERF_COUNT_HW_BRANCH_INSTRUCTIONS,   "hw/branch-instrs"})
#define PERF_HW_BRANCH_MISSES \
    ((PerfCounterSpec){PERF_TYPE_HARDWARE, PERF_COUNT_HW_BRANCH_MISSES,         "hw/branch-misses"})
#define PERF_HW_BUS_CYCLES \
    ((PerfCounterSpec){PERF_TYPE_HARDWARE, PERF_COUNT_HW_BUS_CYCLES,            "hw/bus-cycles"})
#define PERF_HW_STALLED_CYCLES_FRONT \
    ((PerfCounterSpec){PERF_TYPE_HARDWARE, PERF_COUNT_HW_STALLED_CYCLES_FRONTEND, "hw/stalled-front"})
#define PERF_HW_STALLED_CYCLES_BACK \
    ((PerfCounterSpec){PERF_TYPE_HARDWARE, PERF_COUNT_HW_STALLED_CYCLES_BACKEND,  "hw/stalled-back"})

/* Software counters - kernel software events, always available */
#define PERF_SW_CPU_CLOCK \
    ((PerfCounterSpec){PERF_TYPE_SOFTWARE, PERF_COUNT_SW_CPU_CLOCK,             "sw/cpu-clock"})
#define PERF_SW_TASK_CLOCK \
    ((PerfCounterSpec){PERF_TYPE_SOFTWARE, PERF_COUNT_SW_TASK_CLOCK,            "sw/task-clock"})
#define PERF_SW_PAGE_FAULTS \
    ((PerfCounterSpec){PERF_TYPE_SOFTWARE, PERF_COUNT_SW_PAGE_FAULTS,           "sw/page-faults"})
#define PERF_SW_PAGE_FAULTS_MIN \
    ((PerfCounterSpec){PERF_TYPE_SOFTWARE, PERF_COUNT_SW_PAGE_FAULTS_MIN,       "sw/faults-min"})
#define PERF_SW_PAGE_FAULTS_MAJ \
    ((PerfCounterSpec){PERF_TYPE_SOFTWARE, PERF_COUNT_SW_PAGE_FAULTS_MAJ,       "sw/faults-maj"})
#define PERF_SW_CONTEXT_SWITCHES \
    ((PerfCounterSpec){PERF_TYPE_SOFTWARE, PERF_COUNT_SW_CONTEXT_SWITCHES,      "sw/ctx-switches"})
#define PERF_SW_CPU_MIGRATIONS \
    ((PerfCounterSpec){PERF_TYPE_SOFTWARE, PERF_COUNT_SW_CPU_MIGRATIONS,        "sw/cpu-migrations"})

/* Hardware cache counters */
#define PERF_CACHE_L1D_LOADS \
    ((PerfCounterSpec){PERF_TYPE_HW_CACHE, \
        (PERF_COUNT_HW_CACHE_L1D | (PERF_COUNT_HW_CACHE_OP_READ<<8) | (PERF_COUNT_HW_CACHE_RESULT_ACCESS<<16)), \
        "cache/l1d-loads"})
#define PERF_CACHE_L1D_LOAD_MISSES \
    ((PerfCounterSpec){PERF_TYPE_HW_CACHE, \
        (PERF_COUNT_HW_CACHE_L1D | (PERF_COUNT_HW_CACHE_OP_READ<<8) | (PERF_COUNT_HW_CACHE_RESULT_MISS<<16)), \
        "cache/l1d-load-misses"})
#define PERF_CACHE_L1I_LOADS \
    ((PerfCounterSpec){PERF_TYPE_HW_CACHE, \
        (PERF_COUNT_HW_CACHE_L1I | (PERF_COUNT_HW_CACHE_OP_READ<<8) | (PERF_COUNT_HW_CACHE_RESULT_ACCESS<<16)), \
        "cache/l1i-loads"})
#define PERF_CACHE_L1I_LOAD_MISSES \
    ((PerfCounterSpec){PERF_TYPE_HW_CACHE, \
        (PERF_COUNT_HW_CACHE_L1I | (PERF_COUNT_HW_CACHE_OP_READ<<8) | (PERF_COUNT_HW_CACHE_RESULT_MISS<<16)), \
        "cache/l1i-load-misses"})
#define PERF_CACHE_LL_LOADS \
    ((PerfCounterSpec){PERF_TYPE_HW_CACHE, \
        (PERF_COUNT_HW_CACHE_LL | (PERF_COUNT_HW_CACHE_OP_READ<<8) | (PERF_COUNT_HW_CACHE_RESULT_ACCESS<<16)), \
        "cache/ll-loads"})
#define PERF_CACHE_LL_LOAD_MISSES \
    ((PerfCounterSpec){PERF_TYPE_HW_CACHE, \
        (PERF_COUNT_HW_CACHE_LL | (PERF_COUNT_HW_CACHE_OP_READ<<8) | (PERF_COUNT_HW_CACHE_RESULT_MISS<<16)), \
        "cache/ll-load-misses"})
#define PERF_CACHE_DTLB_LOADS \
    ((PerfCounterSpec){PERF_TYPE_HW_CACHE, \
        (PERF_COUNT_HW_CACHE_DTLB | (PERF_COUNT_HW_CACHE_OP_READ<<8) | (PERF_COUNT_HW_CACHE_RESULT_ACCESS<<16)), \
        "cache/dtlb-loads"})
#define PERF_CACHE_DTLB_LOAD_MISSES \
    ((PerfCounterSpec){PERF_TYPE_HW_CACHE, \
        (PERF_COUNT_HW_CACHE_DTLB | (PERF_COUNT_HW_CACHE_OP_READ<<8) | (PERF_COUNT_HW_CACHE_RESULT_MISS<<16)), \
        "cache/dtlb-load-misses"})


/* -------------------------------------------------------------------------
 * PerfCapture - state for one counter at one capture site
 * ------------------------------------------------------------------------- */
typedef struct PerfCapture
{
    int             fd;             /* perf_event_open fd, -1 if failed/inactive */
    uint64_t        count;          /* accumulated count after stop */
    uint64_t        invocations;    /* number of RESUME/PAUSE cycles */
    const char     *name;           /* user-supplied label */
    const char     *file;           /* source file (__FILE__) */
    int             line;           /* source line (__LINE__) */
    const char     *counter_name;   /* stringified counter type for output */
    bool            active;         /* currently counting? */
    bool            warned;         /* emitted open-failure warning already? */
} PerfCapture;


/* -------------------------------------------------------------------------
 * Internal helpers - implemented in perf_instrument.c
 * ------------------------------------------------------------------------- */
extern void perf_capture_open(PerfCapture *cap, const char *name,
                               PerfCounterSpec spec,
                               const char *file, int line);
extern void perf_capture_enable(PerfCapture *cap);
extern void perf_capture_disable(PerfCapture *cap);
extern void perf_capture_read(PerfCapture *cap);
extern void perf_capture_report(const PerfCapture *cap);
extern void perf_capture_close(PerfCapture *cap);


/* -------------------------------------------------------------------------
 * Public API macros
 * ------------------------------------------------------------------------- */

/*
 * PERF_INIT(cap, label, COUNTER)
 *
 *   Initialise a PerfCapture without starting counting.
 *   Use when you want PERF_RESUME / PERF_PAUSE in a loop.
 *
 *   COUNTER is one of the PERF_HW_* / PERF_SW_* / PERF_CACHE_* aliases,
 *   which expand to a single compound-literal token (PerfCounterSpec).
 */
#define PERF_INIT(cap, label, counter) \
    perf_capture_open(&(cap), (label), (counter), __FILE__, __LINE__)

/*
 * PERF_START(cap, label, COUNTER)
 *
 *   Open the counter and immediately start counting.
 *   Pair with PERF_STOP or PERF_STOP_AND_REPORT.
 *
 *   Example:
 *       PerfCapture c;
 *       PERF_START(c, "heap_fetch", PERF_HW_INSTRUCTIONS);
 */
#define PERF_START(cap, label, counter) \
    do { \
        perf_capture_open(&(cap), (label), (counter), __FILE__, __LINE__); \
        perf_capture_enable(&(cap)); \
    } while (0)

/*
 * PERF_RESUME(cap)
 *
 *   Re-enable counting without resetting. Safe to call on a cap that
 *   was never started (will no-op if fd == -1).
 */
#define PERF_RESUME(cap) \
    perf_capture_enable(&(cap))

/*
 * PERF_PAUSE(cap)
 *
 *   Disable counting and accumulate into cap.count. Does not report.
 *   The fd stays open so PERF_RESUME works.
 */
#define PERF_PAUSE(cap) \
    do { \
        perf_capture_disable(&(cap)); \
        perf_capture_read(&(cap)); \
    } while (0)

/*
 * PERF_STOP(cap)
 *
 *   Disable counting, read the final value, and close the fd.
 *   Does not report. Use PERF_REPORT afterwards.
 */
#define PERF_STOP(cap) \
    do { \
        perf_capture_disable(&(cap)); \
        perf_capture_read(&(cap)); \
        perf_capture_close(&(cap)); \
    } while (0)

/*
 * PERF_REPORT(cap)
 *
 *   Emit the current count to the configured output (elog or file).
 *   Can be called after PERF_STOP or after PERF_PAUSE loops.
 */
#define PERF_REPORT(cap) \
    perf_capture_report(&(cap))

/*
 * PERF_STOP_AND_REPORT(cap)
 *
 *   Convenience: PERF_STOP + PERF_REPORT in one macro.
 *   The most common single-region usage pattern.
 */
#define PERF_STOP_AND_REPORT(cap) \
    do { \
        PERF_STOP(cap); \
        PERF_REPORT(cap); \
    } while (0)

/*
 * PERF_STOP_ALL(caps_array, n)
 * PERF_REPORT_ALL(caps_array, n)
 *
 *   Operate on an array of PerfCapture for multi-counter regions.
 *
 *   Example:
 *       PerfCapture caps[3];
 *       PERF_START(caps[0], "instructions", PERF_HW_INSTRUCTIONS);
 *       PERF_START(caps[1], "cache-misses",  PERF_HW_CACHE_MISSES);
 *       PERF_START(caps[2], "branch-misses", PERF_HW_BRANCH_MISSES);
 *       ... code ...
 *       PERF_STOP_ALL(caps, 3);
 *       PERF_REPORT_ALL(caps, 3);
 */
#define PERF_STOP_ALL(caps, n) \
    do { \
        for (int _pi = 0; _pi < (n); _pi++) \
            PERF_STOP((caps)[_pi]); \
    } while (0)

#define PERF_REPORT_ALL(caps, n) \
    do { \
        for (int _pi = 0; _pi < (n); _pi++) \
            PERF_REPORT((caps)[_pi]); \
    } while (0)

/*
 * PERF_SCOPED(cap, label, COUNTER, code_block)
 *
 *   Inline convenience for one-liner measurements. Wraps a block.
 *
 *   Example:
 *       PerfCapture c;
 *       PERF_SCOPED(c, "qsort", PERF_HW_INSTRUCTIONS,
 *       {
 *           qsort(data, n, sizeof(*data), cmp);
 *       });
 */
#define PERF_SCOPED(cap, label, counter, block) \
    do { \
        PERF_START(cap, label, counter); \
        block \
        PERF_STOP_AND_REPORT(cap); \
    } while (0)

/*
 * PERF_DECLARE_GROUP(name, n)
 *
 *   Declare a fixed-size array of PerfCapture on the stack.
 *   Pure syntactic sugar.
 *
 *   Example:
 *       PERF_DECLARE_GROUP(caps, 4);
 *       PERF_START(caps[0], "instructions", PERF_HW_INSTRUCTIONS);
 *       ...
 */
#define PERF_DECLARE_GROUP(name, n)  PerfCapture name[n]


/* -------------------------------------------------------------------------
 * No-op stubs when instrumentation is disabled or not on Linux
 * All macros expand to nothing, zero overhead in production builds.
 * ------------------------------------------------------------------------- */
#else   /* !USE_PERF_INSTRUMENTATION || !__linux__ */

typedef struct PerfCapture { int _dummy; } PerfCapture;
typedef struct PerfCounterSpec { int _dummy; } PerfCounterSpec;

#define PERF_INIT(cap, label, counter)              ((void)0)
#define PERF_START(cap, label, counter)             ((void)0)
#define PERF_RESUME(cap)                            ((void)0)
#define PERF_PAUSE(cap)                             ((void)0)
#define PERF_STOP(cap)                              ((void)0)
#define PERF_REPORT(cap)                            ((void)0)
#define PERF_STOP_AND_REPORT(cap)                   ((void)0)
#define PERF_STOP_ALL(caps, n)                      ((void)0)
#define PERF_REPORT_ALL(caps, n)                    ((void)0)
#define PERF_SCOPED(cap, label, counter, block)     do { block } while(0)
#define PERF_DECLARE_GROUP(name, n)                 PerfCapture name[n]

#endif  /* USE_PERF_INSTRUMENTATION && __linux__ */

#endif  /* PERF_INSTRUMENT_H */