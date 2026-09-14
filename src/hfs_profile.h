#ifndef HFS_PROFILE_H
#define HFS_PROFILE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdatomic.h>
#include <stdint.h>

/* The profiled POSIX timeline has one timestamp for each T0..T11 stage. */
#define HFS_PROFILE_STAGE_COUNT 12U

typedef enum HfsProfileStage {
	HFS_PROFILE_T0 = 0,
	HFS_PROFILE_T1,
	HFS_PROFILE_T2,
	HFS_PROFILE_T3,
	HFS_PROFILE_T4,
	HFS_PROFILE_T5,
	HFS_PROFILE_T6,
	HFS_PROFILE_T7,
	HFS_PROFILE_T8,
	HFS_PROFILE_T9,
	HFS_PROFILE_T10,
	HFS_PROFILE_T11
} HfsProfileStage;

/* Flags describe the selected POSIX path without changing request ownership. */
enum HfsProfileFlags {
	HFS_PROFILE_FLAG_WRITE = 1U << 0,
	HFS_PROFILE_FLAG_DIRECT = 1U << 1,
	HFS_PROFILE_FLAG_STAGING = 1U << 2,
	HFS_PROFILE_FLAG_TICKET = 1U << 3,
	HFS_PROFILE_FLAG_DATA = 1U << 4
};

/* One stack-owned logical request profile shared by all child I/Os. */
typedef struct HfsIoProfile {
	uint64_t parentId;
	_Atomic uint64_t ticks[HFS_PROFILE_STAGE_COUNT];
	_Atomic uint32_t childSubmitted;
	_Atomic uint32_t childCompleted;
	uint64_t bytes;
	uint32_t flags;
	int32_t result;
	bool enabled;
} HfsIoProfile;

/* One request-local child profile prevents pipeline races on stage timestamps. */
typedef struct HfsIoChildProfile {
	HfsIoProfile *parent;
	uint64_t ticks[HFS_PROFILE_STAGE_COUNT];
	uint64_t requestId;
	uint32_t reactorId;
} HfsIoChildProfile;

/* Return true only when timing profiling is explicitly enabled. */
bool hfsProfileTimingEnabled(void);
/* Return the SPDK tick source used by all profile timestamps. */
uint64_t hfsProfileNow(void);
/* Return the tick frequency recorded for the current process. */
uint64_t hfsProfileTicksHz(void);
/* Return and set the current thread's stack profile. */
HfsIoProfile *hfsProfileCurrent(void);
void hfsProfileSetCurrent(HfsIoProfile *profile);

/* Initialize and finalize one logical request profile. */
bool hfsProfileBegin(HfsIoProfile *profile, bool write, size_t bytes);
void hfsProfileFinish(HfsIoProfile *profile, int32_t result);
/* Mark a parent stage using the current application thread. */
void hfsProfileParentStage(HfsProfileStage stage);
/* Mark a parent stage using the explicit profile pointer. */
void hfsProfileStage(HfsIoProfile *profile, HfsProfileStage stage);

/* Initialize a child profile before it can be submitted. */
void hfsProfileChildInit(HfsIoChildProfile *child, HfsIoProfile *parent,
				uint32_t reactorId);
void hfsProfileChildEnqueue(HfsIoChildProfile *child, uint64_t requestId);
void hfsProfileChildDequeue(void *child);
void hfsProfileChildSubmit(void *child);
void hfsProfileChildComplete(void *child);
void hfsProfileChildWake(void *child);
void hfsProfileChildWaitReturn(void *child);
/* Record one complete child submission at the parent boundary. */
void hfsProfileChildSubmitted(HfsIoProfile *profile);

#endif /* HFS_PROFILE_H */
