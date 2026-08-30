

#include <stdio.h>

#include <spdk/event.h>
#include <spdk/blob.h>
#include <spdk/bdev.h>
#include <spdk/blob_bdev.h>
#include <spdk/env.h>
#include <spdk/file.h>
#include <spdk/rpc.h>



#define BUFFER_SIZE		512


#define FILENAME_LENGTH  128

typedef struct Hfs {

	struct spdk_blob_store *blobstore;
	uint64_t unitSize;
	uint64_t freeClusterCount;
	uint64_t numCluster;

	struct spdk_io_channel *channel;
	bool finished;

} Hfs;

typedef struct HfsOperation {
	int (*mount)(struct Hfs *fs);
	int (*umount)(struct Hfs *fs);
} HfsOperation;


typedef struct HfsFile {

	uint8_t filename[FILENAME_LENGTH];

	spdk_blob_id blobId;
	struct spdk_blob *blob;
	Hfs *fs;

	uint8_t *writeBuffer;
	uint8_t *readBuffer;

	bool finished;
	
} HfsFile;

typedef struct HfsFileOperation {

	int (*open)(struct HfsFile *file);
	int (*read)(struct HfsFile *file);
	int (*write)(struct HfsFile *file);
	int (*close)(struct HfsFile *file);
	
} HfsFileOperation;

struct spdk_thread *globalThread = NULL;

static const int WAITER_MAX_TIME = 2000000000;

static bool waiter(struct spdk_thread *thread, spdk_msg_fn startFn, void *ctx, bool *finished) {

	spdk_thread_send_msg(thread, startFn, ctx);

	int waiterCount = 0;

	do {
		spdk_thread_poll(thread, 0, 0);
		waiterCount ++; 
	} while(!(*finished) && waiterCount < WAITER_MAX_TIME);

	if (!(*finished) && waiterCount >= WAITER_MAX_TIME) {
		return false; // timeout
	}

	return true;
}



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

static void hfsSpdkBlobDeleteCb(void *arg, int bserrno) {
	HfsFile *file = (HfsFile *)arg;
	
	spdk_free(file->writeBuffer);
	spdk_free(file->readBuffer);

	SPDK_NOTICELOG("close complete\n");
	file->finished = true;
}


static void hfsSpdkBlobCloseCb(void *arg, int bserrno) {
	HfsFile *file = (HfsFile *)arg;
	
	spdk_bs_delete_blob(file->fs->blobstore, file->blobId, hfsSpdkBlobDeleteCb, file);
}


static void hfsDoClose(void *arg) {

	HfsFile *file = (HfsFile *)arg;

	spdk_blob_close(file->blob, hfsSpdkBlobCloseCb, file);

}


static void hfsSpdkBlobReadCb(void *arg, int bserrno) {

	HfsFile *file = (HfsFile *)arg;

	SPDK_NOTICELOG("READ BUFFER : %s\n", file->readBuffer);

	file->finished = true;

	//spdk_blob_close(ctx->blob, hfsSpdkBlobCloseCb, ctx);
	
}


static void hfsDoRead(void *arg) {

	HfsFile *file = (HfsFile *)arg;

	memset(file->readBuffer, 0x0, BUFFER_SIZE);

	spdk_blob_io_read(file->blob, file->fs->channel, file->readBuffer, 
		0, 1, hfsSpdkBlobReadCb, file);

}


static void hfsSpdkBlobWriteCb(void *arg, int bserrno) {
	HfsFile *file = (HfsFile *)arg;
	SPDK_NOTICELOG("write complete\n");

	file->finished = true;
}


static void hfsDoWrite(void *arg) {

	HfsFile *file = (HfsFile *)arg;

	spdk_blob_io_write(file->blob, file->fs->channel, file->writeBuffer, 
		0, 1, hfsSpdkBlobWriteCb, file);

}

