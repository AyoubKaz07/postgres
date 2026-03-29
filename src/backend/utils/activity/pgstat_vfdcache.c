/* -------------------------------------------------------------------------
 *
 * pgstat_vfdcache.c
 *	  Implementation of VFD cache statistics.
 *
 * VFD events are first counted in backend-local pending storage and then
 * flushed into shared-memory cumulative stats, following the same model as
 * other fixed stats kinds.
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  src/backend/utils/activity/pgstat_vfdcache.c
 * -------------------------------------------------------------------------
 */

#include "postgres.h"

#include "pgstat.h"
#include "storage/fd.h"
#include "utils/memutils.h"
#include "utils/pgstat_internal.h"

/*
 * Backend-local VFD counters waiting to be flushed.
 */
PgStat_VfdCacheStats PendingVfdCacheStats = {0};

/*
 * Count a VFD cache hit.
 */
void
pgstat_count_vfd_hit(void)
{
	PendingVfdCacheStats.vfd_hits++;
	pgstat_report_fixed = true;
}

/*
 * Count a VFD cache miss.
 */
void
pgstat_count_vfd_miss(void)
{
	PendingVfdCacheStats.vfd_misses++;
	pgstat_report_fixed = true;
}

/*
 * Count a VFD cache eviction.
 */
void
pgstat_count_vfd_eviction(void)
{
	PendingVfdCacheStats.vfd_evictions++;
	pgstat_report_fixed = true;
}

/*
 * Flush out backend-local pending VFD cache stats.
 */
bool
pgstat_vfdcache_flush_cb(bool nowait)
{
	PgStat_EntryRef *entry_ref;
	PgStatShared_Backend *shbackendent;
	PgStatShared_VfdCache *stats_shmem = &pgStatLocal.shmem->vfdcache;

	if (pg_memory_is_all_zeros(&PendingVfdCacheStats,
							   sizeof(struct PgStat_VfdCacheStats)))
		return false;

	entry_ref = pgstat_get_entry_ref_locked(PGSTAT_KIND_BACKEND, InvalidOid,
											MyProcNumber, nowait);
	if (!entry_ref)
		return true;

	shbackendent = (PgStatShared_Backend *) entry_ref->shared_stats;

	shbackendent->stats.vfd_stats.vfd_entries = (PgStat_Counter) GetVfdCacheEntries();
	shbackendent->stats.vfd_stats.vfd_cache_bytes = (PgStat_Counter) GetVfdCacheBytes();

	pgstat_unlock_entry(entry_ref);

	pgstat_begin_changecount_write(&stats_shmem->changecount);
	stats_shmem->stats.vfd_hits += PendingVfdCacheStats.vfd_hits;
	stats_shmem->stats.vfd_misses += PendingVfdCacheStats.vfd_misses;
	stats_shmem->stats.vfd_evictions += PendingVfdCacheStats.vfd_evictions;
	pgstat_end_changecount_write(&stats_shmem->changecount);

	MemSet(&PendingVfdCacheStats, 0, sizeof(PendingVfdCacheStats));

	return false;
}

/*
 * Support function for SQL-callable pg_stat_get_vfd_* functions.
 */
PgStat_VfdCacheStats *
pgstat_fetch_stat_vfdcache(void)
{
	pgstat_snapshot_fixed(PGSTAT_KIND_VFDCACHE);

	return &pgStatLocal.snapshot.vfdcache;
}

void
pgstat_vfdcache_init_shmem_cb(void *stats)
{
	PgStatShared_VfdCache *stats_shmem = (PgStatShared_VfdCache *) stats;

	LWLockInitialize(&stats_shmem->lock, LWTRANCHE_PGSTATS_DATA);
}

void
pgstat_vfdcache_reset_all_cb(TimestampTz ts)
{
	PgStatShared_VfdCache *stats_shmem = &pgStatLocal.shmem->vfdcache;

	LWLockAcquire(&stats_shmem->lock, LW_EXCLUSIVE);
	pgstat_copy_changecounted_stats(&stats_shmem->reset_offset,
									&stats_shmem->stats,
									sizeof(stats_shmem->stats),
									&stats_shmem->changecount);
	stats_shmem->stats.stat_reset_timestamp = ts;
	LWLockRelease(&stats_shmem->lock);
}

void
pgstat_vfdcache_snapshot_cb(void)
{
	PgStatShared_VfdCache *stats_shmem = &pgStatLocal.shmem->vfdcache;
	PgStat_VfdCacheStats *reset_offset = &stats_shmem->reset_offset;
	PgStat_VfdCacheStats reset;

	pgstat_copy_changecounted_stats(&pgStatLocal.snapshot.vfdcache,
									&stats_shmem->stats,
									sizeof(stats_shmem->stats),
									&stats_shmem->changecount);

	LWLockAcquire(&stats_shmem->lock, LW_SHARED);
	memcpy(&reset, reset_offset, sizeof(stats_shmem->stats));
	LWLockRelease(&stats_shmem->lock);

	pgStatLocal.snapshot.vfdcache.vfd_hits -= reset.vfd_hits;
	pgStatLocal.snapshot.vfdcache.vfd_misses -= reset.vfd_misses;
	pgStatLocal.snapshot.vfdcache.vfd_evictions -= reset.vfd_evictions;
}

void
pgstat_reset_vfdcache(void)
{
	pgstat_reset_of_kind(PGSTAT_KIND_VFDCACHE);
}
