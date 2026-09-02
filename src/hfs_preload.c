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
static ssize_t (*real_readv)(int fd, const struct iovec *iov, int iovcnt);
static ssize_t (*real_writev)(int fd, const struct iovec *iov, int iovcnt);
static ssize_t (*real_preadv)(int fd, const struct iovec *iov, int iovcnt,
			      off_t offset);
static ssize_t (*real_pwritev)(int fd, const struct iovec *iov, int iovcnt,
			       off_t offset);
static ssize_t (*real_preadv2)(int fd, const struct iovec *iov, int iovcnt,
			       off_t offset, int flags);
static ssize_t (*real_pwritev2)(int fd, const struct iovec *iov, int iovcnt,
				off_t offset, int flags);
static ssize_t (*real___read)(int fd, void *buf, size_t count);
static ssize_t (*real___write)(int fd, const void *buf, size_t count);
static ssize_t (*real_pread)(int fd, void *buf, size_t count, off_t offset);
static ssize_t (*real_pwrite)(int fd, const void *buf, size_t count, off_t offset);
static ssize_t (*real_pread64)(int fd, void *buf, size_t count, off64_t offset);
static ssize_t (*real_pwrite64)(int fd, const void *buf, size_t count, off64_t offset);
static off_t (*real_lseek)(int fd, off_t offset, int whence);
static off64_t (*real_lseek64)(int fd, off64_t offset, int whence);
static int (*real_dup)(int oldfd);
static int (*real_dup2)(int oldfd, int newfd);
static int (*real_dup3)(int oldfd, int newfd, int flags);
static int (*real_fsync)(int fd);
static int (*real_fdatasync)(int fd);
static int (*real_sync_file_range)(int fd, off64_t offset, off64_t nbytes,
				   unsigned int flags);
static int (*real_ftruncate)(int fd, off_t length);
static int (*real_ftruncate64)(int fd, off64_t length);
static int (*real_fallocate)(int fd, int mode, off_t offset, off_t len);
static int (*real_fallocate64)(int fd, int mode, off64_t offset, off64_t len);
static int (*real_posix_fallocate)(int fd, off_t offset, off_t len);
static int (*real_unlink)(const char *path);
static int (*real_unlinkat)(int dirfd, const char *path, int flags);
static int (*real_mkdir)(const char *path, mode_t mode);
static int (*real_rmdir)(const char *path);
static int (*real_rename)(const char *oldpath, const char *newpath);
static int (*real_renameat)(int olddirfd, const char *oldpath,
			    int newdirfd, const char *newpath);
static int (*real_renameat2)(int olddirfd, const char *oldpath,
			     int newdirfd, const char *newpath,
			     unsigned int flags);
static int (*real_stat)(const char *path, struct stat *st);
static int (*real_lstat)(const char *path, struct stat *st);
static int (*real_fstat)(int fd, struct stat *st);
static int (*real_fstatat)(int dirfd, const char *path, struct stat *st, int flags);
static int (*real_access)(const char *path, int mode);
static int (*real_faccessat)(int dirfd, const char *path, int mode, int flags);
static int (*real_fcntl)(int fd, int cmd, ...);
static int (*real_fcntl64)(int fd, int cmd, ...);
static DIR *(*real_opendir)(const char *path);
static struct dirent *(*real_readdir)(DIR *dirp);
static struct dirent64 *(*real_readdir64)(DIR *dirp);
static int (*real_closedir)(DIR *dirp);
static DIR *(*real_fdopendir)(int fd);
static ssize_t (*real_readlink)(const char *path, char *buf, size_t bufsiz);
static ssize_t (*real_readlinkat)(int dirfd, const char *path, char *buf,
				  size_t bufsiz);
/* glibc versioned stat family (__xstat/__fxstat/__lxstat/__fxstatat). */
static int (*real___xstat)(int ver, const char *path, struct stat *st);
static int (*real___lxstat)(int ver, const char *path, struct stat *st);
static int (*real___fxstat)(int ver, int fd, struct stat *st);
static int (*real___fxstatat)(int ver, int dirfd, const char *path,
			      struct stat *st, int flags);
/* 64-bit stat variants used by 64-bit binaries (python, etc.). */
static int (*real_stat64)(const char *path, struct stat64 *st);
static int (*real_lstat64)(const char *path, struct stat64 *st);
static int (*real_fstat64)(int fd, struct stat64 *st);
static int (*real_fstatat64)(int dirfd, const char *path, struct stat64 *st,
			     int flags);
static int (*real___xstat64)(int ver, const char *path, struct stat64 *st);
static int (*real___lxstat64)(int ver, const char *path, struct stat64 *st);
static int (*real___fxstat64)(int ver, int fd, struct stat64 *st);
static int (*real___fxstatat64)(int ver, int dirfd, const char *path,
				struct stat64 *st, int flags);

/* Length of the HFS mount prefix (default "/hfs/"). */
static const char *hfs_prefix;
static size_t hfs_prefix_len;

/*
 * HFS working directory state: when the application chdir()s into a
 * managed path, relative paths are resolved against it (mariadbd does
 * chdir(datadir) then opens files by relative name). Stores the full
 * path including the mount prefix.
 */
static char g_hfs_cwd[PATH_MAX] = "/";
static bool g_hfs_cwd_active;

static int (*real_chdir)(const char *path);
static int (*real_fchdir)(int fd);

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

