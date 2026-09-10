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

/* Parse the bounded ticket scheduler configuration once per process. */
static void hfsPosixTicketConfigInit(void)
{
	const char *enabled = getenv("HFS_POSIX_TICKET");
	const char *limit = getenv("HFS_POSIX_TICKET_DEPTH");
	char *end = NULL;
	unsigned long parsed;

	g_hfsPosixTicketEnabled = enabled == NULL ||
		(!strcasecmp(enabled, "1") || !strcasecmp(enabled, "true") ||
		 !strcasecmp(enabled, "yes") || !strcasecmp(enabled, "on"));
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
	ticket->ticketId = ++runtime->nextTicketId;
	ticket->submitSeq = ++runtime->nextTicketSeq;
	for (current = runtime->ticketHead; current != NULL;
	     current = current->next) {
		if (current->submitSeq < ticket->submitSeq &&
		    hfsPosixTicketConflicts(current, ticket)) {
			ticket->pendingDependencies++;
		}
	}
	ticket->next = runtime->ticketHead;
	runtime->ticketHead = ticket;
	runtime->ticketCount++;
	runtime->ticketLimit = g_hfsPosixTicketLimit;
	ticket->admitted = true;
	ticket->ready = ticket->pendingDependencies == 0;
	ticket->dependencyWait = !ticket->ready;
	pthread_mutex_unlock(&runtime->ticketLock);
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

	if (ticket == NULL || !ticket->admitted) {
		errno = EINVAL;
		return -1;
	}
	waitStartNs = hfsStatsNowNs();
	pthread_mutex_lock(&ticket->waitLock);
	while (!ticket->ready && !ticket->done) {
		pthread_cond_wait(&ticket->waitCondition, &ticket->waitLock);
	}
	result = ticket->result;
	pthread_mutex_unlock(&ticket->waitLock);
	if (ticket->dependencyWait) {
		hfsStatsRecordTicketDependencyWait(hfsStatsNowNs() - waitStartNs);
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

	if (ticket == NULL || !ticket->admitted || ticket->runtime == NULL) {
		return;
	}
	runtime = ticket->runtime;
	pthread_mutex_lock(&runtime->ticketLock);
	link = &runtime->ticketHead;
	while (*link != NULL && *link != ticket) {
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
	pthread_cond_broadcast(&runtime->ticketAvailable);
	pthread_mutex_unlock(&runtime->ticketLock);
	if (promoted != 0) {
		hfsStatsRecordTicketPromote(promoted);
	}
	pthread_mutex_lock(&ticket->waitLock);
	ticket->result = result;
	ticket->done = true;
	pthread_cond_broadcast(&ticket->waitCondition);
	pthread_mutex_unlock(&ticket->waitLock);
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
