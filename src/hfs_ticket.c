#define _GNU_SOURCE

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "hfs_log.h"
#include "hfs_stats.h"
#include "hfs_ticket.h"
#include "hfs_trace.h"

#define HFS_POSIX_TICKET_DEFAULT_LIMIT 64U
#define HFS_POSIX_TICKET_MIN_LIMIT 8U
#define HFS_POSIX_TICKET_MAX_LIMIT 256U

static pthread_once_t g_hfsPosixTicketConfigOnce = PTHREAD_ONCE_INIT;
static bool g_hfsPosixTicketEnabled;
static uint32_t g_hfsPosixTicketLimit;
static HfsPosixOrderingMode g_hfsPosixOrderingMode;

/* Parse the bounded ticket scheduler configuration once per process. */
static void hfsPosixTicketConfigInit(void)
{
	const char *enabled = getenv("HFS_POSIX_TICKET");
	const char *ordering = getenv("HFS_ORDERING");
	const char *limit = getenv("HFS_POSIX_TICKET_DEPTH");
	char *end = NULL;
	unsigned long parsed;

	g_hfsPosixTicketEnabled = enabled == NULL ||
		(!strcasecmp(enabled, "1") || !strcasecmp(enabled, "true") ||
		 !strcasecmp(enabled, "yes") || !strcasecmp(enabled, "on"));
	g_hfsPosixOrderingMode = g_hfsPosixTicketEnabled
		? HFS_POSIX_ORDER_TICKET : HFS_POSIX_ORDER_RANGE;
	if (ordering != NULL && ordering[0] != '\0') {
		if (!strcasecmp(ordering, "ticket")) {
			g_hfsPosixOrderingMode = HFS_POSIX_ORDER_TICKET;
		} else if (!strcasecmp(ordering, "range")) {
			g_hfsPosixOrderingMode = HFS_POSIX_ORDER_RANGE;
		} else if (!strcasecmp(ordering, "none")) {
			g_hfsPosixOrderingMode = HFS_POSIX_ORDER_NONE;
		}
	}
	g_hfsPosixTicketEnabled = g_hfsPosixOrderingMode == HFS_POSIX_ORDER_TICKET;
	g_hfsPosixTicketLimit = HFS_POSIX_TICKET_DEFAULT_LIMIT;
	if (limit == NULL || limit[0] == '\0') {
		return;
	}
	errno = 0;
	parsed = strtoul(limit, &end, 10);
	if (errno != 0 || end == limit || *end != '\0') {
		return;
	}
	if (parsed < HFS_POSIX_TICKET_MIN_LIMIT) {
		parsed = HFS_POSIX_TICKET_MIN_LIMIT;
	}
	if (parsed > HFS_POSIX_TICKET_MAX_LIMIT) {
		parsed = HFS_POSIX_TICKET_MAX_LIMIT;
	}
	g_hfsPosixTicketLimit = (uint32_t)parsed;
}

/* Return true when the internal ticket path is enabled. */
bool hfsPosixTicketEnabled(void)
{
	pthread_once(&g_hfsPosixTicketConfigOnce, hfsPosixTicketConfigInit);
	return g_hfsPosixTicketEnabled;
}

/* Return the configured ordering mode after the one-time environment parse. */
HfsPosixOrderingMode hfsPosixOrderingMode(void)
{
	pthread_once(&g_hfsPosixTicketConfigOnce, hfsPosixTicketConfigInit);
	return g_hfsPosixOrderingMode;
}

/* Return true when two tickets must observe completion order. */
static bool hfsPosixTicketConflicts(const HfsPosixIoTicket *left,
					const HfsPosixIoTicket *right)
{
	uint64_t leftEnd;
	uint64_t rightEnd;
	bool overlap;

	if (left == NULL || right == NULL || left->length == 0 ||
		right->length == 0) {
		return false;
	}
	leftEnd = left->offset + left->length;
	rightEnd = right->offset + right->length;
	overlap = left->offset < rightEnd && right->offset < leftEnd;
	if (!overlap) {
		return false;
	}
	return left->operation != HFS_POSIX_TICKET_READ ||
		right->operation != HFS_POSIX_TICKET_READ;
}

