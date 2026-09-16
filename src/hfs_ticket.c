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

/* Parse the bounded scheduler configuration once per process. */
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

/* Return true when ticket ordering is selected by the compatibility parser. */
bool hfsPosixTicketEnabled(void)
{
	pthread_once(&g_hfsPosixTicketConfigOnce, hfsPosixTicketConfigInit);
	return g_hfsPosixTicketEnabled;
}

/* Return the compatibility ordering mode selected from the environment. */
HfsPosixOrderingMode hfsPosixOrderingMode(void)
{
	pthread_once(&g_hfsPosixTicketConfigOnce, hfsPosixTicketConfigInit);
	return g_hfsPosixOrderingMode;
}

/* Return an exclusive end offset for one validated interval. */
static uint64_t hfsPosixTicketEnd(const HfsPosixIoTicket *ticket)
{
	return ticket->offset + ticket->length;
}

/* Return true when two scheduler operations require completion ordering. */
static bool hfsPosixTicketConflicts(const HfsPosixIoTicket *left,
					const HfsPosixIoTicket *right)
{
	uint64_t leftEnd;
	uint64_t rightEnd;

	if (left == NULL || right == NULL || left->length == 0 ||
	    right->length == 0) {
		return false;
	}
	if (left->operation == HFS_POSIX_TICKET_FENCE ||
	    right->operation == HFS_POSIX_TICKET_FENCE) {
		return true;
	}
	leftEnd = hfsPosixTicketEnd(left);
	rightEnd = hfsPosixTicketEnd(right);
	if (left->offset >= rightEnd || right->offset >= leftEnd) {
		return false;
	}
	return left->operation != HFS_POSIX_TICKET_READ ||
		right->operation != HFS_POSIX_TICKET_READ;
}

/* Order intrusive interval nodes by offset and their unique submit sequence. */
static int hfsPosixTicketCompare(const HfsPosixIoTicket *left,
					 const HfsPosixIoTicket *right)
{
	if (left->offset < right->offset) {
		return -1;
	}
	if (left->offset > right->offset) {
		return 1;
	}
	if (left->submitSeq < right->submitSeq) {
		return -1;
	}
	if (left->submitSeq > right->submitSeq) {
		return 1;
	}
	return 0;
}

/* Refresh the augmented maximum-end field after a tree mutation. */
static void hfsPosixTicketUpdate(HfsPosixIoTicket *ticket)
{
	uint64_t maxEnd;

	if (ticket == NULL) {
		return;
	}
	maxEnd = hfsPosixTicketEnd(ticket);
	if (ticket->left != NULL && ticket->left->maxEnd > maxEnd) {
		maxEnd = ticket->left->maxEnd;
	}
	if (ticket->right != NULL && ticket->right->maxEnd > maxEnd) {
		maxEnd = ticket->right->maxEnd;
	}
	ticket->maxEnd = maxEnd;
}

/* Rotate a treap node toward the side with the smaller priority. */
static HfsPosixIoTicket *hfsPosixTicketRotateRight(HfsPosixIoTicket *root)
{
	HfsPosixIoTicket *pivot = root->left;

	root->left = pivot->right;
	pivot->right = root;
	hfsPosixTicketUpdate(root);
	hfsPosixTicketUpdate(pivot);
	return pivot;
}

/* Rotate a treap node toward the side with the smaller priority. */
static HfsPosixIoTicket *hfsPosixTicketRotateLeft(HfsPosixIoTicket *root)
{
	HfsPosixIoTicket *pivot = root->right;

	root->right = pivot->left;
	pivot->left = root;
	hfsPosixTicketUpdate(root);
	hfsPosixTicketUpdate(pivot);
	return pivot;
}

/* Merge two treap subtrees while preserving the interval ordering key. */
static HfsPosixIoTicket *hfsPosixTicketMerge(HfsPosixIoTicket *left,
						HfsPosixIoTicket *right)
{
	if (left == NULL) {
		return right;
	}
	if (right == NULL) {
		return left;
	}
	if (left->priority <= right->priority) {
		left->right = hfsPosixTicketMerge(left->right, right);
		hfsPosixTicketUpdate(left);
		return left;
	}
	right->left = hfsPosixTicketMerge(left, right->left);
	hfsPosixTicketUpdate(right);
	return right;
}

