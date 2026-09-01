#include <spdk/blob.h>
#include <spdk/blob_bdev.h>
#include <spdk/bdev.h>
#include <spdk/event.h>
#include <spdk/log.h>

#include "hfs_internal.h"
#include "hfs_log.h"

/* Completion callback for spdk_bs_unload: clear the blobstore so a
 * second umount (e.g. from hfs_lib_fini) is a no-op. */
static void hfsSpdkBsUnloadCb(void *arg, int bserrno) {
	Hfs *fs = (Hfs *)arg;

	if (bserrno != 0) {
		HFS_LOG_ERROR("blobstore unload failed bserrno=%d", bserrno);
	}
	fs->blobstore = NULL;
	fs->finished = true;
}

/*
 * SPDK-thread task: release the I/O channel and unload the blobstore.
 * spdk_bs_free_io_channel may print a wrong_thread error (the channel
 * is owned by an SPDK-internal thread), but the release still takes
 * effect and unload needs the channel gone to complete. Both are
 * guarded so a second umount (e.g. from hfs_lib_fini) is a no-op.
 */
static void hfsDoUmount(void *arg) {
	Hfs *fs = (Hfs *)arg;

	if (fs->blobstore) {
		if (fs->channel) {
			spdk_bs_free_io_channel(fs->channel);
			fs->channel = NULL;
		}
		spdk_bs_unload(fs->blobstore, hfsSpdkBsUnloadCb, fs);
	} else {
		fs->finished = true;
	}
}

/*
 * Completion callback for spdk_bs_init: record the blobstore, io unit
 * size and allocate the I/O channel used by all blob operations.
 */
static void hfsSpdkBsInitCb(void *arg, struct spdk_blob_store *bs, int bserrno) {
	Hfs *fs = (Hfs *)arg;

	if (bserrno != 0 || bs == NULL) {
		HFS_LOG_ERROR("blobstore init failed bserrno=%d", bserrno);
		fs->finished = true;
		return;
	}

	uint64_t ioUnitSize = spdk_bs_get_io_unit_size(bs);
	SPDK_NOTICELOG("ioUnitSize : %"PRIu64"\n", ioUnitSize);

	fs->unitSize = ioUnitSize;
	fs->blobstore = bs;

	fs->channel = spdk_bs_alloc_io_channel(fs->blobstore);
	if (fs->channel == NULL) {
		HFS_LOG_ERROR("blobstore io channel allocation failed");
		fs->finished = true;
		return;
	}

	fs->finished = true;

	SPDK_NOTICELOG("mount finished\n");
}

/* Bdev event callback (informational, currently unused). */
static void hfsSpdkBdevEventCb(enum spdk_bdev_event_type type, struct spdk_bdev *bdev,
				void *event_ctx) {
	SPDK_NOTICELOG("hfsSpdkBdevEventCb\n");
}

/*
 * SPDK-thread task: wrap the "Malloc0" bdev as a blobstore device and
 * initialize the blobstore on it.
 */
static void hfsDoMount(void *arg) {
	Hfs *fs = (Hfs *)arg;

	struct spdk_bs_dev *bsDev = NULL;

	int rc = spdk_bdev_create_bs_dev_ext("Malloc0", hfsSpdkBdevEventCb, NULL, &bsDev);
	if (rc != 0) {
		HFS_LOG_ERROR("create bs dev failed rc=%d", rc);
		spdk_app_stop(0);
	}

	spdk_bs_init(bsDev, NULL, hfsSpdkBsInitCb, fs);

	SPDK_NOTICELOG("hfs_entry\n");
}

/* Mount the blobstore synchronously; returns waiter success. */
int hfsMount(Hfs *fs) {
	fs->finished = false;

	return waiter(globalThread, hfsDoMount, fs, &fs->finished);
}

/* Unmount the blobstore synchronously; returns waiter success. */
int hfsUmount(Hfs *fs) {
	fs->finished = false;

	return waiter(globalThread, hfsDoUmount, fs, &fs->finished);
}