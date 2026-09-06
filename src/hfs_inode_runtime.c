#include <errno.h>
#include <stdlib.h>

#include "hfs_inode_runtime.h"

/* Hash persistent ids without depending on the inode table's compact layout. */
static uint32_t hfsInodeRuntimeBucket(uint64_t inodeId) {
	inodeId ^= inodeId >> 33;
	inodeId *= UINT64_C(0xff51afd7ed558ccd);
	inodeId ^= inodeId >> 33;
	return (uint32_t)inodeId & (HFS_INODE_RUNTIME_BUCKET_COUNT - 1U);
}

/* Initialize runtime synchronization before the first file is opened. */
int hfsInodeRuntimeInit(Hfs *fs) {
	if (fs == NULL) {
		errno = EINVAL;
		return -1;
	}
	if (pthread_mutex_init(&fs->inodeRuntimeLock, NULL) != 0) {
		errno = ENOMEM;
		return -1;
	}
	if (pthread_mutex_init(&fs->compatibilityResizeLock, NULL) != 0) {
		pthread_mutex_destroy(&fs->inodeRuntimeLock);
		errno = ENOMEM;
		return -1;
	}
	fs->compatibilityRuntime = hfsInodeRuntimeGet(fs, UINT64_MAX);
	if (fs->compatibilityRuntime == NULL) {
		pthread_mutex_destroy(&fs->compatibilityResizeLock);
		pthread_mutex_destroy(&fs->inodeRuntimeLock);
		return -1;
	}
	return 0;
}

/* Destroy lazy runtime entries after all open files have been released. */
void hfsInodeRuntimeFini(Hfs *fs) {
	uint32_t i;

	if (fs == NULL) {
		return;
	}
	for (i = 0; i < HFS_INODE_RUNTIME_BUCKET_COUNT; i++) {
		HfsInodeRuntime *runtime = fs->inodeRuntimeBuckets[i];

		while (runtime != NULL) {
			HfsInodeRuntime *next = runtime->next;

			pthread_rwlock_destroy(&runtime->syncLock);
			pthread_mutex_destroy(&runtime->appendLock);
			pthread_mutex_destroy(&runtime->resizeLock);
			free(runtime);
			runtime = next;
		}
		fs->inodeRuntimeBuckets[i] = NULL;
	}
	fs->compatibilityRuntime = NULL;
	pthread_mutex_destroy(&fs->compatibilityResizeLock);
	pthread_mutex_destroy(&fs->inodeRuntimeLock);
}

/* Return an existing entry or create one while holding the short registry lock. */
HfsInodeRuntime *hfsInodeRuntimeGet(Hfs *fs, uint64_t inodeId) {
	HfsInodeRuntime *runtime;
	uint32_t bucket;
	int rc;

	if (fs == NULL || inodeId == 0) {
		errno = EINVAL;
		return NULL;
	}
	bucket = hfsInodeRuntimeBucket(inodeId);
	pthread_mutex_lock(&fs->inodeRuntimeLock);
	for (runtime = fs->inodeRuntimeBuckets[bucket]; runtime != NULL;
	     runtime = runtime->next) {
		if (runtime->inodeId == inodeId) {
			pthread_mutex_unlock(&fs->inodeRuntimeLock);
			return runtime;
		}
	}
	runtime = calloc(1, sizeof(*runtime));
	if (runtime == NULL) {
		pthread_mutex_unlock(&fs->inodeRuntimeLock);
		errno = ENOMEM;
		return NULL;
	}
	runtime->inodeId = inodeId;
	rc = pthread_mutex_init(&runtime->resizeLock, NULL);
	if (rc != 0) {
		free(runtime);
		pthread_mutex_unlock(&fs->inodeRuntimeLock);
		errno = rc;
		return NULL;
	}
	rc = pthread_mutex_init(&runtime->appendLock, NULL);
	if (rc != 0) {
		pthread_mutex_destroy(&runtime->resizeLock);
		free(runtime);
		pthread_mutex_unlock(&fs->inodeRuntimeLock);
		errno = rc;
		return NULL;
	}
	rc = pthread_rwlock_init(&runtime->syncLock, NULL);
	if (rc != 0) {
		pthread_mutex_destroy(&runtime->appendLock);
		pthread_mutex_destroy(&runtime->resizeLock);
		free(runtime);
		pthread_mutex_unlock(&fs->inodeRuntimeLock);
		errno = rc;
		return NULL;
	}
	runtime->next = fs->inodeRuntimeBuckets[bucket];
	fs->inodeRuntimeBuckets[bucket] = runtime;
	pthread_mutex_unlock(&fs->inodeRuntimeLock);
	return runtime;
}

/* Return the runtime associated with a file, creating it for normal inode files. */
HfsInodeRuntime *hfsInodeRuntimeForFile(HfsFile *file) {
	if (file == NULL || file->fs == NULL) {
		errno = EINVAL;
		return NULL;
	}
	if (file->runtime != NULL) {
		return file->runtime;
	}
	if (file->inodeId == 0) {
		return file->fs->compatibilityRuntime;
	}
	if (file->inodeId != 0) {
		file->runtime = hfsInodeRuntimeGet(file->fs, file->inodeId);
	}
	return file->runtime;
}
