#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include <spdk/blob.h>
#include <spdk/env.h>
#include <spdk/log.h>

#include "hfs_internal.h"
#include "hfs_meta.h"

static void hfsMetaGetSuperCb(void *arg, spdk_blob_id blobId, int bserrno) {
	Hfs *fs = (Hfs *)arg;

	if (bserrno != 0) {
		fs->metaBlobId = SPDK_BLOBID_INVALID;
		fs->finished = true;
		return;
	}

	fs->metaBlobId = blobId;
	fs->finished = true;
}

static void hfsMetaSetSuperCb(void *arg, int bserrno) {
	Hfs *fs = (Hfs *)arg;

	fs->finished = true;
}

static void hfsMetaCreateBlobCb(void *arg, spdk_blob_id blobId, int bserrno) {
	Hfs *fs = (Hfs *)arg;

	if (bserrno != 0) {
		fs->finished = true;
		return;
	}

	fs->metaBlobId = blobId;
	spdk_bs_set_super(fs->blobstore, blobId, hfsMetaSetSuperCb, fs);
}

static void hfsMetaDoFormat(void *arg) {
	Hfs *fs = (Hfs *)arg;

	spdk_bs_create_blob(fs->blobstore, hfsMetaCreateBlobCb, fs);
}

static void hfsMetaDoGetSuper(void *arg) {
	Hfs *fs = (Hfs *)arg;

	spdk_bs_get_super(fs->blobstore, hfsMetaGetSuperCb, fs);
}

int hfsMetaLoad(Hfs *fs) {
	fs->finished = false;

	if (!waiter(globalThread, hfsMetaDoGetSuper, fs, &fs->finished)) {
		return -1;
	}

	if (fs->metaBlobId == SPDK_BLOBID_INVALID) {
		errno = ENOENT;
		return -1;
	}

	SPDK_NOTICELOG("meta blobId: %"PRIu64"\n", fs->metaBlobId);
	return 0;
}

int hfsMetaFormat(Hfs *fs) {
	fs->finished = false;

	if (!waiter(globalThread, hfsMetaDoFormat, fs, &fs->finished)) {
		return -1;
	}

	if (fs->metaBlobId == SPDK_BLOBID_INVALID) {
		return -1;
	}

	SPDK_NOTICELOG("formatted, meta blobId: %"PRIu64"\n", fs->metaBlobId);
	return 0;
}

int hfsMetaInit(Hfs *fs) {
	if (hfsMetaLoad(fs) == 0) {
		return 0;
	}
	if (errno != ENOENT) {
		return -1;
	}
	return hfsMetaFormat(fs);
}
