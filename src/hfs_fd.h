#ifndef HFS_FD_H
#define HFS_FD_H

#include <stdbool.h>
#include <stdint.h>
#include <limits.h>
#include <stdatomic.h>
#include <sys/types.h>
#include <pthread.h>

#include "hfs.h"

/* Metadata for one POSIX open-file description shared by dup'ed slots. */
typedef struct HfsOpenFile HfsOpenFile;
/* Deferred blob deletion state for an unlinked inode. */
typedef struct HfsOrphan HfsOrphan;

struct HfsOpenFile {
	HfsFile *file;
	uint64_t inodeId;
	int flags;
	bool writable;
	bool isDirectory;
	uint64_t offset;
	_Atomic uint64_t size;
	uint32_t mode;
	uint32_t uid;
	uint32_t gid;
	uint64_t atimeNs;
	_Atomic uint64_t mtimeNs;
	_Atomic uint64_t ctimeNs;
	char dirPath[PATH_MAX];
	pthread_mutex_t ioLock;
	pthread_mutex_t orphanLock;
	_Atomic uint32_t refs;
	HfsOrphan *orphan;
	HfsOpenFile *deferredNext;
};

struct HfsOrphan {
	uint64_t inodeId;
	uint64_t blobId;
	_Atomic bool cancelled;
	_Atomic uint32_t openRefs;
};

/* Allocate one open-file description with one owning reference. */
int hfsFdCreate(HfsOpenFile **out, HfsFile *file, uint64_t inodeId,
		int flags, bool writable, bool isDirectory, const char *dirPath);
/* Destroy an open-file description after its last reference is released. */
void hfsFdDestroy(HfsOpenFile *openFile);
/* Register an fd slot; ownership of the initial reference moves to the slot. */
int hfsFdRegister(int fd, HfsOpenFile *openFile);
/* Acquire a stable reference to an HFS fd slot. */
HfsOpenFile *hfsFdAcquire(int fd);
/* Remove one fd slot and return its owning reference. */
HfsOpenFile *hfsFdDetach(int fd);
/* Detach one arbitrary slot for orderly library teardown. */
HfsOpenFile *hfsFdDetachAny(int *fdOut);
/* Copy an existing slot while sharing its open-file description. */
int hfsFdCopy(int oldfd, int newfd, HfsOpenFile **replacedFile);
/* Release a reference and return true when the object became unused. */
bool hfsFdRelease(HfsOpenFile *openFile, HfsOrphan **readyOrphan);
/* Return true when the fd currently belongs to HFS. */
bool hfsFdIsHfs(int fd);
/* Reset the registry after all slots have been released. */
void hfsFdReset(void);
/* Mark all open descriptions for an inode as unlinked. */
int hfsFdMarkOrphan(uint64_t inodeId, uint64_t blobId,
		   uint64_t size, uint64_t ctimeNs, uint64_t mtimeNs,
		   HfsOrphan **orphanOut);
/* Undo a provisional orphan mark when its metadata transaction did not publish. */
void hfsFdClearOrphan(HfsOrphan *orphan);
/* Read orphan metadata while holding the description's orphan lock. */
bool hfsFdGetOrphanInfo(HfsOpenFile *openFile, uint64_t *size,
			uint64_t *ctimeNs, uint64_t *mtimeNs);
/* Release an orphan descriptor after its blob has been deleted. */
void hfsFdOrphanDestroy(HfsOrphan *orphan);

#endif /* HFS_FD_H */
