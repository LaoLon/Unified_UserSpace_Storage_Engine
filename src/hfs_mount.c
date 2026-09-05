#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include <spdk/blob.h>
#include <spdk/blob_bdev.h>
#include <spdk/bdev.h>
#include <spdk/event.h>
#include <spdk/log.h>

#include "hfs_internal.h"
#include "hfs_log.h"

/* Return the configured bdev name or the legacy in-memory default. */
static const char *hfs_bdev_name(void) {
	const char *name = getenv("HFS_BDEV_NAME");

	return (name != NULL && name[0] != '\0') ? name : "Malloc0";
}

/* Return true only when the caller explicitly requests formatting. */
static bool hfs_format_requested(void) {
	const char *format = getenv("HFS_FORMAT");

	return format != NULL && strcmp(format, "1") == 0;
}

/* Request-private mount state retained through the SPDK init/unload callback. */
typedef struct HfsMountRequest {
	Hfs *fs;
	HfsRequestResult result;
} HfsMountRequest;

/* Completion callback for spdk_bs_unload: clear the blobstore so a
 * second umount (e.g. from hfs_lib_fini) is a no-op. */
static void hfsSpdkBsUnloadCb(void *arg, int bserrno) {
	HfsMountRequest *request = arg;
	Hfs *fs = request->fs;

	if (bserrno != 0) {
		HFS_LOG_ERROR("blobstore unload failed bserrno=%d", bserrno);
	}
	fs->blobstore = NULL;
	fs->bsDev = NULL;
	hfsRequestComplete(&request->result, bserrno, 0);
}

/*
 * SPDK-thread task: release the I/O channel and unload the blobstore.
 * spdk_bs_free_io_channel may print a wrong_thread error (the channel
 * is owned by an SPDK-internal thread), but the release still takes
 * effect and unload needs the channel gone to complete. Both are
 * guarded so a second umount (e.g. from hfs_lib_fini) is a no-op.
 */
static void hfsDoUmount(void *arg) {
	HfsMountRequest *request = arg;
	Hfs *fs = request->fs;

	if (hfsRequestCompletionInjected(request->result.operation)) {
		hfsRequestComplete(&request->result, -EIO, 0);
		return;
	}
	if (fs->blobstore) {
		if (fs->channel) {
			spdk_bs_free_io_channel(fs->channel);
			fs->channel = NULL;
		}
		if (fs->bdevChannel) {
			spdk_put_io_channel(fs->bdevChannel);
			fs->bdevChannel = NULL;
		}
		if (fs->bdevDesc) {
			spdk_bdev_close(fs->bdevDesc);
			fs->bdevDesc = NULL;
		}
		spdk_bs_unload(fs->blobstore, hfsSpdkBsUnloadCb, request);
	} else {
		if (fs->bdevChannel) {
			spdk_put_io_channel(fs->bdevChannel);
			fs->bdevChannel = NULL;
		}
		if (fs->bdevDesc) {
			spdk_bdev_close(fs->bdevDesc);
			fs->bdevDesc = NULL;
		}
		hfsRequestComplete(&request->result, 0, 0);
	}
}

/*
 * Completion callback for spdk_bs_init/spdk_bs_load: record the blobstore,
 * io unit size and allocate the I/O channel used by all blob operations.
 */
static void hfsSpdkBsInitCb(void *arg, struct spdk_blob_store *bs, int bserrno) {
	HfsMountRequest *request = arg;
	Hfs *fs = request->fs;
	uint64_t ioUnitSize;

	if (bserrno != 0 || bs == NULL) {
		HFS_LOG_ERROR("blobstore init failed bserrno=%d", bserrno);
		if (fs->bdevChannel) {
			spdk_put_io_channel(fs->bdevChannel);
			fs->bdevChannel = NULL;
		}
		if (fs->bdevDesc) {
			spdk_bdev_close(fs->bdevDesc);
			fs->bdevDesc = NULL;
		}
		fs->bsDev = NULL;
		hfsRequestComplete(&request->result,
				   bserrno != 0 ? bserrno : -EINVAL, 0);
		return;
	}

	ioUnitSize = spdk_bs_get_io_unit_size(bs);
	SPDK_NOTICELOG("ioUnitSize : %"PRIu64"\n", ioUnitSize);

	fs->unitSize = ioUnitSize;
	fs->blobstore = bs;

	fs->channel = spdk_bs_alloc_io_channel(fs->blobstore);
	if (fs->channel == NULL) {
		HFS_LOG_ERROR("blobstore io channel allocation failed");
		if (fs->bdevChannel) {
			spdk_put_io_channel(fs->bdevChannel);
			fs->bdevChannel = NULL;
		}
		if (fs->bdevDesc) {
			spdk_bdev_close(fs->bdevDesc);
			fs->bdevDesc = NULL;
		}
		fs->bsDev = NULL;
		hfsRequestComplete(&request->result, -ENOMEM, 0);
		return;
	}

	SPDK_NOTICELOG("mount finished\n");
	hfsRequestComplete(&request->result, 0, 0);
}

