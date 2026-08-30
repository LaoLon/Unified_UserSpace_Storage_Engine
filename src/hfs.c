

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

// 
typedef struct hfs_context_s {

	struct spdk_blob_store *blobstore;
	spdk_blob_id blob_id;
	struct spdk_blob *blob;
	uint64_t unit_size;
	uint64_t free_cluster_count;
	uint64_t num_cluster;
	uint8_t *write_buffer;
	uint8_t *read_buffer;
	struct spdk_io_channel *channel;

} hfs_context_t;

typedef struct hfs_s {

	struct spdk_blob_store *blobstore;
	uint64_t unit_size;
	uint64_t free_cluster_count;
	uint64_t num_cluster;

	struct spdk_io_channel *channel;
	bool finished;

} hfs_t;

typedef struct hfs_operation_s {
	int (*mount)(struct hfs_s *fs);
	int (*umount)(struct hfs_s *fs);
} hfs_operation_t;


typedef struct hfs_file_s {

	uint8_t filename[FILENAME_LENGTH];

	spdk_blob_id blob_id;
	struct spdk_blob *blob;
	hfs_t *fs;

	uint8_t *write_buffer;
	uint8_t *read_buffer;

	bool finished;
	
} hfs_file_t;

typedef struct hfs_file_operation_s {

	int (*open)(struct hfs_file_s *file);
	int (*read)(struct hfs_file_s *file);
	int (*write)(struct hfs_file_s *file);
	int (*close)(struct hfs_file_s *file);
	
} hfs_file_operation_t;

struct spdk_thread *global_thread = NULL;

static const int WAITER_MAX_TIME = 2000000000;

static bool waiter(struct spdk_thread *thread, spdk_msg_fn start_fn, void *ctx, bool *finished) {

	spdk_thread_send_msg(thread, start_fn, ctx);

	int waiter_count = 0;

	do {
		spdk_thread_poll(thread, 0, 0);
		waiter_count ++; 
	} while(!(*finished) && waiter_count < WAITER_MAX_TIME);

	if (!(*finished) && waiter_count >= WAITER_MAX_TIME) {
		return false; // timeout
	}

	return true;
}



static void hfs_spdk_bs_unload_cb(void *arg, int bserrno) {

	hfs_t *fs = (hfs_t *)arg;
	fs->finished = true;
	
}



static void hfs_do_umount(void *arg) {

	hfs_t *fs = (hfs_t *)arg;

	if (fs->blobstore) {
		if (fs->channel) {
			spdk_bs_free_io_channel(fs->channel);
		}
		spdk_bs_unload(fs->blobstore, hfs_spdk_bs_unload_cb, fs);
	}
	
}

static void hfs_spdk_blob_delete_cb(void *arg, int bserrno) {
	hfs_file_t *file = (hfs_file_t *)arg;
	
	spdk_free(file->write_buffer);
	spdk_free(file->read_buffer);

	SPDK_NOTICELOG("close complete\n");
	file->finished = true;
}


static void hfs_spdk_blob_close_cb(void *arg, int bserrno) {
	hfs_file_t *file = (hfs_file_t *)arg;
	
	spdk_bs_delete_blob(file->fs->blobstore, file->blob_id, hfs_spdk_blob_delete_cb, file);
}


static void hfs_do_close(void *arg) {

	hfs_file_t *file = (hfs_file_t *)arg;

	spdk_blob_close(file->blob, hfs_spdk_blob_close_cb, file);

}


static void hfs_spdk_blob_read_cb(void *arg, int bserrno) {

	hfs_file_t *file = (hfs_file_t *)arg;

	SPDK_NOTICELOG("READ BUFFER : %s\n", file->read_buffer);

	file->finished = true;

	//spdk_blob_close(ctx->blob, hfs_spdk_blob_close_cb, ctx);
	
}


static void hfs_do_read(void *arg) {

	hfs_file_t *file = (hfs_file_t *)arg;

	memset(file->read_buffer, 0x0, BUFFER_SIZE);

	spdk_blob_io_read(file->blob, file->fs->channel, file->read_buffer, 
		0, 1, hfs_spdk_blob_read_cb, file);

}


static void hfs_spdk_blob_write_cb(void *arg, int bserrno) {
	hfs_file_t *file = (hfs_file_t *)arg;
	SPDK_NOTICELOG("write complete\n");

	file->finished = true;
}