static void hfsSpdkBlobSyncCb(void *arg, int bserrno) {
	HfsFile *file = (HfsFile *)arg;
	file->writeBuffer = spdk_malloc(BUFFER_SIZE, 0x1000, NULL, SPDK_ENV_LCORE_ID_ANY, SPDK_MALLOC_DMA);
	if (file->writeBuffer == NULL) {
		return ;
	}

	file->readBuffer = spdk_malloc(BUFFER_SIZE, 0x1000, NULL, SPDK_ENV_LCORE_ID_ANY, SPDK_MALLOC_DMA);
	if (file->readBuffer == NULL) {
		return ;
	}
	

	SPDK_NOTICELOG("open complete\n");

	file->finished = true;
}



static void hfsSpdkBlobResizeCb(void *arg, int bserrno) {

	HfsFile *file = (HfsFile *)arg;
	
	uint64_t total = spdk_blob_get_num_clusters(file->blob);
	file->fs->numCluster = total;

	SPDK_NOTICELOG("resize blob :%"PRIu64"\n", total);

	spdk_blob_sync_md(file->blob, hfsSpdkBlobSyncCb, file);

}



static void hfsSpdkBsOpenBlobCb(void *arg, struct spdk_blob *blb, int bserrno) {

	HfsFile *file = (HfsFile *)arg;

	file->blob = blb;

	uint64_t freeCluster = spdk_bs_free_cluster_count(file->fs->blobstore);  //
	SPDK_NOTICELOG("freeCluster : %"PRIu64"\n", freeCluster);
	file->fs->freeClusterCount = freeCluster;

	spdk_blob_resize(blb, freeCluster, hfsSpdkBlobResizeCb, file);

}



static void hfsSpdkBsCreateBlobCb(void *arg, spdk_blob_id blobId, int bserrno) {

	HfsFile *file = (HfsFile *)arg;

	file->blobId = blobId;
	SPDK_NOTICELOG("blobId : %"PRIu64"\n", blobId);

	spdk_bs_open_blob(file->fs->blobstore, blobId, hfsSpdkBsOpenBlobCb, file);
}


static void hfsDoOpen(void *arg) {

	HfsFile *file = (HfsFile *)arg;
	
	spdk_bs_create_blob(file->fs->blobstore, hfsSpdkBsCreateBlobCb, file);

}

static void hfsSpdkBsInitCb(void *arg, struct spdk_blob_store *bs,
		int bserrno) {

	Hfs *fs = (Hfs*)arg;

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

	//
}



static void hfsSpdkBdevEventCb(enum spdk_bdev_event_type type, struct spdk_bdev *bdev,
				     void *event_ctx) {

	SPDK_NOTICELOG("hfsSpdkBdevEventCb\n");

}




static void hfsDoMount(void *arg) {

	Hfs *fs = (Hfs*)arg;
	
	struct spdk_bs_dev *bsDev = NULL;

	int rc = spdk_bdev_create_bs_dev_ext("Malloc0", hfsSpdkBdevEventCb, NULL, &bsDev);
	if (rc != 0) {
		spdk_app_stop(0);
	}

	//int *count = malloc(sizeof(int));
	//*count = 2404;
	
	spdk_bs_init(bsDev, NULL, hfsSpdkBsInitCb, fs);

	//sleep(10);
	SPDK_NOTICELOG("hfs_entry\n");

}


// filesystem
// load
static int hfsMount(struct Hfs *fs) {

	fs->finished = false;

	return waiter(globalThread, hfsDoMount, fs, &fs->finished);
} 


// unload
static int hfsUmount(struct Hfs *fs) {

	fs->finished = false;
	
	return waiter(globalThread, hfsDoUmount, fs, &fs->finished);
}


// file
// open
static int hfsOpen(struct HfsFile *file) {

	file->finished = false;
	
	return waiter(globalThread, hfsDoOpen, file, &file->finished);
	
}


// read

