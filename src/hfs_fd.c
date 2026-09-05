#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "hfs_fd.h"
#include "hfs_lib.h"
#include "hfs_stats.h"

/* One short critical section protects fd slots and reference transitions. */
static pthread_rwlock_t g_fdLock = PTHREAD_RWLOCK_INITIALIZER;
static HfsOpenFile *g_fdSlots[HFS_FD_TABLE_SIZE];

/* Count fd registry lock acquisitions while preserving short lock scopes. */
static void hfsFdReadLock(void) {
	HFS_STATS_FD_READ_LOCK();
	pthread_rwlock_rdlock(&g_fdLock);
}

/* Count fd registry write lock acquisitions while preserving short lock scopes. */
static void hfsFdWriteLock(void) {
	HFS_STATS_FD_WRITE_LOCK();
	pthread_rwlock_wrlock(&g_fdLock);
}

/* Allocate a description shared by all duplicate fd slots. */
int hfsFdCreate(HfsOpenFile **out, HfsFile *file, uint64_t inodeId,
		int flags, bool writable, bool isDirectory, const char *dirPath) {
	HfsOpenFile *openFile;

	if (out == NULL) {
		errno = EINVAL;
		return -1;
	}
	openFile = calloc(1, sizeof(*openFile));
	if (openFile == NULL) {
		errno = ENOMEM;
		return -1;
	}
	if (pthread_mutex_init(&openFile->ioLock, NULL) != 0) {
		free(openFile);
		errno = ENOMEM;
		return -1;
	}
	openFile->file = file;
	openFile->inodeId = inodeId;
	openFile->flags = flags;
	openFile->writable = writable;
	openFile->isDirectory = isDirectory;
	openFile->refs = 1;
	if (dirPath != NULL) {
		snprintf(openFile->dirPath, sizeof(openFile->dirPath), "%s", dirPath);
	}
	*out = openFile;
	return 0;
}

/* Release the private synchronization state of an unused description. */
void hfsFdDestroy(HfsOpenFile *openFile) {
	if (openFile == NULL) {
		return;
	}
	pthread_mutex_destroy(&openFile->ioLock);
	free(openFile);
}

/* Install one description in an empty fd slot. */
int hfsFdRegister(int fd, HfsOpenFile *openFile) {
	int rc = 0;

	if (fd < 0 || fd >= HFS_FD_TABLE_SIZE || openFile == NULL) {
		errno = EBADF;
		return -1;
	}
	hfsFdWriteLock();
	if (g_fdSlots[fd] != NULL) {
		rc = -1;
		errno = EBUSY;
	} else {
		g_fdSlots[fd] = openFile;
	}
	pthread_rwlock_unlock(&g_fdLock);
	return rc;
}

/* Acquire a slot reference without exposing a mutable table entry. */
HfsOpenFile *hfsFdAcquire(int fd) {
	HfsOpenFile *openFile = NULL;

	if (fd < 0 || fd >= HFS_FD_TABLE_SIZE) {
		return NULL;
	}
	hfsFdWriteLock();
	openFile = g_fdSlots[fd];
	if (openFile != NULL) {
		openFile->refs++;
	}
	pthread_rwlock_unlock(&g_fdLock);
	return openFile;
}

/* Detach a slot before the kernel placeholder fd is closed or reused. */
HfsOpenFile *hfsFdDetach(int fd) {
	HfsOpenFile *openFile = NULL;

	if (fd < 0 || fd >= HFS_FD_TABLE_SIZE) {
		return NULL;
	}
	hfsFdWriteLock();
	openFile = g_fdSlots[fd];
	g_fdSlots[fd] = NULL;
	pthread_rwlock_unlock(&g_fdLock);
	return openFile;
}

/* Detach one arbitrary slot while the runtime is still available. */
HfsOpenFile *hfsFdDetachAny(int *fdOut) {
	HfsOpenFile *openFile = NULL;
	int i;

	hfsFdWriteLock();
	for (i = 0; i < HFS_FD_TABLE_SIZE; i++) {
		if (g_fdSlots[i] != NULL) {
			openFile = g_fdSlots[i];
			g_fdSlots[i] = NULL;
			if (fdOut != NULL) {
				*fdOut = i;
			}
			break;
		}
	}
	pthread_rwlock_unlock(&g_fdLock);
	return openFile;
}

/* Replace the target slot and make both descriptors share one description. */
int hfsFdCopy(int oldfd, int newfd, HfsOpenFile **replacedFile) {
	HfsOpenFile *oldFile;
	HfsOpenFile *replaced;

	if (replacedFile != NULL) {
		*replacedFile = NULL;
	}

	if (oldfd < 0 || oldfd >= HFS_FD_TABLE_SIZE ||
	    newfd < 0 || newfd >= HFS_FD_TABLE_SIZE) {
		errno = EBADF;
		return -1;
	}
	hfsFdWriteLock();
	oldFile = g_fdSlots[oldfd];
	if (oldFile == NULL) {
		pthread_rwlock_unlock(&g_fdLock);
		errno = EBADF;
		return -1;
	}
	if (oldfd == newfd) {
		pthread_rwlock_unlock(&g_fdLock);
		return 0;
	}
	replaced = g_fdSlots[newfd];
	oldFile->refs++;
	g_fdSlots[newfd] = oldFile;
	pthread_rwlock_unlock(&g_fdLock);

	/* Return the removed slot so the library can close its blob safely. */
	if (replacedFile != NULL) {
		*replacedFile = replaced;
	} else if (replaced != NULL) {
		(void)hfsFdRelease(replaced, NULL);
	}
	return 0;
}

