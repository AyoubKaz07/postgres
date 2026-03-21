/*-------------------------------------------------------------------------
 *
 * pgstat_vfdcache.c
 *	  Implementation of VFD (Virtual File Descriptor) cache statistics.
 *
 * The VFD cache in fd.c maintains a cache of open file
 * descriptors, bounded by max_files_per_process.  When the cache is full,
 * the least-recently-used entry is evicted (its OS fd closed) so a new
 * file can be opened.  A subsequent access to an evicted VFD must call
 * open() again, incurring a syscall that a warm cache would have avoided.
 *
 * This module tracks hits (fd was open, no syscall), misses (fd was
 * closed, open() required), and evictions (close() to make room) for the
 * current backend.  Because the VFD cache is strictly per-backend, these
 * counters are also per-backend so no shared memory or locking is used.
 *
 * The view pg_stat_vfdcache exposes these counters for the current session
 * together with the live cache occupancy and the configured limit, giving
 * possibility to diagnose fd-cache pressure.
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  src/backend/utils/activity/pgstat_vfdcache.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "pgstat.h"
#include "storage/fd.h"
#include "utils/timestamp.h"

/*
 * Per-backend VFD cache counters.
 *
 * Updated directly by fd.c via the inline macros in pgstat.h.  Because
 * only the owning backend ever writes these variables, no atomic ops or
 * locks are required.
 */
PgStat_VfdCacheStats PendingVfdCacheStats;

/*
 * Timestamp of the last pg_stat_reset_vfdcache() call for this backend.
 * Zero just means "never reset".
 */
static TimestampTz vfd_stats_reset_timestamp = 0;

/*
 * pgstat_fetch_stat_vfdcache
 *
 * Return a pointer to a filled PgStat_VfdCacheStats for the current
 * backend.  The returned pointer is valid until the next call.
 */
PgStat_VfdCacheStats *
pgstat_fetch_stat_vfdcache(void)
{
	/* counters live directly in PendingVfdCacheStats; just attach timestamp */
	PendingVfdCacheStats.stat_reset_timestamp = vfd_stats_reset_timestamp;
	return &PendingVfdCacheStats;
}

/*
 * pgstat_reset_vfdcache
 *
 * Reset all VFD cache counters for the current backend and record the
 * reset timestamp.
 */
void
pgstat_reset_vfdcache(void)
{
	memset(&PendingVfdCacheStats, 0, sizeof(PendingVfdCacheStats));
	vfd_stats_reset_timestamp = GetCurrentTimestamp();
}