/* Emit one ticket lifecycle trace with stable request identity fields. */
static void hfsPosixTicketTrace(const HfsPosixIoTicket *ticket,
					HfsTracePhase phase, int result)
{
	HfsTraceOperation operation;

	if (ticket == NULL) {
		return;
	}
	operation = ticket->operation == HFS_POSIX_TICKET_READ
		? HFS_TRACE_OPERATION_READ : HFS_TRACE_OPERATION_WRITE;
	hfsTraceEmitDetail(phase, operation, HFS_TRACE_BUFFER_NONE,
		ticket->ticketId, ticket->parentId,
		ticket->file == NULL ? 0 : ticket->file->inodeId,
		ticket->offset, ticket->length, 0, ticket->pendingDependencies,
		0, 0, 0, 0, 0, HFS_TRACE_WAKE_NONE, 0, 0, result);
}

/* Initialize the wait state owned by one synchronous caller. */
int hfsPosixTicketInit(HfsPosixIoTicket *ticket, HfsInodeRuntime *runtime,
			      HfsFile *file, uint64_t offset, uint64_t length,
			      HfsPosixTicketOperation operation, uint64_t parentId)
{
	int rc;

	if (ticket == NULL || runtime == NULL || length == 0 ||
		offset > UINT64_MAX - length) {
		errno = EINVAL;
		return -1;
	}
	memset(ticket, 0, sizeof(*ticket));
	ticket->runtime = runtime;
	ticket->file = file;
	ticket->offset = offset;
	ticket->length = length;
	ticket->operation = operation;
	ticket->parentId = parentId;
	rc = pthread_mutex_init(&ticket->waitLock, NULL);
	if (rc != 0) {
		errno = rc;
		return -1;
	}
	rc = pthread_cond_init(&ticket->waitCondition, NULL);
	if (rc != 0) {
		pthread_mutex_destroy(&ticket->waitLock);
		errno = rc;
		return -1;
	}
	return 0;
}