/* Insert one intrusive operation node into the expected-logarithmic treap. */
static HfsPosixIoTicket *hfsPosixTicketInsert(HfsPosixIoTicket *root,
						HfsPosixIoTicket *ticket)
{
	int comparison;

	if (root == NULL) {
		return ticket;
	}
	comparison = hfsPosixTicketCompare(ticket, root);
	if (comparison < 0) {
		root->left = hfsPosixTicketInsert(root->left, ticket);
		if (root->left->priority < root->priority) {
			return hfsPosixTicketRotateRight(root);
		}
	} else {
		root->right = hfsPosixTicketInsert(root->right, ticket);
		if (root->right->priority < root->priority) {
			return hfsPosixTicketRotateLeft(root);
		}
	}
	hfsPosixTicketUpdate(root);
	return root;
}

/* Remove one operation node from the intrusive treap. */
static HfsPosixIoTicket *hfsPosixTicketRemove(HfsPosixIoTicket *root,
						HfsPosixIoTicket *ticket)
{
	int comparison;

	if (root == NULL) {
		return NULL;
	}
	comparison = hfsPosixTicketCompare(ticket, root);
	if (comparison < 0) {
		root->left = hfsPosixTicketRemove(root->left, ticket);
	} else if (comparison > 0) {
		root->right = hfsPosixTicketRemove(root->right, ticket);
	} else if (root == ticket) {
		return hfsPosixTicketMerge(root->left, root->right);
	} else {
		/* Unique submit sequences make this branch unreachable; fail closed. */
		return root;
	}
	hfsPosixTicketUpdate(root);
	return root;
}

/* Mix a ticket id into a deterministic treap priority without global RNG state. */
static uint32_t hfsPosixTicketPriority(uint64_t ticketId)
{
	uint64_t value = ticketId + UINT64_C(0x9e3779b97f4a7c15);

	value = (value ^ (value >> 30)) * UINT64_C(0xbf58476d1ce4e5b9);
	value = (value ^ (value >> 27)) * UINT64_C(0x94d049bb133111eb);
	value ^= value >> 31;
	return (uint32_t)(value ^ (value >> 32));
}

typedef void (*HfsPosixTicketVisitFn)(HfsPosixIoTicket *ticket, void *arg);

/* Carry the admission node and candidate count through one overlap walk. */
typedef struct HfsPosixTicketAdmission {
	HfsPosixIoTicket *ticket;
	uint32_t *scanItems;
} HfsPosixTicketAdmission;

/* Carry the completion node and promotion count through one overlap walk. */
typedef struct HfsPosixTicketPromotion {
	HfsPosixIoTicket *completed;
	uint32_t *promoted;
	uint32_t *scanItems;
} HfsPosixTicketPromotion;

/* Visit only intervals that overlap the query, pruning by maxEnd and offset. */
static void hfsPosixTicketVisitOverlaps(HfsPosixIoTicket *root,
						uint64_t offset, uint64_t end,
						HfsPosixTicketVisitFn visit, void *arg)
{
	if (root == NULL || root->maxEnd <= offset) {
		return;
	}
	if (root->left != NULL && root->left->maxEnd > offset) {
		hfsPosixTicketVisitOverlaps(root->left, offset, end, visit, arg);
	}
	if (root->offset < end && hfsPosixTicketEnd(root) > offset) {
		visit(root, arg);
	}
	if (root->right != NULL && root->offset < end) {
		hfsPosixTicketVisitOverlaps(root->right, offset, end, visit, arg);
	}
}

/* Emit one scheduler lifecycle trace with stable request identity fields. */
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

/* Count earlier conflicting intervals during admission. */
static void hfsPosixTicketCountDependency(HfsPosixIoTicket *current,
						void *arg)
{
	HfsPosixTicketAdmission *admission = arg;
	HfsPosixIoTicket *ticket = admission->ticket;

	(*admission->scanItems)++;

	if (current->submitSeq < ticket->submitSeq &&
	    hfsPosixTicketConflicts(current, ticket)) {
		ticket->pendingDependencies++;
	}
}

/* Promote only later intervals that conflict with the completed operation. */
static void hfsPosixTicketPromoteDependency(HfsPosixIoTicket *current,
						void *arg)
{
	HfsPosixTicketPromotion *promotion = arg;
	HfsPosixIoTicket *completed = promotion->completed;

	(*promotion->scanItems)++;

	if (current->submitSeq <= completed->submitSeq ||
	    current->pendingDependencies == 0 || current->ready ||
	    !hfsPosixTicketConflicts(completed, current)) {
		return;
	}
	current->pendingDependencies--;
	if (current->pendingDependencies == 0) {
		current->ready = true;
		(*promotion->promoted)++;
		hfsStatsRecordTicketReady();
		hfsPosixTicketTrace(current, HFS_TRACE_TICKET_READY, 0);
	}
}