static void hfs_do_write(void *arg) {

	hfs_file_t *file = (hfs_file_t *)arg;

	spdk_blob_io_write(file->blob, file->fs->channel, file->write_buffer, 
		0, 1, hfs_spdk_blob_write_cb, file);

}

static void hfs_spdk_blob_sync_cb(void *arg, int bserrno) {
	hfs_file_t *file = (hfs_file_t *)arg;
	file->write_buffer = spdk_malloc(BUFFER_SIZE, 0x1000, NULL, SPDK_ENV_LCORE_ID_ANY, SPDK_MALLOC_DMA);
	if (file->write_buffer == NULL) {
		return ;
	}

	file->read_buffer = spdk_malloc(BUFFER_SIZE, 0x1000, NULL, SPDK_ENV_LCORE_ID_ANY, SPDK_MALLOC_DMA);
	if (file->read_buffer == NULL) {
		return ;
	}
	

	SPDK_NOTICELOG("open complete\n");

	file->finished = true;
}



static void hfs_spdk_blob_resize_cb(void *arg, int bserrno) {

	hfs_file_t *file = (hfs_file_t *)arg;
	
	uint64_t total = spdk_blob_get_num_clusters(file->blob);
	file->fs->num_cluster = total;

	SPDK_NOTICELOG("resize blob :%"PRIu64"\n", total);

	spdk_blob_sync_md(file->blob, hfs_spdk_blob_sync_cb, file);

}



static void hfs_spdk_bs_open_blob_cb(void *arg, struct spdk_blob *blb, int bserrno) {

	hfs_file_t *file = (hfs_file_t *)arg;

	file->blob = blb;

	uint64_t free_cluster = spdk_bs_free_cluster_count(file->fs->blobstore);  //
	SPDK_NOTICELOG("free_cluster : %"PRIu64"\n", free_cluster);
	file->fs->free_cluster_count = free_cluster;

	spdk_blob_resize(blb, free_cluster, hfs_spdk_blob_resize_cb, file);

}



static void hfs_spdk_bs_create_blob_cb(void *arg, spdk_blob_id blobid, int bserrno) {

	hfs_file_t *file = (hfs_file_t *)arg;

	file->blob_id = blobid;
	SPDK_NOTICELOG("blobid : %"PRIu64"\n", blobid);

	spdk_bs_open_blob(file->fs->blobstore, blobid, hfs_spdk_bs_open_blob_cb, file);
}


static void hfs_do_open(void *arg) {

	hfs_file_t *file = (hfs_file_t *)arg;
	
	spdk_bs_create_blob(file->fs->blobstore, hfs_spdk_bs_create_blob_cb, file);

}

static void hfs_spdk_bs_init_cb(void *arg, struct spdk_blob_store *bs,
		int bserrno) {

	hfs_t *fs = (hfs_t*)arg;

	uint64_t io_unit_size = spdk_bs_get_io_unit_size(bs);
	SPDK_NOTICELOG("io_unit_size : %"PRIu64"\n", io_unit_size);

	fs->unit_size = io_unit_size;
	fs->blobstore = bs;

	fs->channel = spdk_bs_alloc_io_channel(fs->blobstore);
	if (fs->channel == NULL) {
		return ;
	}

	fs->finished = true;

	SPDK_NOTICELOG("mount finished\n");

	//
}



static void zv2404_spdk_bdev_event_cb(enum spdk_bdev_event_type type, struct spdk_bdev *bdev,
				     void *event_ctx) {

	SPDK_NOTICELOG("zv2404_spdk_bdev_event_cb\n");

}




static void hfs_do_mount(void *arg) {

	hfs_t *fs = (hfs_t*)arg;
	
	struct spdk_bs_dev *bs_dev = NULL;

	int rc = spdk_bdev_create_bs_dev_ext("Malloc0", zv2404_spdk_bdev_event_cb, NULL, &bs_dev);
	if (rc != 0) {
		spdk_app_stop(0);
	}

	//int *count = malloc(sizeof(int));
	//*count = 2404;
	
	spdk_bs_init(bs_dev, NULL, hfs_spdk_bs_init_cb, fs);

	//sleep(10);
	SPDK_NOTICELOG("hfs_entry\n");

}


// filesystem
// load
static int hfs_mount(struct hfs_s *fs) {

	fs->finished = false;

	return waiter(global_thread, hfs_do_mount, fs, &fs->finished);
} 