/*
 * Translate an application path into an HFS-relative path (prefix
 * stripped). Absolute paths are checked directly; relative paths are
 * resolved against the tracked HFS cwd (set by chdir). Returns NULL
 * when the path is not managed. The result is written to 'buf' (caller
 * provided, so two paths can be translated in one call without
 * clobbering each other, e.g. rename(old, new)).
 */
static const char *hfs_view_path_buf(const char *path, char *buf,
				     size_t size) {
	if (path == NULL) {
		return NULL;
	}
	if (path[0] == '/') {
		if (!hfs_path_managed(path)) {
			return NULL;
		}
		return path + hfs_prefix_len;
	}
	if (!g_hfs_cwd_active) {
		return NULL;
	}
	if (snprintf(buf, size, "%s/%s", g_hfs_cwd, path)
	    >= (int)size) {
		return NULL;
	}
	if (!hfs_path_managed(buf)) {
		return NULL;
	}
	return buf + hfs_prefix_len;
}

/* TLS convenience wrapper of hfs_view_path_buf. */
static const char *hfs_view_path(const char *path) {
	static __thread char buf[PATH_MAX];

	return hfs_view_path_buf(path, buf, sizeof(buf));
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
	const char *vp = hfs_view_path(path);
	if (vp != NULL) {
		if (hfs_lib_init() != 0) {
			errno = ENODEV;
			return -1;
		}
		/* The library sees the inner (prefix-stripped) path. */
		return hfs_lib_open(vp, flags, mode);
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
	} else if (hfs_is_hfs_fd(dirfd)) {
		/* Relative to an HFS directory fd (aria/InnoDB use this). */
		if (hfs_lib_init() != 0) {
			errno = ENODEV;
			return -1;
		}
		return hfs_lib_openat(dirfd, path, flags, mode);
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
	} else if (hfs_is_hfs_fd(dirfd)) {
		if (hfs_lib_init() != 0) {
			errno = ENODEV;
			return -1;
		}
		return hfs_lib_openat(dirfd, path, flags, mode);
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

/* Intercepted __read: glibc internal symbol used by fopen/fread paths. */
ssize_t __read(int fd, void *buf, size_t count) {
	if (hfs_is_hfs_fd(fd)) {
		return hfs_lib_read(fd, buf, count);
	}
	return real___read(fd, buf, count);
}

/* Intercepted __write: glibc internal symbol used by fwrite paths. */
ssize_t __write(int fd, const void *buf, size_t count) {
	if (hfs_is_hfs_fd(fd)) {
		return hfs_lib_write(fd, buf, count);
	}
	return real___write(fd, buf, count);
}

/* Intercepted write(2). */
ssize_t write(int fd, const void *buf, size_t count) {
	if (hfs_is_hfs_fd(fd)) {
		return hfs_lib_write(fd, buf, count);
	}
	return real_write(fd, buf, count);
}

/*
 * Scatter/gather IO helpers. RocksDB reads CURRENT/MANIFEST with
 * readv, which bypasses our read interceptor and hits the kernel
 * /dev/null placeholder (returns 0 -> "CURRENT does not end with
 * newline"). Route vector IO on HFS fds through the library.
 */
static ssize_t hfs_iovec_total(const struct iovec *iov, int iovcnt) {
	size_t total = 0;
	int i;

	for (i = 0; i < iovcnt; i++) {
		total += iov[i].iov_len;
	}
	return (ssize_t)total;
}

static ssize_t hfs_iovec_scatter(char *src, ssize_t n,
				 const struct iovec *iov, int iovcnt) {
	ssize_t done = 0;
	int i;

	for (i = 0; i < iovcnt && done < n; i++) {
		size_t len = iov[i].iov_len;
		if ((ssize_t)len > n - done) {
			len = (size_t)(n - done);
		}
		memcpy(iov[i].iov_base, src + done, len);
		done += (ssize_t)len;
	}
	return done;
}

static ssize_t hfs_iovec_gather(char *dst, size_t cap,
				const struct iovec *iov, int iovcnt) {
	size_t total = 0;
	int i;

	for (i = 0; i < iovcnt; i++) {
		size_t len = iov[i].iov_len;
		if (total + len > cap) {
			len = cap - total;
		}
		if (len > 0) {
			memcpy(dst + total, iov[i].iov_base, len);
			total += len;
		}
	}
	return (ssize_t)total;
}

/* Intercepted readv(2): HFS fds read via a temporary buffer. */
ssize_t readv(int fd, const struct iovec *iov, int iovcnt) {
	ssize_t total;
	char *tmp;
	ssize_t n;

	if (!hfs_is_hfs_fd(fd)) {
		return real_readv(fd, iov, iovcnt);
	}
	total = hfs_iovec_total(iov, iovcnt);
	if (total == 0) {
		return 0;
	}
	tmp = malloc((size_t)total);
	if (tmp == NULL) {
		errno = ENOMEM;
		return -1;
	}
	n = hfs_lib_read(fd, tmp, (size_t)total);
	if (n > 0) {
		n = hfs_iovec_scatter(tmp, n, iov, iovcnt);
	}
	free(tmp);
	return n;
}

/* Intercepted writev(2): HFS fds write from a temporary buffer. */
ssize_t writev(int fd, const struct iovec *iov, int iovcnt) {
	ssize_t total;
	char *tmp;
	ssize_t n;

	if (!hfs_is_hfs_fd(fd)) {
		return real_writev(fd, iov, iovcnt);
	}
	total = hfs_iovec_total(iov, iovcnt);
	if (total == 0) {
		return 0;
	}
	tmp = malloc((size_t)total);
	if (tmp == NULL) {
		errno = ENOMEM;
		return -1;
	}
	n = hfs_iovec_gather(tmp, (size_t)total, iov, iovcnt);
	if (n > 0) {
		n = hfs_lib_write(fd, tmp, (size_t)n);
	}
	free(tmp);
	return n;
}

/* Intercepted preadv(2): like readv at an explicit offset. */
ssize_t preadv(int fd, const struct iovec *iov, int iovcnt, off_t offset) {
	ssize_t total;
	char *tmp;
	ssize_t n;

	if (!hfs_is_hfs_fd(fd)) {
		return real_preadv(fd, iov, iovcnt, offset);
	}
	total = hfs_iovec_total(iov, iovcnt);
	if (total == 0) {
		return 0;
	}
	tmp = malloc((size_t)total);
	if (tmp == NULL) {
		errno = ENOMEM;
		return -1;
	}
	n = hfs_lib_pread(fd, tmp, (size_t)total, offset);
	if (n > 0) {
		n = hfs_iovec_scatter(tmp, n, iov, iovcnt);
	}
	free(tmp);
	return n;
}

/* Intercepted pwritev(2): like writev at an explicit offset. */
ssize_t pwritev(int fd, const struct iovec *iov, int iovcnt, off_t offset) {
	ssize_t total;
	char *tmp;
	ssize_t n;

	if (!hfs_is_hfs_fd(fd)) {
		return real_pwritev(fd, iov, iovcnt, offset);
	}
	total = hfs_iovec_total(iov, iovcnt);
	if (total == 0) {
		return 0;
	}
	tmp = malloc((size_t)total);
	if (tmp == NULL) {
		errno = ENOMEM;
		return -1;
	}
	n = hfs_iovec_gather(tmp, (size_t)total, iov, iovcnt);
	if (n > 0) {
		n = hfs_lib_pwrite(fd, tmp, (size_t)n, offset);
	}
	free(tmp);
	return n;
}

/* Intercepted preadv2(2): RocksDB uses it for reads with RWF flags. */
ssize_t preadv2(int fd, const struct iovec *iov, int iovcnt, off_t offset,
		int flags) {
	if (!hfs_is_hfs_fd(fd)) {
		return real_preadv2(fd, iov, iovcnt, offset, flags);
	}
	if (flags != 0) {
		errno = EOPNOTSUPP;
		return -1;
	}
	return preadv(fd, iov, iovcnt, offset);
}

/* Intercepted pwritev2(2): like pwritev with RWF flags. */
ssize_t pwritev2(int fd, const struct iovec *iov, int iovcnt, off_t offset,
		 int flags) {
	if (!hfs_is_hfs_fd(fd)) {
		return real_pwritev2(fd, iov, iovcnt, offset, flags);
	}
	if (flags != 0) {
		errno = EOPNOTSUPP;
		return -1;
	}
	return pwritev(fd, iov, iovcnt, offset);
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

/* Intercepted fsync(2): flush HFS data and metadata. */
int fsync(int fd) {
	if (hfs_is_hfs_fd(fd)) {
		return hfs_lib_fsync(fd);
	}
	return real_fsync(fd);
}

/* Intercepted fdatasync(2): same flush path as fsync. */
int fdatasync(int fd) {
	if (hfs_is_hfs_fd(fd)) {
		return hfs_lib_fsync(fd);
	}
	return real_fdatasync(fd);
}

/*
 * Intercepted sync_file_range(2). RocksDB probes support with a zero
 * range on a fresh fd; the kernel answers EBADF/EINVAL for our
 * /dev/null placeholder, which trips its assertion. Report ENOSYS so
 * RocksDB falls back to fsync.
 */
int sync_file_range(int fd, off64_t offset, off64_t nbytes,
		    unsigned int flags) {
	if (hfs_is_hfs_fd(fd)) {
		errno = ENOSYS;
		return -1;
	}
	return real_sync_file_range(fd, offset, nbytes, flags);
}

/* Intercepted ftruncate(2). */
int ftruncate(int fd, off_t length) {
	if (hfs_is_hfs_fd(fd)) {
		return hfs_lib_ftruncate(fd, length);
	}
	return real_ftruncate(fd, length);
}

/* Intercepted ftruncate64(2) (large-file variant). */
int ftruncate64(int fd, off64_t length) {
	if (hfs_is_hfs_fd(fd)) {
		return hfs_lib_ftruncate(fd, (off_t)length);
	}
	return real_ftruncate64(fd, length);
}

/* Intercepted fallocate(2): HFS grows on demand, so KEEP_SIZE is free. */
int fallocate(int fd, int mode, off_t offset, off_t len) {
	if (hfs_is_hfs_fd(fd)) {
		return hfs_lib_fallocate(fd, mode, offset, len);
	}
	return real_fallocate(fd, mode, offset, len);
}

/* Intercepted fallocate64(2): large-file variant, used by mariadbd. */
int fallocate64(int fd, int mode, off64_t offset, off64_t len) {
	if (hfs_is_hfs_fd(fd)) {
		return hfs_lib_fallocate(fd, mode, (off_t)offset, (off_t)len);
	}
	return real_fallocate64(fd, mode, offset, len);
}

/*
 * Intercepted posix_fallocate(3). The glibc wrapper issues the
 * fallocate syscall directly, bypassing our symbol interceptor, which
 * would hit the kernel /dev/null placeholder fd (EBADF). Route HFS fds
 * to the library; note posix_fallocate returns an errno, not -1.
 */
int posix_fallocate(int fd, off_t offset, off_t len) {
	if (hfs_is_hfs_fd(fd)) {
		if (hfs_lib_fallocate(fd, 0, offset, len) == 0) {
			return 0;
		}
		return errno;
	}
	return real_posix_fallocate(fd, offset, len);
}

/* Intercepted unlink(2): managed paths remove an HFS file. */
int unlink(const char *path) {
	const char *vp = hfs_view_path(path);
	if (vp != NULL) {
		if (hfs_lib_init() != 0) {
			errno = ENODEV;
			return -1;
		}
		return hfs_lib_unlink(vp);
	}
	return real_unlink(path);
}

/*
 * Intercepted unlinkat(2). AT_FDCWD with a managed path is routed to
 * HFS (AT_REMOVEDIR maps to rmdir); everything else passes through.
 */
int unlinkat(int dirfd, const char *path, int flags) {
	const char *vp = hfs_view_path(path);
	if (dirfd == AT_FDCWD && vp != NULL) {
		if (hfs_lib_init() != 0) {
			errno = ENODEV;
			return -1;
		}
		if (flags & AT_REMOVEDIR) {
			return hfs_lib_rmdir(vp);
		}
		return hfs_lib_unlink(vp);
	}
	return real_unlinkat(dirfd, path, flags);
}

/* Intercepted mkdir(2): managed paths create an HFS directory. */
int mkdir(const char *path, mode_t mode) {
	const char *vp = hfs_view_path(path);
	if (vp != NULL) {
		if (hfs_lib_init() != 0) {
			errno = ENODEV;
			return -1;
		}
		return hfs_lib_mkdir(vp, mode);
	}
	return real_mkdir(path, mode);
}

/* Intercepted rmdir(2): managed paths remove an HFS directory. */
int rmdir(const char *path) {
	const char *vp = hfs_view_path(path);
	if (vp != NULL) {
		if (hfs_lib_init() != 0) {
			errno = ENODEV;
			return -1;
		}
		return hfs_lib_rmdir(vp);
	}
	return real_rmdir(path);
}

/*
 * Intercepted rename(2). Both sides must be managed by HFS (or both
 * outside); a mixed pair is rejected with EXDEV like a cross-fs move.
 */
int rename(const char *oldpath, const char *newpath) {
	char oldBuf[PATH_MAX];
	char newBuf[PATH_MAX];
	const char *oldvp = hfs_view_path_buf(oldpath, oldBuf, sizeof(oldBuf));
	const char *newvp = hfs_view_path_buf(newpath, newBuf, sizeof(newBuf));
	bool oldManaged = (oldvp != NULL);
	bool newManaged = (newvp != NULL);

	if (oldManaged || newManaged) {
		if (oldManaged != newManaged) {
			errno = EXDEV;
			return -1;
		}
		if (hfs_lib_init() != 0) {
			errno = ENODEV;
			return -1;
		}
		return hfs_lib_rename(oldvp, newvp);
	}
	return real_rename(oldpath, newpath);
}

/* Intercepted renameat(2): AT_FDCWD relative paths, same rules as rename. */
int renameat(int olddirfd, const char *oldpath,
	     int newdirfd, const char *newpath) {
	char oldBuf[PATH_MAX];
	char newBuf[PATH_MAX];
	const char *oldvp = hfs_view_path_buf(oldpath, oldBuf, sizeof(oldBuf));
	const char *newvp = hfs_view_path_buf(newpath, newBuf, sizeof(newBuf));
	bool oldManaged = (olddirfd == AT_FDCWD) && (oldvp != NULL);
	bool newManaged = (newdirfd == AT_FDCWD) && (newvp != NULL);

	if (oldManaged || newManaged) {
		if (oldManaged != newManaged) {
			errno = EXDEV;
			return -1;
		}
		if (hfs_lib_init() != 0) {
			errno = ENODEV;
			return -1;
		}
		return hfs_lib_rename(oldvp, newvp);
	}
	return real_renameat(olddirfd, oldpath, newdirfd, newpath);
}

/* Intercepted renameat2(2): only flags=0 (plain rename) is supported. */
int renameat2(int olddirfd, const char *oldpath,
	      int newdirfd, const char *newpath, unsigned int flags) {
	char oldBuf[PATH_MAX];
	char newBuf[PATH_MAX];
	const char *oldvp = hfs_view_path_buf(oldpath, oldBuf, sizeof(oldBuf));
	const char *newvp = hfs_view_path_buf(newpath, newBuf, sizeof(newBuf));
	bool oldManaged = (olddirfd == AT_FDCWD) && (oldvp != NULL);
	bool newManaged = (newdirfd == AT_FDCWD) && (newvp != NULL);

	if (oldManaged || newManaged) {
		if (oldManaged != newManaged) {
			errno = EXDEV;
			return -1;
		}
		if (flags != 0) {
			errno = EOPNOTSUPP;
			return -1;
		}
		if (hfs_lib_init() != 0) {
			errno = ENODEV;
			return -1;
		}
		return hfs_lib_rename(oldvp, newvp);
	}
	return real_renameat2(olddirfd, oldpath, newdirfd, newpath, flags);
}

/* Intercepted chdir(2): managed paths switch the tracked HFS cwd. */
int chdir(const char *path) {
	const char *vp = hfs_view_path(path);
	struct stat st;

	if (vp != NULL) {
		if (hfs_lib_init() != 0) {
			errno = ENODEV;
			return -1;
		}
		/* Must be an existing directory. */
		if (hfs_lib_stat(vp, &st) != 0 || !S_ISDIR(st.st_mode)) {
			errno = ENOTDIR;
			return -1;
		}
		snprintf(g_hfs_cwd, sizeof(g_hfs_cwd), "%s", path);
		g_hfs_cwd_active = true;
		return 0;
	}
	if (g_hfs_cwd_active) {
		/* Leaving the HFS tree: fall back to kernel cwd semantics. */
		g_hfs_cwd_active = false;
	}
	return real_chdir(path);
}

/* Intercepted fchdir(2): HFS directory fds are not supported. */
int fchdir(int fd) {
	if (hfs_is_hfs_fd(fd)) {
		errno = ENOTSUP;
		return -1;
	}
	return real_fchdir(fd);
}

/* Intercepted stat(2): managed paths stat an HFS inode. */
int stat(const char *path, struct stat *st) {
	const char *vp = hfs_view_path(path);
	if (vp != NULL) {
		if (hfs_lib_init() != 0) {
			errno = ENODEV;
			return -1;
		}
		return hfs_lib_stat(vp, st);
	}
	return real_stat(path, st);
}

/* Intercepted lstat(2): HFS has no symlinks, same as stat. */
int lstat(const char *path, struct stat *st) {
	const char *vp = hfs_view_path(path);
	if (vp != NULL) {
		if (hfs_lib_init() != 0) {
			errno = ENODEV;
			return -1;
		}
		return hfs_lib_stat(vp, st);
	}
	return real_lstat(path, st);
}

/* Intercepted fstat(2): HFS fds report the inode metadata. */
int fstat(int fd, struct stat *st) {
	if (hfs_is_hfs_fd(fd)) {
		return hfs_lib_fstat(fd, st);
	}
	return real_fstat(fd, st);
}

/* Intercepted fstatat(2) / newfstatat: AT_FDCWD with a managed path. */
int fstatat(int dirfd, const char *path, struct stat *st, int flags) {
	const char *vp = hfs_view_path(path);
	if (dirfd == AT_FDCWD && vp != NULL) {
		if (hfs_lib_init() != 0) {
			errno = ENODEV;
			return -1;
		}
		return hfs_lib_stat(vp, st);
	}
	return real_fstatat(dirfd, path, st, flags);
}

/* Intercepted newfstatat(2): glibc legacy name of fstatat. */
int newfstatat(int dirfd, const char *path, struct stat *st, int flags) {
	return fstatat(dirfd, path, st, flags);
}

/*
 * Glibc versioned stat wrappers (__xstat/__fxstat/__lxstat/__fxstatat)
 * used by binaries compiled against older glibc headers (python etc.).
 * The version argument is ignored; the routing matches stat/fstat.
 */
int __xstat(int ver, const char *path, struct stat *st) {
	const char *vp = hfs_view_path(path);
	if (vp != NULL) {
		if (hfs_lib_init() != 0) {
			errno = ENODEV;
			return -1;
		}
		return hfs_lib_stat(vp, st);
	}
	return real___xstat(ver, path, st);
}

/* Versioned lstat: HFS has no symlinks, same as stat. */
int __lxstat(int ver, const char *path, struct stat *st) {
	const char *vp = hfs_view_path(path);
	if (vp != NULL) {
		if (hfs_lib_init() != 0) {
			errno = ENODEV;
			return -1;
		}
		return hfs_lib_stat(vp, st);
	}
	return real___lxstat(ver, path, st);
}

/* Versioned fstat. */
int __fxstat(int ver, int fd, struct stat *st) {
	if (hfs_is_hfs_fd(fd)) {
		return hfs_lib_fstat(fd, st);
	}
	return real___fxstat(ver, fd, st);
}

/* Versioned fstatat. */
int __fxstatat(int ver, int dirfd, const char *path, struct stat *st, int flags) {
	const char *vp = hfs_view_path(path);
	if (dirfd == AT_FDCWD && vp != NULL) {
		if (hfs_lib_init() != 0) {
			errno = ENODEV;
			return -1;
		}
		return hfs_lib_stat(vp, st);
	}
	return real___fxstatat(ver, dirfd, path, st, flags);
}

/* Intercepted stat64(2): large-file variant of stat (same layout on x86-64). */
int stat64(const char *path, struct stat64 *st) {
	const char *vp = hfs_view_path(path);
	if (vp != NULL) {
		if (hfs_lib_init() != 0) {
			errno = ENODEV;
			return -1;
		}
		return hfs_lib_stat(vp, (struct stat *)st);
	}
	return real_stat64(path, st);
}

/* Intercepted lstat64(2): large-file variant of lstat. */
int lstat64(const char *path, struct stat64 *st) {
	const char *vp = hfs_view_path(path);
	if (vp != NULL) {
		if (hfs_lib_init() != 0) {
			errno = ENODEV;
			return -1;
		}
		return hfs_lib_stat(vp, (struct stat *)st);
	}
	return real_lstat64(path, st);
}

/* Intercepted fstat64(2): large-file variant of fstat. */
int fstat64(int fd, struct stat64 *st) {
	if (hfs_is_hfs_fd(fd)) {
		return hfs_lib_fstat(fd, (struct stat *)st);
	}
	return real_fstat64(fd, st);
}

/* Intercepted fstatat64(2): large-file variant of fstatat. */
int fstatat64(int dirfd, const char *path, struct stat64 *st, int flags) {
	const char *vp = hfs_view_path(path);
	if (dirfd == AT_FDCWD && vp != NULL) {
		if (hfs_lib_init() != 0) {
			errno = ENODEV;
			return -1;
		}
		return hfs_lib_stat(vp, (struct stat *)st);
	}
	return real_fstatat64(dirfd, path, st, flags);
}

/* Versioned large-file stat. */
int __xstat64(int ver, const char *path, struct stat64 *st) {
	const char *vp = hfs_view_path(path);
	if (vp != NULL) {
		if (hfs_lib_init() != 0) {
			errno = ENODEV;
			return -1;
		}
		return hfs_lib_stat(vp, (struct stat *)st);
	}
	return real___xstat64(ver, path, st);
}

/* Versioned large-file lstat. */
int __lxstat64(int ver, const char *path, struct stat64 *st) {
	const char *vp = hfs_view_path(path);
	if (vp != NULL) {
		if (hfs_lib_init() != 0) {
			errno = ENODEV;
			return -1;
		}
		return hfs_lib_stat(vp, (struct stat *)st);
	}
	return real___lxstat64(ver, path, st);
}

/* Versioned large-file fstat. */
int __fxstat64(int ver, int fd, struct stat64 *st) {
	if (hfs_is_hfs_fd(fd)) {
		return hfs_lib_fstat(fd, (struct stat *)st);
	}
	return real___fxstat64(ver, fd, st);
}

/* Versioned large-file fstatat. */
int __fxstatat64(int ver, int dirfd, const char *path, struct stat64 *st,
		 int flags) {
	const char *vp = hfs_view_path(path);
	if (dirfd == AT_FDCWD && vp != NULL) {
		if (hfs_lib_init() != 0) {
			errno = ENODEV;
			return -1;
		}
		return hfs_lib_stat(vp, (struct stat *)st);
	}
	return real___fxstatat64(ver, dirfd, path, st, flags);
}

/* Intercepted access(2): managed paths have no permission model. */
int access(const char *path, int mode) {
	const char *vp = hfs_view_path(path);
	if (vp != NULL) {
		if (hfs_lib_init() != 0) {
			errno = ENODEV;
			return -1;
		}
		return hfs_lib_access(vp, mode);
	}
	return real_access(path, mode);
}

/* Intercepted faccessat(2): AT_FDCWD with a managed path. */
int faccessat(int dirfd, const char *path, int mode, int flags) {
	const char *vp = hfs_view_path(path);
	if (dirfd == AT_FDCWD && vp != NULL) {
		if (hfs_lib_init() != 0) {
			errno = ENODEV;
			return -1;
		}
		return hfs_lib_access(vp, mode);
	}
	return real_faccessat(dirfd, path, mode, flags);
}

/*
 * Duplicate an HFS fd via F_DUPFD/F_DUPFD_CLOEXEC: reserve a kernel
 * fd through the real fcntl, then copy the table slot.
 */
static int hfs_fcntl_dup(int fd, int cmd, uintptr_t arg) {
	int nf = real_fcntl(fd, cmd, arg);
	if (nf >= 0 && hfs_lib_fd_copy(fd, nf) != 0) {
		real_close(nf);
		return -1;
	}
	return nf;
}

/*
 * Intercepted fcntl(2). HFS fd duplication (F_DUPFD/F_DUPFD_CLOEXEC)
 * reserves a kernel fd via the real fcntl, then copies the table slot;
 * other commands go through hfs_lib_fcntl.
 */
int fcntl(int fd, int cmd, ...) {
	va_list ap;
	uintptr_t arg = 0;

	va_start(ap, cmd);
	arg = va_arg(ap, uintptr_t);
	va_end(ap);

	if (hfs_is_hfs_fd(fd)) {
		if (cmd == F_DUPFD || cmd == F_DUPFD_CLOEXEC) {
			return hfs_fcntl_dup(fd, cmd, arg);
		}
		return hfs_lib_fcntl(fd, cmd, (long)arg);
	}
	return real_fcntl(fd, cmd, arg);
}

/*
 * Intercepted fcntl64(2): glibc large-file variant, used by python
 * for F_DUPFD_CLOEXEC among others.
 */
int fcntl64(int fd, int cmd, ...) {
	va_list ap;
	uintptr_t arg = 0;

	va_start(ap, cmd);
	arg = va_arg(ap, uintptr_t);
	va_end(ap);

	if (hfs_is_hfs_fd(fd)) {
		if (cmd == F_DUPFD || cmd == F_DUPFD_CLOEXEC) {
			return hfs_fcntl_dup(fd, cmd, arg);
		}
		return hfs_lib_fcntl(fd, cmd, (long)arg);
	}
	return real_fcntl64(fd, cmd, arg);
}

/*
 * Registered HFS directory handles. DIR is opaque to applications, so
 * readdir/closedir resolve a pointer by scanning this table.
 */
#define HFS_MAX_OPEN_DIRS	512

struct hfs_open_dir {
	struct hfs_dir *hdir;
};

static struct hfs_open_dir g_hfsDirs[HFS_MAX_OPEN_DIRS];
static int g_hfsDirCount;

/* Returns true when 'dirp' is one of our HFS directory handles. */
static bool hfs_is_hfs_dir(DIR *dirp) {
	int i;

	for (i = 0; i < g_hfsDirCount; i++) {
		if (g_hfsDirs[i].hdir == (struct hfs_dir *)dirp) {
			return true;
		}
	}
	return false;
}

/* TLS buffer converting an hfs_dirent into a struct dirent. */
static __thread struct dirent hfs_dir_result;

/*
 * Lazy real-symbol resolution. Interceptions may run before our
 * constructor (dependency library constructors call opendir/readdir
 * while the loader is still starting), so the real pointer can be
 * NULL; resolve it on first use instead of dereferencing a NULL.
 */
static DIR *(*hfs_real_opendir_get(void))(const char *) {
	if (real_opendir == NULL) {
		real_opendir = hfs_real_sym("opendir");
	}
	return real_opendir;
}

static struct dirent *(*hfs_real_readdir_get(void))(DIR *) {
	if (real_readdir == NULL) {
		real_readdir = hfs_real_sym("readdir");
	}
	return real_readdir;
}

static struct dirent64 *(*hfs_real_readdir64_get(void))(DIR *) {
	if (real_readdir64 == NULL) {
		real_readdir64 = hfs_real_sym("readdir64");
	}
	return real_readdir64;
}

static int (*hfs_real_closedir_get(void))(DIR *) {
	if (real_closedir == NULL) {
		real_closedir = hfs_real_sym("closedir");
	}
	return real_closedir;
}

static DIR *(*hfs_real_fdopendir_get(void))(int) {
	if (real_fdopendir == NULL) {
real_fdopendir = hfs_real_sym("fdopendir");
	real_readlink = hfs_real_sym("readlink");
	real_readlinkat = hfs_real_sym("readlinkat");
	}
	return real_fdopendir;
}

/* Shared readdir logic: HFS handles return the next snapshot entry. */
static struct dirent *hfs_readdir_impl(DIR *dirp,
				       struct dirent *(*real_fn)(DIR *)) {
	struct hfs_dirent *e;

	if (hfs_is_hfs_dir(dirp)) {
		e = hfs_lib_readdir((struct hfs_dir *)dirp);
		if (e == NULL) {
			return NULL;	/* end of directory */
		}
		memset(&hfs_dir_result, 0, sizeof(hfs_dir_result));
		hfs_dir_result.d_ino = (ino_t)e->inodeId;
		hfs_dir_result.d_type = e->d_type;
		strncpy(hfs_dir_result.d_name, e->name,
			sizeof(hfs_dir_result.d_name) - 1);
		return &hfs_dir_result;
	}
	return real_fn(dirp);
}

/* Intercepted opendir(2): managed paths open an HFS directory. */
DIR *opendir(const char *path) {
	struct hfs_dir *d;

	const char *vp = hfs_view_path(path);
	if (vp != NULL) {
		if (hfs_lib_init() != 0) {
			errno = ENODEV;
			return NULL;
		}
		d = hfs_lib_opendir(vp);
		if (d == NULL) {
			return NULL;
		}
		if (g_hfsDirCount >= HFS_MAX_OPEN_DIRS) {
			hfs_lib_closedir(d);
			errno = EMFILE;
			return NULL;
		}
		g_hfsDirs[g_hfsDirCount].hdir = d;
		g_hfsDirCount++;
		return (DIR *)d;
	}
	return hfs_real_opendir_get()(path);
}

/* Intercepted readdir(2). */
struct dirent *readdir(DIR *dirp) {
	return hfs_readdir_impl(dirp, hfs_real_readdir_get());
}

/* TLS buffer converting an hfs_dirent into a struct dirent64. */
static __thread struct dirent64 hfs_dir64_result;

/* Intercepted readdir64(2): large-file variant, same layout on x86-64. */
struct dirent64 *readdir64(DIR *dirp) {
	struct hfs_dirent *e;

	if (hfs_is_hfs_dir(dirp)) {
		e = hfs_lib_readdir((struct hfs_dir *)dirp);
		if (e == NULL) {
			return NULL;	/* end of directory */
		}
		memset(&hfs_dir64_result, 0, sizeof(hfs_dir64_result));
		hfs_dir64_result.d_ino = (ino_t)e->inodeId;
		hfs_dir64_result.d_type = e->d_type;
		strncpy(hfs_dir64_result.d_name, e->name,
			sizeof(hfs_dir64_result.d_name) - 1);
		return &hfs_dir64_result;
	}
	return hfs_real_readdir64_get()(dirp);
}

/* Intercepted closedir(2): unregister and release HFS handles. */
int closedir(DIR *dirp) {
	int i;

	if (hfs_is_hfs_dir(dirp)) {
		for (i = 0; i < g_hfsDirCount; i++) {
			if (g_hfsDirs[i].hdir == (struct hfs_dir *)dirp) {
				g_hfsDirs[i] = g_hfsDirs[g_hfsDirCount - 1];
				g_hfsDirCount--;
				break;
			}
		}
		return hfs_lib_closedir((struct hfs_dir *)dirp);
	}
	return hfs_real_closedir_get()(dirp);
}

/* Intercepted fdopendir(2): HFS fds have no directory backing store. */
DIR *fdopendir(int fd) {
	if (hfs_is_hfs_fd(fd)) {
		errno = ENOTSUP;
		return NULL;
	}
	return hfs_real_fdopendir_get()(fd);
}

/* Lazy real-symbol resolution: interceptions may run before our ctor
 * (dependency library constructors), so never trust a NULL pointer. */
static ssize_t (*hfs_real_readlink_get(void))(const char *, char *, size_t) {
	if (real_readlink == NULL) {
		real_readlink = hfs_real_sym("readlink");
	}
	return real_readlink;
}

static ssize_t (*hfs_real_readlinkat_get(void))(int, const char *, char *,
						size_t) {
	if (real_readlinkat == NULL) {
		real_readlinkat = hfs_real_sym("readlinkat");
	}
	return real_readlinkat;
}

/*
 * Shared readlink routing: HFS has no symlinks, so an existing managed
 * path yields EINVAL (not a link) and a missing one yields ENOENT.
 * Without this, aria/mariadbd sees kernel ENOENT for files that exist
 * in HFS and mis-handles its control files.
 */
static ssize_t hfs_readlink_impl(const char *path, char *buf, size_t bufsiz,
				 ssize_t (*real_fn)(const char *, char *,
						   size_t)) {
	const char *vp = hfs_view_path(path);
	struct stat st;

	if (vp != NULL) {
		if (hfs_lib_init() != 0) {
			errno = ENODEV;
			return -1;
		}
		if (hfs_lib_stat(vp, &st) != 0) {
			errno = ENOENT;
			return -1;
		}
		errno = EINVAL;	/* exists but is not a symlink */
		return -1;
	}
	return real_fn(path, buf, bufsiz);
}

/* Intercepted readlink(2). */
ssize_t readlink(const char *path, char *buf, size_t bufsiz) {
	return hfs_readlink_impl(path, buf, bufsiz, hfs_real_readlink_get());
}

/* Intercepted readlinkat(2): AT_FDCWD with a managed path. */
ssize_t readlinkat(int dirfd, const char *path, char *buf, size_t bufsiz) {
	const char *vp = hfs_view_path(path);
	struct stat st;

	if (dirfd == AT_FDCWD && vp != NULL) {
		if (hfs_lib_init() != 0) {
			errno = ENODEV;
			return -1;
		}
		if (hfs_lib_stat(vp, &st) != 0) {
			errno = ENOENT;
			return -1;
		}
		errno = EINVAL;	/* exists but is not a symlink */
		return -1;
	}
	return hfs_real_readlinkat_get()(dirfd, path, buf, bufsiz);
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
	real___read = hfs_real_sym("__read");
	real___write = hfs_real_sym("__write");
	real_readv = hfs_real_sym("readv");
	real_writev = hfs_real_sym("writev");
	real_preadv = hfs_real_sym("preadv");
	real_pwritev = hfs_real_sym("pwritev");
	real_preadv2 = hfs_real_sym("preadv2");
	real_pwritev2 = hfs_real_sym("pwritev2");
	real_pread = hfs_real_sym("pread");
	real_pwrite = hfs_real_sym("pwrite");
	real_pread64 = hfs_real_sym("pread64");
	real_pwrite64 = hfs_real_sym("pwrite64");
	real_lseek = hfs_real_sym("lseek");
	real_lseek64 = hfs_real_sym("lseek64");
	real_dup = hfs_real_sym("dup");
	real_dup2 = hfs_real_sym("dup2");
	real_dup3 = hfs_real_sym("dup3");
	real_fsync = hfs_real_sym("fsync");
	real_fdatasync = hfs_real_sym("fdatasync");
	real_sync_file_range = hfs_real_sym("sync_file_range");
	real_ftruncate = hfs_real_sym("ftruncate");
	real_ftruncate64 = hfs_real_sym("ftruncate64");
	real_fallocate = hfs_real_sym("fallocate");
	real_fallocate64 = hfs_real_sym("fallocate64");
	real_posix_fallocate = hfs_real_sym("posix_fallocate");
	real_unlink = hfs_real_sym("unlink");
	real_unlinkat = hfs_real_sym("unlinkat");
	real_mkdir = hfs_real_sym("mkdir");
	real_rmdir = hfs_real_sym("rmdir");
	real_rename = hfs_real_sym("rename");
	real_renameat = hfs_real_sym("renameat");
	real_renameat2 = hfs_real_sym("renameat2");
	real_chdir = hfs_real_sym("chdir");
	real_fchdir = hfs_real_sym("fchdir");
	real_stat = hfs_real_sym("stat");
	real_lstat = hfs_real_sym("lstat");
	real_fstat = hfs_real_sym("fstat");
	real_fstatat = hfs_real_sym("fstatat");
	real_access = hfs_real_sym("access");
	real_faccessat = hfs_real_sym("faccessat");
	real_fcntl = hfs_real_sym("fcntl");
	real_fcntl64 = hfs_real_sym("fcntl64");
	real_opendir = hfs_real_sym("opendir");
	real_readdir = hfs_real_sym("readdir");
	real_readdir64 = hfs_real_sym("readdir64");
	real_closedir = hfs_real_sym("closedir");
	real_fdopendir = hfs_real_sym("fdopendir");
	real___xstat = hfs_real_sym("__xstat");
	real___lxstat = hfs_real_sym("__lxstat");
	real___fxstat = hfs_real_sym("__fxstat");
	real___fxstatat = hfs_real_sym("__fxstatat");
	real_stat64 = hfs_real_sym("stat64");
	real_lstat64 = hfs_real_sym("lstat64");
	real_fstat64 = hfs_real_sym("fstat64");
	real_fstatat64 = hfs_real_sym("fstatat64");
	real___xstat64 = hfs_real_sym("__xstat64");
	real___lxstat64 = hfs_real_sym("__lxstat64");
	real___fxstat64 = hfs_real_sym("__fxstat64");
	real___fxstatat64 = hfs_real_sym("__fxstatat64");

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