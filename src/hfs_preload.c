/*
 * hfs_preload.c: LD_PRELOAD interception layer.
 * Resolves the real libc symbols via dlsym(RTLD_NEXT) and redirects
 * calls whose path falls under the HFS mount prefix to libhfs.
 */
#define _GNU_SOURCE

#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "hfs_lib.h"
#include "hfs_log.h"

/* Real libc symbols captured once at load time. */
static int (*real_open)(const char *path, int flags, ...);
static int (*real_open64)(const char *path, int flags, ...);
static int (*real_openat)(int dirfd, const char *path, int flags, ...);
static int (*real_openat64)(int dirfd, const char *path, int flags, ...);
static int (*real_close)(int fd);
static ssize_t (*real_read)(int fd, void *buf, size_t count);
static ssize_t (*real_write)(int fd, const void *buf, size_t count);
static ssize_t (*real_pread)(int fd, void *buf, size_t count, off_t offset);
static ssize_t (*real_pwrite)(int fd, const void *buf, size_t count, off_t offset);
static ssize_t (*real_pread64)(int fd, void *buf, size_t count, off64_t offset);
static ssize_t (*real_pwrite64)(int fd, const void *buf, size_t count, off64_t offset);
static off_t (*real_lseek)(int fd, off_t offset, int whence);
static off64_t (*real_lseek64)(int fd, off64_t offset, int whence);
static int (*real_dup)(int oldfd);
static int (*real_dup2)(int oldfd, int newfd);
static int (*real_dup3)(int oldfd, int newfd, int flags);

/* Length of the HFS mount prefix (default "/hfs/"). */
static const char *hfs_prefix;
static size_t hfs_prefix_len;

/*
 * Set when this process inherited an initialized HFS environment from
 * its parent (fork+exec). Such children must NOT re-initialize SPDK:
 * a second DPDK primary conflicts with the parent and hangs. They run
 * in pass-through mode instead.
 */
static bool hfs_inherited;

/*
 * Returns true when the given path lives under the HFS mount prefix.
 * A path equals to the prefix itself (without trailing slash) is not
 * managed, so the mount point directory can live on the real fs.
 */
static bool hfs_path_managed(const char *path) {
	if (hfs_prefix == NULL || path == NULL || hfs_inherited) {
		return false;
	}
	return strncmp(path, hfs_prefix, hfs_prefix_len) == 0;
}

/* Resolve a libc symbol once via dlsym(RTLD_NEXT). */
static void *hfs_real_sym(const char *name) {
	void *sym = dlsym(RTLD_NEXT, name);
	if (sym == NULL) {
		fprintf(stderr, "libhfs: dlsym(%s) failed: %s\n", name, dlerror());
	}
	return sym;
}

/* Shared body of the open/open64 interception. */
static int hfs_intercept_open(const char *path, int flags, mode_t mode) {
	if (hfs_path_managed(path)) {
		if (hfs_lib_init() != 0) {
			errno = ENODEV;
			return -1;
		}
		/* Strip the mount prefix; the library sees the inner path. */
		return hfs_lib_open(path + hfs_prefix_len, flags, mode);
	}
	return -2;	/* not managed: caller falls through to real libc */
}

/*
 * Intercepted open(2): managed paths go to hfs_lib_open, everything
 * else is passed through to the real libc open.
 * The library is initialized lazily on the first managed call: doing
 * it in the constructor would deadlock with glibc's loader lock while
 * SPDK/DPDK spins up threads.
 */
int open(const char *path, int flags, ...) {
	va_list ap;
	mode_t mode = 0;
	int rc;

	va_start(ap, flags);
	mode = va_arg(ap, mode_t);
	va_end(ap);

	rc = hfs_intercept_open(path, flags, mode);
	if (rc != -2) {
		return rc;
	}
	return real_open(path, flags, mode);
}

/*
 * Intercepted open64(2) (large-file variant, used by dash/busybox).
 * Same routing as open(2).
 */
int open64(const char *path, int flags, ...) {
	va_list ap;
	mode_t mode = 0;
	int rc;

	va_start(ap, flags);
	mode = va_arg(ap, mode_t);
	va_end(ap);

	rc = hfs_intercept_open(path, flags, mode);
	if (rc != -2) {
		return rc;
	}
	return real_open64(path, flags, mode);
}

/*
 * Intercepted openat(2). Only AT_FDCWD with a managed path is routed
 * to HFS; everything else (including dirfd-relative paths) is passed
 * through to the real libc call.
 */
int openat(int dirfd, const char *path, int flags, ...) {
	va_list ap;
	mode_t mode = 0;

	va_start(ap, flags);
	mode = va_arg(ap, mode_t);
	va_end(ap);

	if (dirfd == AT_FDCWD) {
		int rc = hfs_intercept_open(path, flags, mode);
		if (rc != -2) {
			return rc;
		}
	}
	return real_openat(dirfd, path, flags, mode);
}

/*
 * Intercepted openat64(2) (large-file variant). Same routing as openat.
 */
int openat64(int dirfd, const char *path, int flags, ...) {
	va_list ap;
	mode_t mode = 0;

	va_start(ap, flags);
	mode = va_arg(ap, mode_t);
	va_end(ap);

	if (dirfd == AT_FDCWD) {
		int rc = hfs_intercept_open(path, flags, mode);
		if (rc != -2) {
			return rc;
		}
	}
	return real_openat64(dirfd, path, flags, mode);
}

/* Intercepted close(2): HFS fds are closed via hfs_lib_close. */
int close(int fd) {
	if (hfs_is_hfs_fd(fd)) {
		return hfs_lib_close(fd);
	}
	return real_close(fd);
}

