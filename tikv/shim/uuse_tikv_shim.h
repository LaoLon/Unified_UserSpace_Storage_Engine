#pragma once

/* C ABI between the UUSE core and TiKV frontends (raft-engine FileSystem and
 * future callers). All functions are synchronous; errors are negative
 * errno-style codes, byte counts are >= 0. See doc/shard/uuse_tidb_shard_plan.md
 * Shard-116 and doc/review/uuse_tikv_storage_audit_20261009.md. */

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct uuse_tikv_engine uuse_tikv_engine;
typedef struct uuse_tikv_file uuse_tikv_file;

/* Open flags for uuse_tikv_open. */
enum {
    UUSE_TIKV_OPEN_CREATE = 1u << 0,
    UUSE_TIKV_OPEN_TRUNCATE = 1u << 1,
    UUSE_TIKV_OPEN_EXCLUSIVE = 1u << 2,
};

/* Mounts the volume described by the UUSE .conf. Returns 0 or negative errno. */
int uuse_tikv_engine_open(const char *conf_path, uuse_tikv_engine **out);
void uuse_tikv_engine_close(uuse_tikv_engine *engine);

/* Path operations. */
int uuse_tikv_open(uuse_tikv_engine *engine, const char *path, unsigned flags,
                   uuse_tikv_file **out);
void uuse_tikv_close(uuse_tikv_file *file);
int uuse_tikv_remove(uuse_tikv_engine *engine, const char *path);
int uuse_tikv_rename(uuse_tikv_engine *engine, const char *from, const char *to);
/* Creates every missing directory component of an absolute path. */
int uuse_tikv_create_dirs(uuse_tikv_engine *engine, const char *path);
/* Lists regular-file names directly under `path` as NUL-separated bytes.
 * Returns the required buffer size when `buf` is NULL or too small, 0 on
 * success, or a negative errno on error. */
long long uuse_tikv_read_dir(uuse_tikv_engine *engine, const char *path,
                             char *buf, size_t buf_len);
/* Sets *exists to 1/0; returns 0 or negative errno. */
int uuse_tikv_exists(uuse_tikv_engine *engine, const char *path, int *exists);

/* Data operations; return bytes moved or negative errno. */
long long uuse_tikv_pread(uuse_tikv_file *file, void *buf, size_t len,
                          uint64_t offset);
long long uuse_tikv_pwrite(uuse_tikv_file *file, const void *buf, size_t len,
                           uint64_t offset);
long long uuse_tikv_append(uuse_tikv_file *file, const void *buf, size_t len);
int uuse_tikv_truncate(uuse_tikv_file *file, uint64_t size);
int uuse_tikv_allocate(uuse_tikv_file *file, uint64_t offset, uint64_t len);
int uuse_tikv_sync(uuse_tikv_file *file);
long long uuse_tikv_file_size(uuse_tikv_file *file);

#ifdef __cplusplus
}
#endif