static int hfsRead(struct HfsFile *file, uint8_t *buffer, size_t count) {

	file->finished = false;
	waiter(globalThread, hfsDoRead, file, &file->finished);

	int len = strlen(file->readBuffer);
	memcpy(buffer, file->readBuffer, len);  //

	return len;
}


// write

static int hfsWrite(struct HfsFile *file, const uint8_t *buffer, size_t count) {

	file->finished = false;

	memcpy(file->writeBuffer, buffer, count);  // count / 512
	
	return waiter(globalThread, hfsDoWrite, file, &file->finished);
}

// close
static int hfsClose(struct HfsFile *file) {

	file->finished = false;

	return waiter(globalThread, hfsDoClose, file, &file->finished);
}


// fd --> file


// zvfs.json

#ifdef HFS_DEFAULT_CONFIG
static const char *jsonFile = HFS_DEFAULT_CONFIG;
#else
static const char *jsonFile = "config/hfs.json";
#endif


static void jsonAppLoadDone(int rc, void *ctx) {

	bool *done = ctx;
	*done = true;

	SPDK_NOTICELOG("jsonAppLoadDone\n");
}

static void hfsSubsystemInitDone(int rc, void *arg)
{
	void *json;
	size_t jsonSize;

	if (rc != 0) {
		jsonAppLoadDone(rc, arg);
		return;
	}

	json = spdk_posix_file_load_from_name(jsonFile, &jsonSize);
	if (json == NULL) {
		SPDK_ERRLOG("Could not read JSON config file: %s\n", jsonFile);
		jsonAppLoadDone(-EINVAL, arg);
		return;
	}

	spdk_rpc_set_state(SPDK_RPC_RUNTIME);

	spdk_subsystem_load_config(json, (ssize_t)jsonSize,
					   jsonAppLoadDone, arg, true);
	free(json);
}

static void hfsJsonLoadFn(void *arg)
{
	spdk_subsystem_init(hfsSubsystemInitDone, arg);
}


static int hfsEnvSetup(void) {

	struct spdk_env_opts opts;
	spdk_env_opts_init(&opts);

	if (0 != spdk_env_init(&opts)) {
		return -1;
	}

	spdk_log_set_print_level(SPDK_LOG_NOTICE);
	spdk_log_set_level(SPDK_LOG_NOTICE);
	spdk_log_open(NULL);

	spdk_thread_lib_init(NULL, 0);
	globalThread = spdk_thread_create("global", NULL);
	spdk_set_thread(globalThread);

	bool done = false;
	//hfsJsonLoadFn(&done);

	waiter(globalThread, hfsJsonLoadFn, &done, &done);
	SPDK_NOTICELOG("jsonAppLoadDone complete\n");


	return 0;
	
}


int main(int argc, char *argv[]) {

	if (hfsEnvSetup()) {
		return -1;
	}

	SPDK_NOTICELOG("hfsEnvSetup success\n");

	SPDK_NOTICELOG("\n\n hfs mount start \n\n");
	Hfs *fs = calloc(1, sizeof(Hfs));
	hfsMount(fs);

	SPDK_NOTICELOG("\n\n hfs open start \n\n");
	HfsFile *file = calloc(1, sizeof(HfsFile));
	file->fs = fs;
	hfsOpen(file);


	SPDK_NOTICELOG("\n\n hfs write start \n\n");
	char *buffer = "ABCDEFGHIJKLMNOPQRSTUVWXYZ";
	hfsWrite(file, buffer, strlen(buffer));


	SPDK_NOTICELOG("\n\n hfs read start \n\n");
	char rBuffer[BUFFER_SIZE] = {0};
	hfsRead(file, rBuffer, BUFFER_SIZE);
	SPDK_NOTICELOG("READ BUFFER: %s\n", rBuffer);

	SPDK_NOTICELOG("\n\n hfs close start \n\n");
	hfsClose(file);

	free(file);

	SPDK_NOTICELOG("\n\n hfs umount start \n\n");
	hfsUmount(fs);
free(fs);
	
}