// unload
static int hfs_umount(struct hfs_s *fs) {

	fs->finished = false;
	
	return waiter(global_thread, hfs_do_umount, fs, &fs->finished);
}


// file
// open
static int hfs_open(struct hfs_file_s *file) {

	file->finished = false;
	
	return waiter(global_thread, hfs_do_open, file, &file->finished);
	
}


// read

static int hfs_read(struct hfs_file_s *file, uint8_t *buffer, size_t count) {

	file->finished = false;
	waiter(global_thread, hfs_do_read, file, &file->finished);

	int len = strlen(file->read_buffer);
	memcpy(buffer, file->read_buffer, len);  //

	return len;
}


// write

static int hfs_write(struct hfs_file_s *file, const uint8_t *buffer, size_t count) {

	file->finished = false;

	memcpy(file->write_buffer, buffer, count);  // count / 512
	
	return waiter(global_thread, hfs_do_write, file, &file->finished);
}

// close
static int hfs_close(struct hfs_file_s *file) {

	file->finished = false;

	return waiter(global_thread, hfs_do_close, file, &file->finished);
}


// fd --> file


// zvfs.json

#ifdef HFS_DEFAULT_CONFIG
static const char *json_file = HFS_DEFAULT_CONFIG;
#else
static const char *json_file = "config/hfs.json";
#endif


static void json_app_load_done(int rc, void *ctx) {

	bool *done = ctx;
	*done = true;

	SPDK_NOTICELOG("json_app_load_done\n");
}

static void hfs_subsystem_init_done(int rc, void *arg)
{
	void *json;
	size_t json_size;

	if (rc != 0) {
		json_app_load_done(rc, arg);
		return;
	}

	json = spdk_posix_file_load_from_name(json_file, &json_size);
	if (json == NULL) {
		SPDK_ERRLOG("Could not read JSON config file: %s\n", json_file);
		json_app_load_done(-EINVAL, arg);
		return;
	}

	spdk_rpc_set_state(SPDK_RPC_RUNTIME);

	spdk_subsystem_load_config(json, (ssize_t)json_size,
					   json_app_load_done, arg, true);
	free(json);
}

static void hfs_json_load_fn(void *arg)
{
	spdk_subsystem_init(hfs_subsystem_init_done, arg);
}


static int hfs_env_setup(void) {

	struct spdk_env_opts opts;
	spdk_env_opts_init(&opts);

	if (0 != spdk_env_init(&opts)) {
		return -1;
	}

	spdk_log_set_print_level(SPDK_LOG_NOTICE);
	spdk_log_set_level(SPDK_LOG_NOTICE);
	spdk_log_open(NULL);

	spdk_thread_lib_init(NULL, 0);
	global_thread = spdk_thread_create("global", NULL);
	spdk_set_thread(global_thread);

	bool done = false;
	//hfs_json_load_fn(&done);

	waiter(global_thread, hfs_json_load_fn, &done, &done);
	SPDK_NOTICELOG("json_app_load_done complete\n");


	return 0;
	
}


int main(int argc, char *argv[]) {

	if (hfs_env_setup()) {
		return -1;
	}

	SPDK_NOTICELOG("hfs_env_setup success\n");

	SPDK_NOTICELOG("\n\n hfs mount start \n\n");
	hfs_t *fs = calloc(1, sizeof(hfs_t));
	hfs_mount(fs);

	SPDK_NOTICELOG("\n\n hfs open start \n\n");
	hfs_file_t *file = calloc(1, sizeof(hfs_file_t));
	file->fs = fs;
	hfs_open(file);


	SPDK_NOTICELOG("\n\n hfs write start \n\n");
	char *buffer = "ABCDEFGHIJKLMNOPQRSTUVWXYZ";
	hfs_write(file, buffer, strlen(buffer));


	SPDK_NOTICELOG("\n\n hfs read start \n\n");
	char rbuffer[BUFFER_SIZE] = {0};
	hfs_read(file, rbuffer, BUFFER_SIZE);
	SPDK_NOTICELOG("READ BUFFER: %s\n", rbuffer);

	SPDK_NOTICELOG("\n\n hfs close start \n\n");
	hfs_close(file);

	free(file);

	SPDK_NOTICELOG("\n\n hfs umount start \n\n");
	hfs_umount(fs);
free(fs);
	
}


