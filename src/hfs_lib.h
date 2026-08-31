#ifndef HFS_LIB_H
#define HFS_LIB_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/stat.h>
#include <sys/types.h>

#include "hfs.h"

#define HFS_FD_TABLE_SIZE	65536

struct hfs_dir;

struct hfs_dirent {
	uint64_t inodeId;
	char name[FILENAME_LENGTH];
};

int hfs_lib_init(void);
void hfs_lib_fini(void);

int hfs_lib_open(const char *path, int flags, mode_t mode);
ssize_t hfs_lib_read(int fd, void *buf, size_t count);
ssize_t hfs_lib_write(int fd, const void *buf, size_t count);
ssize_t hfs_lib_pread(int fd, void *buf, size_t count, off_t offset);
ssize_t hfs_lib_pwrite(int fd, const void *buf, size_t count, off_t offset);
off_t hfs_lib_lseek(int fd, off_t offset, int whence);
int hfs_lib_close(int fd);
int hfs_lib_unlink(const char *path);
int hfs_lib_ftruncate(int fd, off_t length);
int hfs_lib_fsync(int fd);
int hfs_lib_fstat(int fd, struct stat *st);

int hfs_lib_mkdir(const char *path, mode_t mode);
int hfs_lib_rmdir(const char *path);
int hfs_lib_rename(const char *oldPath, const char *newPath);
struct hfs_dir *hfs_lib_opendir(const char *path);
struct hfs_dirent *hfs_lib_readdir(struct hfs_dir *dir);
int hfs_lib_closedir(struct hfs_dir *dir);
int hfs_lib_stat(const char *path, struct stat *st);

#endif /* HFS_LIB_H */