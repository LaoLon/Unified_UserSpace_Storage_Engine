#ifndef HFS_DURABILITY_H
#define HFS_DURABILITY_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <pthread.h>

struct Hfs;
struct HfsOpenFile;

/* One caller waiting for a mount-level durability epoch. */
typedef struct HfsDurabilityRequest {
	struct HfsOpenFile *openFile;
	bool dataOnly;
	int result;
	int error;
	bool completed;
	pthread_mutex_t lock;
	pthread_cond_t done;
	struct HfsDurabilityRequest *next;
} HfsDurabilityRequest;

/* Batch callback executed by the coordinator worker for one HFS mount. */
typedef int (*HfsDurabilityBatchProcessor)(struct Hfs *fs,
		HfsDurabilityRequest **requests, size_t requestCount);

typedef struct HfsDurabilityCoordinator HfsDurabilityCoordinator;

/* Initialize the optional fsync ordering trace for one HFS process. */
void hfsDurabilityTraceInit(void);
/* Record one fsync phase without changing the caller-visible errno value. */
void hfsDurabilityTraceRecord(uint64_t inodeId, const char *phase, int result);
/* Write the optional trace after all HFS activity has stopped. */
void hfsDurabilityTraceFini(void);

/* Start the bounded group-commit worker for one mounted filesystem. */
int hfsDurabilityCoordinatorInit(struct Hfs *fs,
		HfsDurabilityBatchProcessor processor);
/* Drain queued durability requests and stop the group-commit worker. */
void hfsDurabilityCoordinatorStop(struct Hfs *fs);
/* Initialize one stack-owned request before submitting it to the worker. */
int hfsDurabilityRequestInit(HfsDurabilityRequest *request,
		struct HfsOpenFile *openFile, bool dataOnly);
/* Submit and synchronously wait for one request's batch result. */
int hfsDurabilityRequestSubmit(struct Hfs *fs,
		HfsDurabilityRequest *request);
/* Release the request synchronization primitives after submission returns. */
void hfsDurabilityRequestFini(HfsDurabilityRequest *request);

#endif /* HFS_DURABILITY_H */