/* Initialize one stack-owned scheduler operation without per-ticket primitives. */
int hfsPosixTicketInit(HfsPosixIoTicket *ticket, HfsInodeRuntime *runtime,
			      HfsFile *file, uint64_t offset, uint64_t length,
			      HfsPosixTicketOperation operation, uint64_t parentId)
{
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
	return 0;
}

/* Insert an interval and compute dependencies in O(log A + K) expected time. */
int hfsPosixTicketAdmit(HfsPosixIoTicket *ticket)
{
	HfsInodeScheduler *scheduler;
	uint32_t scanItems = 0;
	uint32_t activeCount;
	uint64_t admitStartNs;
	uint64_t scanStartNs;
	uint64_t scanEndNs;
	uint64_t insertStartNs;
	uint64_t admitNs;
	HfsPosixTicketAdmission admission = { ticket, &scanItems };

	if (ticket == NULL || ticket->runtime == NULL) {
		errno = EINVAL;
		return -1;
	}
	scheduler = &ticket->runtime->scheduler;
	pthread_once(&g_hfsPosixTicketConfigOnce, hfsPosixTicketConfigInit);
	scheduler->limit = g_hfsPosixTicketLimit;
	admitStartNs = hfsStatsTimingFastEnabled() ? hfsStatsNowNs() : 0;
	scanStartNs = 0;
	scanEndNs = 0;
	insertStartNs = 0;
	pthread_mutex_lock(&scheduler->lock);
	while (!scheduler->stopping && scheduler->count >= scheduler->limit) {
		pthread_cond_wait(&scheduler->condition, &scheduler->lock);
	}
	if (scheduler->stopping) {
		pthread_mutex_unlock(&scheduler->lock);
		errno = ESHUTDOWN;
		return -1;
	}
	ticket->ticketId = ++scheduler->nextTicketId;
	ticket->submitSeq = ++scheduler->nextTicketSeq;
	ticket->submitEpoch = ++scheduler->epoch;
	ticket->priority = hfsPosixTicketPriority(ticket->ticketId);
	ticket->maxEnd = hfsPosixTicketEnd(ticket);
	scanStartNs = admitStartNs != 0 ? hfsStatsNowNs() : 0;
	hfsPosixTicketVisitOverlaps(scheduler->root, ticket->offset,
		hfsPosixTicketEnd(ticket), hfsPosixTicketCountDependency, &admission);
	scanEndNs = scanStartNs != 0 ? hfsStatsNowNs() : 0;
	insertStartNs = scanEndNs;
	scheduler->root = hfsPosixTicketInsert(scheduler->root, ticket);
	scheduler->count++;
	ticket->admitted = true;
	ticket->ready = ticket->pendingDependencies == 0;
	ticket->dependencyWait = !ticket->ready;
	activeCount = scheduler->count;
	/* Trace while the scheduler lock protects pendingDependencies. */
	hfsPosixTicketTrace(ticket, HFS_TRACE_TICKET_ADMIT, 0);
	if (ticket->ready) {
		hfsStatsRecordTicketReady();
		hfsPosixTicketTrace(ticket, HFS_TRACE_TICKET_READY, 0);
	}
	pthread_mutex_unlock(&scheduler->lock);
	admitNs = admitStartNs != 0 ? hfsStatsNowNs() : 0;
	if (scanStartNs != 0) {
		hfsStatsRecordTicketMetric(HFS_STATS_TICKET_ADMIT_SCAN,
			scanEndNs >= scanStartNs ? scanEndNs - scanStartNs : 0);
		hfsStatsRecordTicketMetric(HFS_STATS_TICKET_ADMIT_INSERT,
			admitNs >= insertStartNs ? admitNs - insertStartNs : 0);
	}
	hfsStatsRecordTicketAdmission(scanItems, ticket->pendingDependencies,
		activeCount, admitNs >= admitStartNs ? admitNs - admitStartNs : 0);
	hfsStatsRecordTicketAdmit(ticket->pendingDependencies, activeCount);
	return 0;
}

