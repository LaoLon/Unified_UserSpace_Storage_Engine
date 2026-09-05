#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <limits.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <unistd.h>

#include "hfs_durability.h"

#define HFS_DURABILITY_TRACE_MAX_EVENTS 512U
#define HFS_DURABILITY_TRACE_PHASE_SIZE 32U

/* One optional trace event emitted only by the synchronous durability path. */
typedef struct HfsDurabilityTraceEvent {
	uint64_t sequence;
	uint64_t inodeId;
	int result;
	char phase[HFS_DURABILITY_TRACE_PHASE_SIZE];
} HfsDurabilityTraceEvent;

static pthread_mutex_t g_hfsDurabilityTraceLock = PTHREAD_MUTEX_INITIALIZER;
static HfsDurabilityTraceEvent
	g_hfsDurabilityTrace[HFS_DURABILITY_TRACE_MAX_EVENTS];
static char g_hfsDurabilityTracePath[PATH_MAX];
static uint32_t g_hfsDurabilityTraceCount;
static uint64_t g_hfsDurabilityTraceSequence;
static bool g_hfsDurabilityTraceInitialized;
static bool g_hfsDurabilityTraceEnabled;

/* Initialize a disabled-by-default trace from HFS_DURABILITY_TRACE_PATH. */
void hfsDurabilityTraceInit(void) {
	const char *path;

	if (g_hfsDurabilityTraceInitialized) {
		return;
	}
	path = getenv("HFS_DURABILITY_TRACE_PATH");
	g_hfsDurabilityTracePath[0] = '\0';
	g_hfsDurabilityTraceEnabled = false;
	if (path != NULL && path[0] != '\0' &&
	    snprintf(g_hfsDurabilityTracePath,
		     sizeof(g_hfsDurabilityTracePath), "%s", path) <
		(int)sizeof(g_hfsDurabilityTracePath)) {
		g_hfsDurabilityTraceEnabled = true;
	}
	g_hfsDurabilityTraceCount = 0;
	g_hfsDurabilityTraceSequence = 0;
	g_hfsDurabilityTraceInitialized = true;
}

/* Append a bounded in-memory event while preserving the public errno value. */
void hfsDurabilityTraceRecord(uint64_t inodeId, const char *phase, int result) {
	HfsDurabilityTraceEvent *event;
	int savedErrno = errno;

	if (!g_hfsDurabilityTraceEnabled || phase == NULL) {
		return;
	}
	pthread_mutex_lock(&g_hfsDurabilityTraceLock);
	if (g_hfsDurabilityTraceCount < HFS_DURABILITY_TRACE_MAX_EVENTS) {
		event = &g_hfsDurabilityTrace[g_hfsDurabilityTraceCount++];
		event->sequence = ++g_hfsDurabilityTraceSequence;
		event->inodeId = inodeId;
		event->result = result;
		snprintf(event->phase, sizeof(event->phase), "%s", phase);
	}
	pthread_mutex_unlock(&g_hfsDurabilityTraceLock);
	errno = savedErrno;
}

/* Write trace events through direct syscalls to avoid LD_PRELOAD recursion. */
void hfsDurabilityTraceFini(void) {
	char line[160];
	uint32_t i;
	int fd;
	int length;
	int savedErrno = errno;

	if (!g_hfsDurabilityTraceInitialized || !g_hfsDurabilityTraceEnabled) {
		return;
	}
	pthread_mutex_lock(&g_hfsDurabilityTraceLock);
	fd = (int)syscall(SYS_openat, AT_FDCWD, g_hfsDurabilityTracePath,
		O_CREAT | O_WRONLY | O_TRUNC | O_CLOEXEC, 0644);
	if (fd >= 0) {
		for (i = 0; i < g_hfsDurabilityTraceCount; i++) {
			length = snprintf(line, sizeof(line),
				"sequence=%"PRIu64" inode=%"PRIu64" phase=%s result=%d\n",
				g_hfsDurabilityTrace[i].sequence,
				g_hfsDurabilityTrace[i].inodeId,
				g_hfsDurabilityTrace[i].phase,
				g_hfsDurabilityTrace[i].result);
			if (length > 0 && length < (int)sizeof(line)) {
				(void)syscall(SYS_write, fd, line, (size_t)length);
			}
		}
		(void)syscall(SYS_close, fd);
	}
	g_hfsDurabilityTraceEnabled = false;
	pthread_mutex_unlock(&g_hfsDurabilityTraceLock);
	errno = savedErrno;
}