/* Insert a ticket and compute conflicts without holding a backend lock. */
int hfsPosixTicketAdmit(HfsPosixIoTicket *ticket)
{
	HfsPosixIoTicket *current;
	HfsInodeRuntime *runtime;
	uint32_t scanItems = 0;
	uint64_t admitStartNs = hfsStatsTimingFastEnabled() ? hfsStatsNowNs() : 0;
	uint64_t lockStartNs = admitStartNs;
	uint64_t scanStartNs;
	uint64_t scanEndNs;
	uint64_t insertStartNs;
	uint64_t admitNs;

	if (ticket == NULL || ticket->runtime == NULL) {
		errno = EINVAL;
		return -1;
	}
	runtime = ticket->runtime;
	pthread_once(&g_hfsPosixTicketConfigOnce, hfsPosixTicketConfigInit);
	pthread_mutex_lock(&runtime->ticketLock);
	while (!runtime->ticketStopping &&
	       runtime->ticketCount >= g_hfsPosixTicketLimit) {
		pthread_cond_wait(&runtime->ticketAvailable, &runtime->ticketLock);
	}
	if (runtime->ticketStopping) {
		pthread_mutex_unlock(&runtime->ticketLock);
		errno = ESHUTDOWN;
		return -1;
	}
	if (lockStartNs != 0) {
		uint64_t lockNow = hfsStatsNowNs();

		hfsStatsRecordTicketMetric(HFS_STATS_TICKET_ADMIT_LOCK_WAIT,
			lockNow >= lockStartNs ? lockNow - lockStartNs : 0);
	}
	ticket->ticketId = ++runtime->nextTicketId;
	ticket->submitSeq = ++runtime->nextTicketSeq;
	scanStartNs = lockStartNs != 0 ? hfsStatsNowNs() : 0;
	for (current = runtime->ticketHead; current != NULL;
	     current = current->next) {
		scanItems++;
		if (current->submitSeq < ticket->submitSeq &&
		    hfsPosixTicketConflicts(current, ticket)) {
			ticket->pendingDependencies++;
		}
	}
	scanEndNs = scanStartNs != 0 ? hfsStatsNowNs() : 0;
	insertStartNs = scanEndNs;
	ticket->next = runtime->ticketHead;
	runtime->ticketHead = ticket;
	runtime->ticketCount++;
	runtime->ticketLimit = g_hfsPosixTicketLimit;
	ticket->admitted = true;
	ticket->ready = ticket->pendingDependencies == 0;
	ticket->dependencyWait = !ticket->ready;
	pthread_mutex_unlock(&runtime->ticketLock);
	admitNs = admitStartNs != 0 ? hfsStatsNowNs() : 0;
	if (scanStartNs != 0) {
		hfsStatsRecordTicketMetric(HFS_STATS_TICKET_ADMIT_SCAN,
			scanEndNs >= scanStartNs ? scanEndNs - scanStartNs : 0);
		hfsStatsRecordTicketMetric(HFS_STATS_TICKET_ADMIT_INSERT,
			admitNs >= insertStartNs ? admitNs - insertStartNs : 0);
	}
	hfsStatsRecordTicketAdmission(scanItems, ticket->pendingDependencies,
		runtime->ticketCount,
		admitNs >= admitStartNs ? admitNs - admitStartNs : 0);
	hfsStatsRecordTicketAdmit(ticket->pendingDependencies,
		runtime->ticketCount);
	hfsPosixTicketTrace(ticket, HFS_TRACE_TICKET_ADMIT, 0);
	if (ticket->ready) {
		hfsStatsRecordTicketReady();
		hfsPosixTicketTrace(ticket, HFS_TRACE_TICKET_READY, 0);
	}
	return 0;
}

/* Wait only for dependency promotion; no range lock is held here. */
int hfsPosixTicketWaitReady(HfsPosixIoTicket *ticket)
{
	uint64_t waitStartNs;
	int result;
	bool slept = false;

	if (ticket == NULL || !ticket->admitted) {
		errno = EINVAL;
		return -1;
	}
	waitStartNs = hfsStatsTimingFastEnabled() ? hfsStatsNowNs() : 0;
	pthread_mutex_lock(&ticket->waitLock);
	while (!ticket->ready && !ticket->done) {
		slept = true;
		pthread_cond_wait(&ticket->waitCondition, &ticket->waitLock);
	}
	result = ticket->result;
	pthread_mutex_unlock(&ticket->waitLock);
	if (ticket->dependencyWait && slept) {
		uint64_t waitNow = waitStartNs != 0 ? hfsStatsNowNs() : 0;
		uint64_t waitNs = waitNow >= waitStartNs ? waitNow - waitStartNs : 0;

		hfsStatsRecordTicketDependencyWait(waitNs);
		hfsStatsRecordTicketRealWait(waitNs);
		hfsStatsRecordTicketMetric(HFS_STATS_TICKET_DEPENDENCY_WAIT, waitNs);
		hfsPosixTicketTrace(ticket, HFS_TRACE_TICKET_DEP_WAIT, result);
	}
	return result == 0 ? 0 : -1;
}

