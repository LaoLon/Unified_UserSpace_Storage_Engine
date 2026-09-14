#include "hfs_profile.h"

#include <stddef.h>
#include <string.h>

#include <spdk/env.h>

#include "hfs_stats.h"
#include "hfs_trace.h"

static _Atomic uint64_t g_hfsProfileId;
static _Thread_local HfsIoProfile *g_hfsProfileCurrent;

/* Return the monotonic SPDK tick frequency, with a safe fallback. */
uint64_t hfsProfileTicksHz(void)
{
	uint64_t ticksHz = spdk_get_ticks_hz();

	return ticksHz == 0 ? UINT64_C(1000000000) : ticksHz;
}

/* Return the single tick source used by all T0..T11 measurements. */
uint64_t hfsProfileNow(void)
{
	return spdk_get_ticks();
}

/* Keep timing disabled unless HFS_PROFILE=timing or stats timing is enabled. */
bool hfsProfileTimingEnabled(void)
{
	return hfsStatsTimingFastEnabled();
}

/* Return the profile currently owned by the calling application thread. */
HfsIoProfile *hfsProfileCurrent(void)
{
	return g_hfsProfileCurrent;
}

/* Publish the profile used by the synchronous HFS call stack. */
void hfsProfileSetCurrent(HfsIoProfile *profile)
{
	g_hfsProfileCurrent = profile;
}

/* Store the first timestamp observed for one parent stage. */
static void hfsProfileStoreFirst(HfsIoProfile *profile,
				 HfsProfileStage stage, uint64_t tick)
{
	uint64_t expected = 0;

	if (profile == NULL || !profile->enabled || tick == 0 ||
		stage >= HFS_PROFILE_STAGE_COUNT) {
		return;
	}
	(void)atomic_compare_exchange_strong_explicit(&profile->ticks[stage],
		&expected, tick, memory_order_relaxed, memory_order_relaxed);
}

/* Store the latest timestamp observed for one parent stage. */
static void hfsProfileStoreLast(HfsIoProfile *profile,
				HfsProfileStage stage, uint64_t tick)
{
	uint64_t current;

	if (profile == NULL || !profile->enabled || tick == 0 ||
		stage >= HFS_PROFILE_STAGE_COUNT) {
		return;
	}
	current = atomic_load_explicit(&profile->ticks[stage], memory_order_relaxed);
	while (current < tick && !atomic_compare_exchange_weak_explicit(
		&profile->ticks[stage], &current, tick, memory_order_relaxed,
		memory_order_relaxed)) {
		/* Retry after another child published a later timestamp. */
	}
}

/* Store an application-owned stage exactly once. */
void hfsProfileStage(HfsIoProfile *profile, HfsProfileStage stage)
{
	if (profile == NULL || !profile->enabled || stage >= HFS_PROFILE_STAGE_COUNT) {
		return;
	}
	atomic_store_explicit(&profile->ticks[stage], hfsProfileNow(),
		memory_order_relaxed);
}

/* Store a stage on the current application profile. */
void hfsProfileParentStage(HfsProfileStage stage)
{
	hfsProfileStage(g_hfsProfileCurrent, stage);
}

/* Begin one logical request without allocating a profile object. */
bool hfsProfileBegin(HfsIoProfile *profile, bool write, size_t bytes)
{
	if (profile == NULL || !hfsProfileTimingEnabled()) {
		return false;
	}
	memset(profile, 0, sizeof(*profile));
	profile->enabled = true;
	profile->parentId = atomic_fetch_add_explicit(&g_hfsProfileId, 1,
		memory_order_relaxed) + 1U;
	profile->bytes = bytes;
	if (write) {
		profile->flags |= HFS_PROFILE_FLAG_WRITE;
	}
	atomic_store_explicit(&profile->ticks[HFS_PROFILE_T0], hfsProfileNow(),
		memory_order_relaxed);
	return true;
}

/* Finish one profile and append a single parent row to the trace snapshot. */
void hfsProfileFinish(HfsIoProfile *profile, int32_t result)
{
	uint64_t ticks[HFS_PROFILE_STAGE_COUNT];
	bool monotonic = true;
	unsigned int i;

	if (profile == NULL || !profile->enabled) {
		return;
	}
	profile->result = result;
	atomic_store_explicit(&profile->ticks[HFS_PROFILE_T11], hfsProfileNow(),
		memory_order_relaxed);
	for (i = 0; i < HFS_PROFILE_STAGE_COUNT; i++) {
		ticks[i] = atomic_load_explicit(&profile->ticks[i], memory_order_relaxed);
		if (i != 0 && ticks[i] != 0 && ticks[i - 1U] != 0 &&
			ticks[i] < ticks[i - 1U]) {
			monotonic = false;
		}
	}
	hfsTraceEmitProfile(profile->parentId, profile->bytes, profile->flags,
		atomic_load_explicit(&profile->childSubmitted, memory_order_relaxed),
		ticks, monotonic, result);
}

