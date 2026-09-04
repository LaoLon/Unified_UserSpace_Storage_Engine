#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#include "hfs_stats.h"

_Atomic bool hfsStatsEnabledState;
_Atomic uint_fast64_t hfsStatsFdReadLocks;
_Atomic uint_fast64_t hfsStatsFdWriteLocks;
_Atomic uint_fast64_t hfsStatsMetaReadLocks;
_Atomic uint_fast64_t hfsStatsMetaWriteLocks;
_Atomic uint_fast64_t hfsStatsDmaAcquires;
_Atomic uint_fast64_t hfsStatsDmaWaits;
_Atomic uint_fast64_t hfsStatsDmaReleases;
_Atomic uint_fast64_t hfsStatsDmaActivePeak;
_Atomic uint_fast64_t hfsStatsSpdkSubmits;
_Atomic uint_fast64_t hfsStatsSpdkCompletions;
_Atomic uint_fast64_t hfsStatsSpdkRejects;
_Atomic uint_fast64_t hfsStatsErrors;

static bool g_hfsStatsInitialized;
static char g_hfsStatsPath[PATH_MAX];

/* Reset all counters before enabling the optional instrumentation. */
static void hfsStatsReset(void) {
	atomic_store_explicit(&hfsStatsFdReadLocks, 0, memory_order_relaxed);
	atomic_store_explicit(&hfsStatsFdWriteLocks, 0, memory_order_relaxed);
	atomic_store_explicit(&hfsStatsMetaReadLocks, 0, memory_order_relaxed);
	atomic_store_explicit(&hfsStatsMetaWriteLocks, 0, memory_order_relaxed);
	atomic_store_explicit(&hfsStatsDmaAcquires, 0, memory_order_relaxed);
	atomic_store_explicit(&hfsStatsDmaWaits, 0, memory_order_relaxed);
	atomic_store_explicit(&hfsStatsDmaReleases, 0, memory_order_relaxed);
	atomic_store_explicit(&hfsStatsDmaActivePeak, 0, memory_order_relaxed);
	atomic_store_explicit(&hfsStatsSpdkSubmits, 0, memory_order_relaxed);
	atomic_store_explicit(&hfsStatsSpdkCompletions, 0, memory_order_relaxed);
	atomic_store_explicit(&hfsStatsSpdkRejects, 0, memory_order_relaxed);
	atomic_store_explicit(&hfsStatsErrors, 0, memory_order_relaxed);
}

/* Return true for the small set of accepted boolean environment values. */
static bool hfsStatsBool(const char *value) {
	return value != NULL &&
		(!strcmp(value, "1") || !strcasecmp(value, "true") ||
		 !strcasecmp(value, "yes") || !strcasecmp(value, "on"));
}

/* Write a stable key-value snapshot without depending on the log subsystem. */
static void hfsStatsWriteSnapshot(void) {
	char buffer[2048];
	int fd;
	int length;

	if (g_hfsStatsPath[0] == '\0') {
		return;
	}
	fd = open(g_hfsStatsPath, O_CREAT | O_WRONLY | O_TRUNC | O_CLOEXEC, 0644);
	if (fd < 0) {
		return;
	}
	length = snprintf(buffer, sizeof(buffer),
		"fd_read_lock=%"PRIuFAST64"\n"
		"fd_write_lock=%"PRIuFAST64"\n"
		"meta_read_lock=%"PRIuFAST64"\n"
		"meta_write_lock=%"PRIuFAST64"\n"
		"dma_acquire=%"PRIuFAST64"\n"
		"dma_wait=%"PRIuFAST64"\n"
		"dma_release=%"PRIuFAST64"\n"
		"dma_active_peak=%"PRIuFAST64"\n"
		"spdk_submit=%"PRIuFAST64"\n"
		"spdk_complete=%"PRIuFAST64"\n"
		"spdk_reject=%"PRIuFAST64"\n"
		"errors=%"PRIuFAST64"\n",
		atomic_load_explicit(&hfsStatsFdReadLocks, memory_order_relaxed),
		atomic_load_explicit(&hfsStatsFdWriteLocks, memory_order_relaxed),
		atomic_load_explicit(&hfsStatsMetaReadLocks, memory_order_relaxed),
		atomic_load_explicit(&hfsStatsMetaWriteLocks, memory_order_relaxed),
		atomic_load_explicit(&hfsStatsDmaAcquires, memory_order_relaxed),
		atomic_load_explicit(&hfsStatsDmaWaits, memory_order_relaxed),
		atomic_load_explicit(&hfsStatsDmaReleases, memory_order_relaxed),
		atomic_load_explicit(&hfsStatsDmaActivePeak, memory_order_relaxed),
		atomic_load_explicit(&hfsStatsSpdkSubmits, memory_order_relaxed),
		atomic_load_explicit(&hfsStatsSpdkCompletions, memory_order_relaxed),
		atomic_load_explicit(&hfsStatsSpdkRejects, memory_order_relaxed),
		atomic_load_explicit(&hfsStatsErrors, memory_order_relaxed));
	if (length > 0 && length < (int)sizeof(buffer)) {
		(void)write(fd, buffer, (size_t)length);
	}
	close(fd);
}

/* Enable optional counters only when a report path or explicit switch exists. */
void hfsStatsInit(void) {
	const char *path;
	const char *enabled;

	if (g_hfsStatsInitialized) {
		return;
	}
	hfsStatsReset();
	g_hfsStatsPath[0] = '\0';
	path = getenv("HFS_STATS_PATH");
	enabled = getenv("HFS_STATS");
	if (path != NULL && path[0] != '\0') {
		if (snprintf(g_hfsStatsPath, sizeof(g_hfsStatsPath), "%s", path) >=
		    (int)sizeof(g_hfsStatsPath)) {
			g_hfsStatsPath[0] = '\0';
		}
	} else if (hfsStatsBool(enabled)) {
		snprintf(g_hfsStatsPath, sizeof(g_hfsStatsPath),
			"/var/tmp/hfs-stats-%ld.txt", (long)getpid());
	}
	g_hfsStatsInitialized = true;
	atomic_store_explicit(&hfsStatsEnabledState, g_hfsStatsPath[0] != '\0',
				      memory_order_release);
}

/* Stop counting before writing a coherent final report. */
void hfsStatsFini(void) {
	if (!g_hfsStatsInitialized) {
		return;
	}
	atomic_store_explicit(&hfsStatsEnabledState, false, memory_order_release);
	hfsStatsWriteSnapshot();
	g_hfsStatsInitialized = false;
}