/* Drop one slot or in-flight operation reference. */
bool hfsFdRelease(HfsOpenFile *openFile, HfsOrphan **readyOrphan) {
	bool unused = false;

	if (readyOrphan != NULL) {
		*readyOrphan = NULL;
	}
	if (openFile == NULL) {
		return false;
	}
	hfsFdWriteLock();
	if (openFile->refs > 0) {
		openFile->refs--;
	}
	unused = openFile->refs == 0;
	if (unused && openFile->orphan != NULL &&
	    --openFile->orphan->openRefs == 0) {
		if (readyOrphan != NULL) {
			*readyOrphan = openFile->orphan;
		}
		openFile->orphan = NULL;
	}
	pthread_rwlock_unlock(&g_fdLock);
	return unused;
}

/* Check a slot under a read lock; callers must acquire again for use. */
bool hfsFdIsHfs(int fd) {
	bool result = false;

	if (fd < 0 || fd >= HFS_FD_TABLE_SIZE) {
		return false;
	}
	hfsFdReadLock();
	result = g_fdSlots[fd] != NULL;
	pthread_rwlock_unlock(&g_fdLock);
	return result;
}

/* Clear the registry during a fresh library initialization. */
void hfsFdReset(void) {
	hfsFdWriteLock();
	memset(g_fdSlots, 0, sizeof(g_fdSlots));
	pthread_rwlock_unlock(&g_fdLock);
}

/* Attach one orphan record to every unique open description for an inode. */
int hfsFdMarkOrphan(uint64_t inodeId, uint64_t blobId,
		   uint64_t size, uint64_t ctimeNs, uint64_t mtimeNs,
		   HfsOrphan **orphanOut) {
	HfsOrphan *orphan = NULL;
	uint32_t i;
	uint32_t j;

	if (orphanOut != NULL) {
		*orphanOut = NULL;
	}
	hfsFdWriteLock();
	for (i = 0; i < HFS_FD_TABLE_SIZE; i++) {
		HfsOpenFile *openFile = g_fdSlots[i];
		bool seen = false;

		if (openFile == NULL || openFile->inodeId != inodeId ||
		    openFile->orphan != NULL) {
			continue;
		}
		for (j = 0; j < i; j++) {
			if (g_fdSlots[j] == openFile) {
				seen = true;
				break;
			}
		}
		if (seen) {
			continue;
		}
		if (orphan == NULL) {
			orphan = calloc(1, sizeof(*orphan));
			if (orphan == NULL) {
				pthread_rwlock_unlock(&g_fdLock);
				errno = ENOMEM;
				return -1;
			}
			orphan->inodeId = inodeId;
			orphan->blobId = blobId;
		}
		openFile->orphan = orphan;
		atomic_store_explicit(&openFile->size, size, memory_order_relaxed);
		openFile->ctimeNs = ctimeNs;
		atomic_store_explicit(&openFile->mtimeNs, mtimeNs,
				      memory_order_relaxed);
		orphan->openRefs++;
	}
	pthread_rwlock_unlock(&g_fdLock);
	if (orphanOut != NULL) {
		*orphanOut = orphan;
	}
	return 0;
}

/* Detach a provisional orphan from all descriptions and release its record. */
void hfsFdClearOrphan(HfsOrphan *orphan) {
	uint32_t i;

	if (orphan == NULL) {
		return;
	}
	hfsFdWriteLock();
	for (i = 0; i < HFS_FD_TABLE_SIZE; i++) {
		HfsOpenFile *openFile = g_fdSlots[i];

		if (openFile != NULL && openFile->orphan == orphan) {
			openFile->orphan = NULL;
		}
	}
	pthread_rwlock_unlock(&g_fdLock);
	free(orphan);
}

/* Read metadata retained by an unlinked open-file description. */
bool hfsFdGetOrphanInfo(HfsOpenFile *openFile, uint64_t *size,
			uint64_t *ctimeNs, uint64_t *mtimeNs) {
	bool result;

	if (openFile == NULL) {
		return false;
	}
	hfsFdReadLock();
	result = openFile->orphan != NULL;
	if (result) {
		if (size != NULL) {
			*size = atomic_load_explicit(&openFile->size,
					     memory_order_relaxed);
		}
		if (ctimeNs != NULL) {
			*ctimeNs = openFile->ctimeNs;
		}
		if (mtimeNs != NULL) {
			*mtimeNs = atomic_load_explicit(&openFile->mtimeNs,
					     memory_order_relaxed);
		}
	}
	pthread_rwlock_unlock(&g_fdLock);
	return result;
}

/* Free an orphan record after its deferred blob deletion completes. */
void hfsFdOrphanDestroy(HfsOrphan *orphan) {
	free(orphan);
}