/* Initialize one child profile that is private to one HfsIoRequest. */
void hfsProfileChildInit(HfsIoChildProfile *child, HfsIoProfile *parent,
				uint32_t reactorId)
{
	if (child == NULL) {
		return;
	}
	memset(child, 0, sizeof(*child));
	child->parent = parent;
	child->reactorId = reactorId;
}

/* Record the child enqueue boundary and the first parent enqueue. */
void hfsProfileChildEnqueue(HfsIoChildProfile *child, uint64_t requestId)
{
	uint64_t tick;

	if (child == NULL || child->parent == NULL || !child->parent->enabled) {
		return;
	}
	tick = hfsProfileNow();
	if (requestId != 0) {
		child->requestId = requestId;
	}
	child->ticks[HFS_PROFILE_T4] = tick;
	hfsProfileStoreFirst(child->parent, HFS_PROFILE_T4, tick);
}

/* Record the dequeue boundary on the reactor that owns the child. */
void hfsProfileChildDequeue(void *arg)
{
	HfsIoChildProfile *child = arg;
	uint64_t tick;

	if (child == NULL || child->parent == NULL || !child->parent->enabled) {
		return;
	}
	tick = hfsProfileNow();
	child->ticks[HFS_PROFILE_T5] = tick;
	hfsProfileStoreFirst(child->parent, HFS_PROFILE_T5, tick);
}

/* Record the SPDK submit call boundary on one child. */
void hfsProfileChildSubmit(void *arg)
{
	HfsIoChildProfile *child = arg;
	uint64_t tick;

	if (child == NULL || child->parent == NULL || !child->parent->enabled) {
		return;
	}
	tick = hfsProfileNow();
	child->ticks[HFS_PROFILE_T6] = tick;
	hfsProfileStoreFirst(child->parent, HFS_PROFILE_T6, tick);
}

/* Record the SPDK callback entry boundary on one child. */
void hfsProfileChildComplete(void *arg)
{
	HfsIoChildProfile *child = arg;
	uint64_t tick;

	if (child == NULL || child->parent == NULL || !child->parent->enabled) {
		return;
	}
	tick = hfsProfileNow();
	child->ticks[HFS_PROFILE_T7] = tick;
	hfsProfileStoreLast(child->parent, HFS_PROFILE_T7, tick);
}

/* Record completion publication and wake issuance on one child. */
void hfsProfileChildWake(void *arg)
{
	HfsIoChildProfile *child = arg;
	uint64_t tick;

	if (child == NULL || child->parent == NULL || !child->parent->enabled) {
		return;
	}
	tick = hfsProfileNow();
	child->ticks[HFS_PROFILE_T8] = tick;
	hfsProfileStoreLast(child->parent, HFS_PROFILE_T8, tick);
	atomic_fetch_add_explicit(&child->parent->childCompleted, 1,
		memory_order_relaxed);
	if (child->ticks[HFS_PROFILE_T7] != 0 && tick >= child->ticks[HFS_PROFILE_T7]) {
		hfsStatsRecordCompletionToWake(tick - child->ticks[HFS_PROFILE_T7]);
	}
}

/* Record the return from the application's futex wait primitive. */
void hfsProfileChildWaitReturn(void *arg)
{
	HfsIoChildProfile *child = arg;
	uint64_t tick;

	if (child == NULL || child->parent == NULL || !child->parent->enabled) {
		return;
	}
	tick = hfsProfileNow();
	child->ticks[HFS_PROFILE_T9] = tick;
	hfsProfileStoreLast(child->parent, HFS_PROFILE_T9, tick);
	if (child->ticks[HFS_PROFILE_T8] != 0 && tick >= child->ticks[HFS_PROFILE_T8]) {
		hfsStatsRecordWakeToResume(tick - child->ticks[HFS_PROFILE_T8]);
	}
}

/* Increment the parent child count after a successful nonblocking submit. */
void hfsProfileChildSubmitted(HfsIoProfile *profile)
{
	if (profile != NULL && profile->enabled) {
		atomic_fetch_add_explicit(&profile->childSubmitted, 1,
			memory_order_relaxed);
	}
}