/* Remove a completed ticket and promote all now-unblocked dependents. */
void hfsPosixTicketComplete(HfsPosixIoTicket *ticket, int result)
{
	HfsPosixIoTicket *current;
	HfsPosixIoTicket **link;
	HfsInodeRuntime *runtime;
	uint32_t promoted = 0;
	uint32_t scanItems = 0;
	uint64_t completeStartNs = hfsStatsTimingFastEnabled() ? hfsStatsNowNs() : 0;
	uint64_t completeLockStartNs = completeStartNs;
	uint64_t scanStartNs;
	uint64_t scanEndNs;
	uint64_t wakeStartNs;
	uint64_t wakeEndNs;

	if (ticket == NULL || !ticket->admitted || ticket->runtime == NULL) {
		return;
	}
	runtime = ticket->runtime;
	pthread_mutex_lock(&runtime->ticketLock);
	if (completeLockStartNs != 0) {
		uint64_t lockNow = hfsStatsNowNs();

		hfsStatsRecordTicketMetric(HFS_STATS_TICKET_COMPLETE_LOCK_WAIT,
			lockNow >= completeLockStartNs ? lockNow - completeLockStartNs : 0);
	}
	scanStartNs = completeLockStartNs != 0 ? hfsStatsNowNs() : 0;
	link = &runtime->ticketHead;
	while (*link != NULL && *link != ticket) {
		scanItems++;
		link = &(*link)->next;
	}
	if (*link == ticket) {
		*link = ticket->next;
		if (runtime->ticketCount != 0) {
			runtime->ticketCount--;
		}
	}
	for (current = runtime->ticketHead; current != NULL;
	     current = current->next) {
		scanItems++;
		if (current->submitSeq <= ticket->submitSeq ||
		    current->pendingDependencies == 0 || current->ready ||
		    !hfsPosixTicketConflicts(ticket, current)) {
			continue;
		}
		current->pendingDependencies--;
		if (current->pendingDependencies == 0) {
			pthread_mutex_lock(&current->waitLock);
			current->ready = true;
			pthread_cond_broadcast(&current->waitCondition);
			pthread_mutex_unlock(&current->waitLock);
			promoted++;
			hfsStatsRecordTicketReady();
			hfsPosixTicketTrace(current, HFS_TRACE_TICKET_READY, 0);
		}
	}
	scanEndNs = scanStartNs != 0 ? hfsStatsNowNs() : 0;
	pthread_cond_broadcast(&runtime->ticketAvailable);
	pthread_mutex_unlock(&runtime->ticketLock);
	{
		uint64_t completeNow = completeStartNs != 0 ? hfsStatsNowNs() : 0;
		uint64_t completeNs = completeNow >= completeStartNs ?
			completeNow - completeStartNs : 0;

		hfsStatsRecordTicketCompletion(completeNs,
			scanEndNs >= scanStartNs ? scanEndNs - scanStartNs : completeNs,
			scanItems);
	}
	if (promoted != 0) {
		hfsStatsRecordTicketPromote(promoted);
	}
	wakeStartNs = completeStartNs != 0 ? hfsStatsNowNs() : 0;
	pthread_mutex_lock(&ticket->waitLock);
	ticket->result = result;
	ticket->done = true;
	pthread_cond_broadcast(&ticket->waitCondition);
	pthread_mutex_unlock(&ticket->waitLock);
	wakeEndNs = wakeStartNs != 0 ? hfsStatsNowNs() : 0;
	if (wakeStartNs != 0) {
		hfsStatsRecordTicketMetric(HFS_STATS_TICKET_WAKE_PROCESSING,
			wakeEndNs >= wakeStartNs ? wakeEndNs - wakeStartNs : 0);
	}
	hfsStatsRecordTicketComplete();
	hfsStatsRecordTicketWake();
	hfsPosixTicketTrace(ticket, HFS_TRACE_TICKET_WAKE, result);
}

/* Destroy one ticket after it has been removed from the active list. */
void hfsPosixTicketDestroy(HfsPosixIoTicket *ticket)
{
	if (ticket == NULL) {
		return;
	}
	pthread_cond_destroy(&ticket->waitCondition);
	pthread_mutex_destroy(&ticket->waitLock);
	memset(ticket, 0, sizeof(*ticket));
}
