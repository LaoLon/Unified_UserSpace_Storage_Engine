#pragma once

/* C ABI between the UUSE core and the MinIO disk seam (Phase 9, Shard-9.1).
 *
 * Model: one UUSE engine per MinIO drive (process-wide weak registry keyed by
 * .conf path, same pattern as the TiKV shim), one shared path-op Client
 * serialized by a mutex with its frontend thread owned by the engine state,
 * and file data operations issued by the calling thread. All functions are
 * synchronous; errors are negative errno-style codes, byte counts are >= 0.
 * Paths are absolute UUSE namespace paths.
 *
 * Design: doc/design/uuse_minio_frontend.md 4.4; audit:
 * doc/review/uuse_minio_storage_audit_20261011.md. */

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct uuse_minio_engine uuse_minio_engine;
typedef struct uuse_minio_file uuse_minio_file;
typedef struct uuse_minio_dir uuse_minio_dir;

/* Open flags for uuse_minio_open. */
enum {
    UUSE_MINIO_OPEN_CREATE = 1u << 0,
    UUSE_MINIO_OPEN_EXCLUSIVE = 1u << 1,
    UUSE_MINIO_OPEN_TRUNCATE = 1u << 2,
};

/* Error buffer size for the diagnostic message of the last failure. */
#define UUSE_MINIO_ERRBUF 256

/* Snapshot of one namespace entry (file or directory). */
typedef struct uuse_minio_stat {
    uint64_t inode_id;
    uint32_t generation;
    uint32_t link_count;
    uint64_t logical_size;
    uint64_t mtime_ns;
    uint64_t ctime_ns;
    uint32_t directory; /* 1 when the path is a directory */
} uuse_minio_stat;

/* One directory entry returned by uuse_minio_dir_read. */
typedef struct uuse_minio_dirent {
    uint64_t inode_id;
    uint64_t reserved;
    uint32_t generation;
    uint32_t flags; /* bit 0: directory */
    char name[256]; /* NUL-terminated, truncated when longer */
} uuse_minio_dirent;

/* Opens (or reuses) the engine described by `conf_path` and mounts it.
 * Returns 0 or a negative errno; `errbuf` receives a message on failure. */
int uuse_minio_engine_open(const char *conf_path, uuse_minio_engine **out,
                           char *errbuf, size_t errlen);
/* Releases the caller reference; the engine stops when the last reference
 * (engine plus open files) goes away. */
void uuse_minio_engine_close(uuse_minio_engine *engine);

/* Disk capacity/usage: allocatable data bytes across data devices. */
int uuse_minio_disk_stats(uuse_minio_engine *engine, uint64_t *total_bytes,
                          uint64_t *free_bytes, uint64_t *used_bytes,
                          char *errbuf, size_t errlen);
/* Namespace inode counts (active inodes and the configured limit). */
int uuse_minio_inode_stats(uuse_minio_engine *engine, uint64_t *used_inodes,
                           uint64_t *max_inodes, char *errbuf, size_t errlen);

/* File operations; return 0 or a negative errno. */
int uuse_minio_open(uuse_minio_engine *engine, const char *path, unsigned flags,
                    uuse_minio_file **out, char *errbuf, size_t errlen);
void uuse_minio_close(uuse_minio_file *file);
/* Data operations; return bytes moved or a negative errno. */
long long uuse_minio_pread(uuse_minio_file *file, void *buf, size_t len,
                           uint64_t offset);
long long uuse_minio_pwrite(uuse_minio_file *file, const void *buf, size_t len,
                            uint64_t offset);
int uuse_minio_fdatasync(uuse_minio_file *file);
int uuse_minio_ftruncate(uuse_minio_file *file, uint64_t size);
int uuse_minio_fstat(uuse_minio_file *file, uuse_minio_stat *out);
int uuse_minio_allocate(uuse_minio_file *file, uint64_t offset, uint64_t length);

/* Path metadata and namespace operations; 0 or a negative errno (NotFound is
 * -ENOENT for stat-like calls without an `exists` out-parameter). */
int uuse_minio_path_stat(uuse_minio_engine *engine, const char *path,
                         uuse_minio_stat *out, char *errbuf, size_t errlen);
int uuse_minio_exists(uuse_minio_engine *engine, const char *path, int *exists,
                      char *errbuf, size_t errlen);
int uuse_minio_mkdir_all(uuse_minio_engine *engine, const char *path,
                         char *errbuf, size_t errlen);
int uuse_minio_remove(uuse_minio_engine *engine, const char *path,
                      uuse_minio_stat *removed, char *errbuf, size_t errlen);
/* Recursive delete for MinIO RemoveAll semantics. */
int uuse_minio_remove_all(uuse_minio_engine *engine, const char *path,
                          char *errbuf, size_t errlen);
int uuse_minio_rename(uuse_minio_engine *engine, const char *from,
                      const char *to, char *errbuf, size_t errlen);
int uuse_minio_link(uuse_minio_engine *engine, const char *from, const char *to,
                    char *errbuf, size_t errlen);

/* Bounded directory iteration (cursor-based, mirrors Client::getChildren). */
int uuse_minio_dir_open(uuse_minio_engine *engine, const char *path,
                        uuse_minio_dir **out, char *errbuf, size_t errlen);
/* Fills up to `max` entries; *count receives the number written and *eof is set
 * to 1 when the iteration is complete. */
int uuse_minio_dir_read(uuse_minio_dir *dir, uuse_minio_dirent *entries,
                        size_t max, size_t *count, int *eof);
void uuse_minio_dir_close(uuse_minio_dir *dir);

/* Version check for the linked shim. */
void uuse_minio_version(char *buf, size_t len);

#ifdef __cplusplus
}
#endif