/* Wait on the inode scheduler condition until this operation becomes ready. */
int hfsPosixTicketWaitReady(HfsPosixIoTicket *ticket)
{
	HfsInodeScheduler *scheduler;
	uint64_t waitStartNs;
	int result;
	bool slept = false;

	if (ticket == NULL || !ticket->admitted || ticket->runtime == NULL) {
		errno = EINVAL;
		return -1;
	}
	scheduler = &ticket->runtime->scheduler;
	waitStartNs = hfsStatsTimingFastEnabled() ? hfsStatsNowNs() : 0;
	pthread_mutex_lock(&scheduler->lock);
	while (!ticket->ready && !ticket->done && !scheduler->stopping) {
		slept = true;
		pthread_cond_wait(&scheduler->condition, &scheduler->lock);
	}
	if (!ticket->ready && !ticket->done && scheduler->stopping) {
		result = -ESHUTDOWN;
	} else {
		result = ticket->result;
	}
	pthread_mutex_unlock(&scheduler->lock);
	if (ticket->dependencyWait && slept) {
		uint64_t waitNow = waitStartNs != 0 ? hfsStatsNowNs() : 0;
		uint64_t waitNs = waitNow >= waitStartNs ? waitNow - waitStartNs : 0;

		hfsStatsRecordTicketDependencyWait(waitNs);
		hfsStatsRecordTicketRealWait(waitNs);
		hfsStatsRecordTicketMetric(HFS_STATS_TICKET_DEPENDENCY_WAIT, waitNs);
		hfsPosixTicketTrace(ticket, HFS_TRACE_TICKET_DEP_WAIT, result);
	}
	if (result != 0) {
		errno = result < 0 ? -result : result;
		return -1;
	}
	return 0;
}

/* Remove a completed interval and promote only its affected successors. */
void hfsPosixTicketComplete(HfsPosixIoTicket *ticket, int result)
{
	HfsInodeScheduler *scheduler;
	uint32_t promoted = 0;
	uint32_t scanItems = 0;
	uint64_t completeStartNs;
	uint64_t completeLockStartNs;
	uint64_t scanStartNs;
	uint64_t scanEndNs;
	uint64_t completeNow;
	uint64_t completeNs;
	HfsPosixTicketPromotion promotion = { ticket, &promoted, &scanItems };

	if (ticket == NULL || !ticket->admitted || ticket->runtime == NULL) {
		return;
	}
	scheduler = &ticket->runtime->scheduler;
	completeStartNs = hfsStatsTimingFastEnabled() ? hfsStatsNowNs() : 0;
	completeLockStartNs = completeStartNs;
	pthread_mutex_lock(&scheduler->lock);
	if (completeLockStartNs != 0) {
		uint64_t lockNow = hfsStatsNowNs();

		hfsStatsRecordTicketMetric(HFS_STATS_TICKET_COMPLETE_LOCK_WAIT,
			lockNow >= completeLockStartNs ? lockNow - completeLockStartNs : 0);
	}
	scanStartNs = completeLockStartNs != 0 ? hfsStatsNowNs() : 0;
	scheduler->root = hfsPosixTicketRemove(scheduler->root, ticket);
	if (scheduler->count != 0) {
		scheduler->count--;
	}
	ticket->admitted = false;
	hfsPosixTicketVisitOverlaps(scheduler->root, ticket->offset,
		hfsPosixTicketEnd(ticket), hfsPosixTicketPromoteDependency,
		&promotion);
	scanEndNs = scanStartNs != 0 ? hfsStatsNowNs() : 0;
	ticket->result = result;
	ticket->done = true;
	pthread_cond_broadcast(&scheduler->condition);
	hfsPosixTicketTrace(ticket, HFS_TRACE_TICKET_WAKE, result);
	pthread_mutex_unlock(&scheduler->lock);
	completeNow = completeStartNs != 0 ? hfsStatsNowNs() : 0;
	completeNs = completeNow >= completeStartNs ?
		completeNow - completeStartNs : 0;
	hfsStatsRecordTicketCompletion(completeNs,
		scanEndNs >= scanStartNs ? scanEndNs - scanStartNs : completeNs,
		scanItems);
	if (promoted != 0) {
		hfsStatsRecordTicketPromote(promoted);
	}
	hfsStatsRecordTicketComplete();
	hfsStatsRecordTicketWake();
}

/* Destroy one stack-owned ticket; scheduler state has already been removed. */
void hfsPosixTicketDestroy(HfsPosixIoTicket *ticket)
{
	if (ticket != NULL) {
		memset(ticket, 0, sizeof(*ticket));
	}
}
