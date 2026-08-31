#include <spdk/blob.h>
#include <spdk/blob_bdev.h>
#include <spdk/bdev.h>
#include <spdk/event.h>
#include <spdk/log.h>

#include "hfs_internal.h"

static void hfsSpdkBsUnloadCb(void *arg, int bserrno) {
	Hfs *fs = (Hfs *)arg;
	fs->finished = true;
}

static void hfsDoUmount(void *arg) {
	Hfs *fs = (Hfs *)arg;

	if (fs->blobstore) {
		if (fs->channel) {
			spdk_bs_free_io_channel(fs->channel);
		}
		spdk_bs_unload(fs->blobstore, hfsSpdkBsUnloadCb, fs);
	}
}

static void hfsSpdkBsInitCb(void *arg, struct spdk_blob_store *bs, int bserrno) {
	Hfs *fs = (Hfs *)arg;

	uint64_t ioUnitSize = spdk_bs_get_io_unit_size(bs);
	SPDK_NOTICELOG("ioUnitSize : %"PRIu64"\n", ioUnitSize);

	fs->unitSize = ioUnitSize;
	fs->blobstore = bs;

	fs->channel = spdk_bs_alloc_io_channel(fs->blobstore);
	if (fs->channel == NULL) {
		return ;
	}

	fs->finished = true;

	SPDK_NOTICELOG("mount finished\n");
}

static void hfsSpdkBdevEventCb(enum spdk_bdev_event_type type, struct spdk_bdev *bdev,
				void *event_ctx) {
	SPDK_NOTICELOG("hfsSpdkBdevEventCb\n");
}

static void hfsDoMount(void *arg) {
	Hfs *fs = (Hfs *)arg;

	struct spdk_bs_dev *bsDev = NULL;

	int rc = spdk_bdev_create_bs_dev_ext("Malloc0", hfsSpdkBdevEventCb, NULL, &bsDev);
	if (rc != 0) {
		spdk_app_stop(0);
	}

	spdk_bs_init(bsDev, NULL, hfsSpdkBsInitCb, fs);

	SPDK_NOTICELOG("hfs_entry\n");
}

// mount
int hfsMount(Hfs *fs) {
	fs->finished = false;

	return waiter(globalThread, hfsDoMount, fs, &fs->finished);
}

// umount
int hfsUmount(Hfs *fs) {
	fs->finished = false;

	return waiter(globalThread, hfsDoUmount, fs, &fs->finished);
}