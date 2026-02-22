/*-------------------------------------------------------------------------
 * perf_instrument.c
 *
 *   Implementation of perf_event_open(2) based measurement helpers.
 *   See perf_instrument.h for the full API documentation.
 *
 * Copyright (c) 2024, PostgreSQL Global Development Group
 * (debug/investigation patch - not for upstream)
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#if defined(USE_PERF_INSTRUMENTATION) && defined(__linux__)

#include <linux/perf_event.h>
#include <sys/ioctl.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <string.h>
#include <stdint.h>
#include <stdio.h>
#include <time.h>

#include "utils/perf_instrument.h"
#include "storage/fd.h"     /* DataDir */

/* ----------
 * Output mode:
 *   default  -> elog(LOG, ...)
 *   -DPERF_OUTPUT_FILE -> append to $PGDATA/pg_perf.log as TSV
 * ---------- */
#ifdef PERF_OUTPUT_FILE
static FILE *perf_output_file = NULL;

static FILE *
perf_get_output_file(void)
{
    if (perf_output_file == NULL)
    {
        char        path[MAXPGPATH];
        snprintf(path, sizeof(path), "%s/pg_perf.log", DataDir);
        perf_output_file = fopen(path, "a");
        if (perf_output_file == NULL)
            elog(WARNING, "[perf] could not open output file \"%s\": %m", path);
    }
    return perf_output_file;
}
#endif  /* PERF_OUTPUT_FILE */


/* ----------
 * perf_event_open wrapper
 * Not in glibc, must be done via raw syscall.
 * ---------- */
static long
sys_perf_event_open(struct perf_event_attr *attr,
                    pid_t pid, int cpu, int group_fd, unsigned long flags)
{
    return syscall(__NR_perf_event_open, attr, pid, cpu, group_fd, flags);
}


/*
 * perf_capture_open
 *
 *   Initialise the PerfCapture struct and open the perf fd.
 *   Does NOT start counting; call perf_capture_enable() for that.
 */
void
perf_capture_open(PerfCapture *cap, const char *name,
                  PerfCounterSpec spec,
                  const char *file, int line)
{
    struct perf_event_attr  attr;

    /* Zero the struct so any unset fields are safe defaults */
    memset(cap, 0, sizeof(PerfCapture));
    cap->fd           = -1;
    cap->name         = name;
    cap->file         = file;
    cap->line         = line;
    cap->counter_name = spec.name;
    cap->active       = false;
    cap->warned       = false;
    cap->count        = 0;
    cap->invocations  = 0;

    memset(&attr, 0, sizeof(attr));
    attr.type           = spec.type;
    attr.size           = sizeof(attr);
    attr.config         = spec.config;
    attr.disabled       = 1;    /* start disabled; we enable explicitly */
    attr.exclude_kernel = 1;    /* userspace only - matches typical pg analysis */
    attr.exclude_hv     = 1;

    cap->fd = (int) sys_perf_event_open(&attr,
                                         0,    /* pid=0: current process */
                                        -1,    /* cpu=-1: any cpu */
                                        -1,    /* group_fd=-1: no group */
                                         0);   /* flags */

    if (cap->fd == -1)
    {
        if (!cap->warned)
        {
            elog(WARNING,
                 "[perf] perf_event_open failed for \"%s\" (%s:%d): %m "
                 "(check /proc/sys/kernel/perf_event_paranoid)",
                 cap->name, cap->file, cap->line);
            cap->warned = true;
        }
    }
}


/*
 * perf_capture_enable
 *
 *   Reset and enable the counter. Safe to call repeatedly (RESUME pattern).
 *   Increments invocation counter each time.
 */
void
perf_capture_enable(PerfCapture *cap)
{
    if (cap->fd == -1)
        return;

    /*
     * Reset the kernel counter but NOT our accumulated cap->count,
     * so that across multiple RESUME/PAUSE cycles the totals add up
     * correctly in perf_capture_read.
     */
    ioctl(cap->fd, PERF_EVENT_IOC_RESET, 0);
    ioctl(cap->fd, PERF_EVENT_IOC_ENABLE, 0);
    cap->active = true;
    cap->invocations++;
}


/*
 * perf_capture_disable
 *
 *   Stop counting. Does not read or close.
 */
void
perf_capture_disable(PerfCapture *cap)
{
    if (cap->fd == -1 || !cap->active)
        return;

    ioctl(cap->fd, PERF_EVENT_IOC_DISABLE, 0);
    cap->active = false;
}


/*
 * perf_capture_read
 *
 *   Read the current counter value and accumulate into cap->count.
 *   Must be called after perf_capture_disable.
 */
void
perf_capture_read(PerfCapture *cap)
{
    uint64_t    val;
    ssize_t     ret;

    if (cap->fd == -1)
        return;

    ret = read(cap->fd, &val, sizeof(val));
    if (ret != sizeof(val))
    {
        elog(WARNING, "[perf] read failed for \"%s\": %m", cap->name);
        return;
    }

    cap->count += val;
}


/*
 * perf_capture_close
 *
 *   Close the fd. The count is still accessible for reporting.
 */
void
perf_capture_close(PerfCapture *cap)
{
    if (cap->fd == -1)
        return;

    close(cap->fd);
    cap->fd = -1;
}


/*
 * perf_capture_report
 *
 *   Emit the captured measurement to the configured output destination.
 *
 *   elog mode output (default):
 *     LOG:  [perf] instructions | sort_inner_loop | count=1847392 | invocations=1 | tuplesort.c:1423
 *
 *   File mode (PERF_OUTPUT_FILE):
 *     TSV columns: timestamp_ns, pid, counter, name, count, invocations, file, line
 */
void
perf_capture_report(const PerfCapture *cap)
{
    if (cap->fd != -1 && cap->active)
    {
        /*
         * Caller forgot to PERF_STOP before PERF_REPORT.
         * Not fatal, just note it.
         */
        elog(WARNING, "[perf] PERF_REPORT called while counter is still active for \"%s\"",
             cap->name);
    }

#ifdef PERF_OUTPUT_FILE
    {
        FILE       *f = perf_get_output_file();
        struct timespec ts;

        if (f)
        {
            clock_gettime(CLOCK_MONOTONIC, &ts);
            fprintf(f, "%lld\t%d\t%s\t%s\t%llu\t%llu\t%s\t%d\n",
                    (long long) ts.tv_sec * 1000000000LL + ts.tv_nsec,
                    (int) getpid(),
                    cap->counter_name ? cap->counter_name : "?",
                    cap->name,
                    (unsigned long long) cap->count,
                    (unsigned long long) cap->invocations,
                    cap->file,
                    cap->line);
            fflush(f);
        }
    }
#else
    elog(LOG,
         "[perf] %s | %s | count=%llu | invocations=%llu | %s:%d",
         cap->counter_name ? cap->counter_name : "?",
         cap->name,
         (unsigned long long) cap->count,
         (unsigned long long) cap->invocations,
         cap->file,
         cap->line);
#endif  /* PERF_OUTPUT_FILE */
}

#endif  /* USE_PERF_INSTRUMENTATION && __linux__ */