/* Intercepted read(2). */
ssize_t read(int fd, void *buf, size_t count) {
	if (hfs_is_hfs_fd(fd)) {
		return hfs_lib_read(fd, buf, count);
	}
	return real_read(fd, buf, count);
}

/* Intercepted write(2). */
ssize_t write(int fd, const void *buf, size_t count) {
	if (hfs_is_hfs_fd(fd)) {
		return hfs_lib_write(fd, buf, count);
	}
	return real_write(fd, buf, count);
}

/* Intercepted pread(2). */
ssize_t pread(int fd, void *buf, size_t count, off_t offset) {
	if (hfs_is_hfs_fd(fd)) {
		return hfs_lib_pread(fd, buf, count, offset);
	}
	return real_pread(fd, buf, count, offset);
}

/* Intercepted pwrite(2). */
ssize_t pwrite(int fd, const void *buf, size_t count, off_t offset) {
	if (hfs_is_hfs_fd(fd)) {
		return hfs_lib_pwrite(fd, buf, count, offset);
	}
	return real_pwrite(fd, buf, count, offset);
}

/* Intercepted lseek(2). */
off_t lseek(int fd, off_t offset, int whence) {
	if (hfs_is_hfs_fd(fd)) {
		return hfs_lib_lseek(fd, offset, whence);
	}
	return real_lseek(fd, offset, whence);
}

/* Intercepted lseek64(2) (large-file variant, used by 64-bit builds). */
off64_t lseek64(int fd, off64_t offset, int whence) {
	if (hfs_is_hfs_fd(fd)) {
		return (off64_t)hfs_lib_lseek(fd, (off_t)offset, whence);
	}
	return real_lseek64(fd, offset, whence);
}

/* Intercepted pread64(2) (large-file variant). */
ssize_t pread64(int fd, void *buf, size_t count, off64_t offset) {
	if (hfs_is_hfs_fd(fd)) {
		return hfs_lib_pread(fd, buf, count, (off_t)offset);
	}
	return real_pread64(fd, buf, count, offset);
}

/* Intercepted pwrite64(2) (large-file variant). */
ssize_t pwrite64(int fd, const void *buf, size_t count, off64_t offset) {
	if (hfs_is_hfs_fd(fd)) {
		return hfs_lib_pwrite(fd, buf, count, (off_t)offset);
	}
	return real_pwrite64(fd, buf, count, offset);
}

/*
 * Intercepted dup(2): duplicate an HFS fd. The kernel side is reserved
 * with the real dup (the placeholder /dev/null is copied), then the fd
 * table slot is copied so both fds share the same file handle.
 */
int dup(int oldfd) {
	if (hfs_is_hfs_fd(oldfd)) {
		int nf = real_dup(oldfd);
		if (nf >= 0 && hfs_lib_fd_copy(oldfd, nf) != 0) {
			real_close(nf);
			return -1;
		}
		return nf;
	}
	return real_dup(oldfd);
}

/* Intercepted dup2(2): same slot copying as dup(). */
int dup2(int oldfd, int newfd) {
	if (hfs_is_hfs_fd(oldfd)) {
		int rc = real_dup2(oldfd, newfd);
		if (rc >= 0 && hfs_lib_fd_copy(oldfd, newfd) != 0) {
			return -1;
		}
		return rc;
	}
	return real_dup2(oldfd, newfd);
}

/* Intercepted dup3(2): same slot copying as dup(). */
int dup3(int oldfd, int newfd, int flags) {
	if (hfs_is_hfs_fd(oldfd)) {
		int rc = real_dup3(oldfd, newfd, flags);
		if (rc >= 0 && hfs_lib_fd_copy(oldfd, newfd) != 0) {
			return -1;
		}
		return rc;
	}
	return real_dup3(oldfd, newfd, flags);
}

/*
 * Runs before main(): capture the real libc symbols and the mount
 * prefix. SPDK initialization is deferred to the first managed call:
 * running it here would deadlock with glibc's loader lock. Children
 * of an initialized process (fork+exec) run in pass-through mode.
 */
__attribute__((constructor))
static void hfs_preload_ctor(void) {
	HFS_LOG_DEBUG("libhfs preload active");

	hfs_prefix = getenv("HFS_MOUNT_PREFIX");
	if (hfs_prefix == NULL) {
		hfs_prefix = "/hfs/";
	}
	hfs_prefix_len = strlen(hfs_prefix);

	real_open = hfs_real_sym("open");
	real_open64 = hfs_real_sym("open64");
	real_openat = hfs_real_sym("openat");
	real_openat64 = hfs_real_sym("openat64");
	real_close = hfs_real_sym("close");
	real_read = hfs_real_sym("read");
	real_write = hfs_real_sym("write");
	real_pread = hfs_real_sym("pread");
	real_pwrite = hfs_real_sym("pwrite");
	real_pread64 = hfs_real_sym("pread64");
	real_pwrite64 = hfs_real_sym("pwrite64");
	real_lseek = hfs_real_sym("lseek");
	real_lseek64 = hfs_real_sym("lseek64");
	real_dup = hfs_real_sym("dup");
	real_dup2 = hfs_real_sym("dup2");
	real_dup3 = hfs_real_sym("dup3");

	if (getenv("_HFS_INIT_DONE") != NULL) {
		hfs_inherited = true;
		HFS_LOG_DEBUG("libhfs pass-through mode (inherited)");
	}
}

/* Runs at process exit: unmount and free the filesystem. */
__attribute__((destructor))
static void hfs_preload_dtor(void) {
	hfs_lib_fini();
}