/* Bdev event callback (informational, currently unused). */
static void hfsSpdkBdevEventCb(enum spdk_bdev_event_type type, struct spdk_bdev *bdev,
				void *event_ctx) {
	SPDK_NOTICELOG("hfsSpdkBdevEventCb\n");
}

/* Open a durable-flush descriptor for the base bdev on the SPDK owner thread. */
static int hfsOpenBdevFlushHandle(Hfs *fs, struct spdk_bs_dev *bsDev) {
	struct spdk_bdev *baseBdev;
	const char *bdevName;
	int rc;

	if (bsDev == NULL || bsDev->get_base_bdev == NULL) {
		return -EINVAL;
	}
	baseBdev = bsDev->get_base_bdev(bsDev);
	if (baseBdev == NULL) {
		return -ENODEV;
	}
	bdevName = spdk_bdev_get_name(baseBdev);
	if (bdevName == NULL) {
		return -ENODEV;
	}

	rc = spdk_bdev_open_ext(bdevName, true, hfsSpdkBdevEventCb, NULL,
				&fs->bdevDesc);
	if (rc != 0) {
		return rc;
	}
	fs->bdevChannel = spdk_bdev_get_io_channel(fs->bdevDesc);
	if (fs->bdevChannel == NULL) {
		spdk_bdev_close(fs->bdevDesc);
		fs->bdevDesc = NULL;
		return -ENOMEM;
	}
	return 0;
}

/*
 * SPDK-thread task: wrap the configured bdev as a blobstore device and
 * initialize or load the blobstore according to the explicit format flag.
 */
static void hfsDoMount(void *arg) {
	HfsMountRequest *request = arg;
	Hfs *fs = request->fs;
	struct spdk_bs_dev *bsDev = NULL;
	int rc;

	if (hfsRequestCompletionInjected(request->result.operation)) {
		hfsRequestComplete(&request->result, -EIO, 0);
		return;
	}
	rc = spdk_bdev_create_bs_dev_ext(hfs_bdev_name(), hfsSpdkBdevEventCb,
					     NULL, &bsDev);
	if (rc != 0) {
		HFS_LOG_ERROR("create bs dev failed rc=%d", rc);
		hfsRequestComplete(&request->result, rc, 0);
		return;
	}
	fs->bsDev = bsDev;
	rc = hfsOpenBdevFlushHandle(fs, bsDev);
	if (rc != 0) {
		HFS_LOG_ERROR("open bdev flush handle failed rc=%d", rc);
		bsDev->destroy(bsDev);
		fs->bsDev = NULL;
		hfsRequestComplete(&request->result, rc, 0);
		return;
	}

	if (hfs_format_requested()) {
		spdk_bs_init(bsDev, NULL, hfsSpdkBsInitCb, request);
	} else {
		spdk_bs_load(bsDev, NULL, hfsSpdkBsInitCb, request);
	}

	SPDK_NOTICELOG("hfs_entry\n");
}

/* Mount the blobstore synchronously; return false on timeout or SPDK error. */
int hfsMount(Hfs *fs) {
	HfsMountRequest request;

	if (fs == NULL) {
		errno = EINVAL;
		return 0;
	}
	memset(&request, 0, sizeof(request));
	request.fs = fs;
	hfsRequestInit(&request.result, HFS_REQUEST_MOUNT);
	return hfsRequestWait(&request.result, hfsDoMount, &request) == 0 &&
		fs->blobstore != NULL;
}

/* Unmount the blobstore synchronously; return false on timeout or SPDK error. */
int hfsUmount(Hfs *fs) {
	HfsMountRequest request;

	if (fs == NULL) {
		errno = EINVAL;
		return 0;
	}
	memset(&request, 0, sizeof(request));
	request.fs = fs;
	hfsRequestInit(&request.result, HFS_REQUEST_UMOUNT);
	return hfsRequestWait(&request.result, hfsDoUmount, &request) == 0 &&
		fs->blobstore == NULL;
}
