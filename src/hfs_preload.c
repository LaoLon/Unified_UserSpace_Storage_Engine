/*
 * hfs_preload.c: LD_PRELOAD interception layer.
 * Resolves the real libc symbols via dlsym(RTLD_NEXT) and redirects
 * calls whose path falls under the HFS mount prefix to libhfs.
 */
#define _GNU_SOURCE

#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/stat.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/types.h>
#include <sys/vfs.h>
#include <unistd.h>

#include "hfs_lib.h"
#include "hfs_log.h"

/* Real libc symbols captured once at load time. */
static int (*real_open)(const char *path, int flags, ...);
static int (*real_open64)(const char *path, int flags, ...);
static int (*real_openat)(int dirfd, const char *path, int flags, ...);
static int (*real_openat64)(int dirfd, const char *path, int flags, ...);
static int (*real___open_2)(const char *path, int flags);
static int (*real___open64_2)(const char *path, int flags);
static int (*real___openat_2)(int dirfd, const char *path, int flags);
static int (*real___openat64_2)(int dirfd, const char *path, int flags);
static int (*real_close)(int fd);
static ssize_t (*real_read)(int fd, void *buf, size_t count);
static size_t (*real_fread)(void *ptr, size_t size, size_t nmemb,
				   FILE *stream);
static size_t (*real_fread_unlocked)(void *ptr, size_t size, size_t nmemb,
					     FILE *stream);
static int (*real_fileno)(FILE *stream);
static int (*real_feof)(FILE *stream);
static int (*real_ferror)(FILE *stream);
static void (*real_clearerr)(FILE *stream);
static int (*real_fclose)(FILE *stream);
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
static int (*real_mkdirat)(int dirfd, const char *path, mode_t mode);
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
static int (*real_statx)(int dirfd, const char *path, int flags,
			 unsigned int mask, struct statx *stx);
static int (*real_statfs)(const char *path, struct statfs *st);
static int (*real_fstatfs)(int fd, struct statfs *st);
static int (*real_statfs64)(const char *path, struct statfs64 *st);
static int (*real_fstatfs64)(int fd, struct statfs64 *st);
static int (*real_statvfs)(const char *path, struct statvfs *st);
static int (*real_fstatvfs)(int fd, struct statvfs *st);
static int (*real_statvfs64)(const char *path, struct statvfs64 *st);
static int (*real_fstatvfs64)(int fd, struct statvfs64 *st);
static int (*real_chmod)(const char *path, mode_t mode);
static int (*real_fchmod)(int fd, mode_t mode);
static int (*real_fchmodat)(int dirfd, const char *path, mode_t mode,
			    int flags);
static int (*real_chown)(const char *path, uid_t uid, gid_t gid);
static int (*real_lchown)(const char *path, uid_t uid, gid_t gid);
static int (*real_fchown)(int fd, uid_t uid, gid_t gid);
static int (*real_fchownat)(int dirfd, const char *path, uid_t uid,
			    gid_t gid, int flags);
static int (*real_utimensat)(int dirfd, const char *path,
			    const struct timespec times[2], int flags);
static int (*real_futimens)(int fd, const struct timespec times[2]);
static int (*real_posix_fadvise)(int fd, off_t offset, off_t len, int advice);
static int (*real_posix_fadvise64)(int fd, off64_t offset, off64_t len,
				   int advice);
static void *(*real_mmap)(void *addr, size_t length, int prot, int flags,
			 int fd, off_t offset);
static void *(*real_mmap64)(void *addr, size_t length, int prot, int flags,
			    int fd, off64_t offset);
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
static pthread_rwlock_t g_hfs_cwd_lock = PTHREAD_RWLOCK_INITIALIZER;

/* Track stdio EOF/error state for HFS streams without touching glibc FILE. */
#define HFS_MAX_STDIO_STREAMS 256
#define HFS_STDIO_EOF 0x1
#define HFS_STDIO_ERROR 0x2
struct hfs_stdio_state {
	FILE *stream;
	unsigned int flags;
};
static struct hfs_stdio_state g_hfs_stdio_states[HFS_MAX_STDIO_STREAMS];
static pthread_mutex_t g_hfs_stdio_lock = PTHREAD_MUTEX_INITIALIZER;

static int (*real_chdir)(const char *path);
static int (*real_fchdir)(int fd);
static char *(*real_getcwd)(char *buf, size_t size);
static char *(*real_get_current_dir_name)(void);
static char *(*real_realpath)(const char *path, char *resolved);
static char *(*real___realpath_chk)(const char *path, char *resolved,
					    size_t resolvedSize);

/*
 * Set when this process inherited an initialized HFS environment from
 * its parent (fork+exec). Such children must NOT re-initialize SPDK:
 * a second DPDK primary conflicts with the parent and hangs. They run
 * in pass-through mode instead.
 */
static bool hfs_inherited;

/*
 * Return true when the path equals or is below the HFS mount prefix.
 * The exact mount point is managed as the HFS root directory.
 */
static bool hfs_path_managed(const char *path) {
	size_t mountLen;

	if (hfs_prefix == NULL || path == NULL || hfs_inherited ||
	    hfs_lib_is_fork_child()) {
		return false;
	}
	mountLen = hfs_prefix_len;
	while (mountLen > 1 && hfs_prefix[mountLen - 1] == '/') {
		mountLen--;
	}
	if (strncmp(path, hfs_prefix, mountLen) != 0) {
		return false;
	}
	return path[mountLen] == '\0' || path[mountLen] == '/';
}

/* Return the prefix-stripped portion of a validated managed path. */
static const char *hfs_inner_path(const char *path) {
	size_t mountLen = hfs_prefix_len;

	while (mountLen > 1 && hfs_prefix[mountLen - 1] == '/') {
		mountLen--;
	}
	if (path[mountLen] == '\0' ||
	    (path[mountLen] == '/' && path[mountLen + 1] == '\0')) {
		return "/";
	}
	if (path[mountLen] == '/') {
		mountLen++;
	}
	return path + mountLen;
}

/* Build a full managed path from the process HFS cwd and a user path. */
static bool hfs_full_path_buf(const char *path, char *buf, size_t size) {
	bool active;

	if (hfs_lib_is_fork_child() || path == NULL || path[0] == '\0' ||
	    buf == NULL || size == 0) {
		return false;
	}
	if (path[0] == '/') {
		if (!hfs_path_managed(path) ||
		    snprintf(buf, size, "%s", path) >= (int)size) {
			return false;
		}
		return true;
	}
	pthread_rwlock_rdlock(&g_hfs_cwd_lock);
	active = g_hfs_cwd_active;
	if (active && snprintf(buf, size, "%s/%s", g_hfs_cwd, path)
	    >= (int)size) {
		active = false;
	}
	pthread_rwlock_unlock(&g_hfs_cwd_lock);
	return active && hfs_path_managed(buf);
}

/* Build a full managed path from a prefix-stripped directory path. */
static bool hfs_inner_to_full_path(const char *inner, char *buf, size_t size) {
	size_t prefixLen;
	const char *component;

	if (hfs_prefix == NULL || inner == NULL || buf == NULL || size == 0) {
		return false;
	}
	prefixLen = hfs_prefix_len;
	while (prefixLen > 1 && hfs_prefix[prefixLen - 1] == '/') {
		prefixLen--;
	}
	component = inner;
	while (*component == '/') {
		component++;
	}
	if (*component == '\0') {
		return snprintf(buf, size, "%.*s", (int)prefixLen, hfs_prefix)
		       < (int)size;
	}
	return snprintf(buf, size, "%.*s/%s", (int)prefixLen, hfs_prefix,
			component) < (int)size;
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
	char fullPath[PATH_MAX];

	if (path == NULL) {
		return NULL;
	}
	if (path[0] == '/') {
		if (!hfs_path_managed(path)) {
			return NULL;
		}
		return hfs_inner_path(path);
	}
	if (!hfs_full_path_buf(path, fullPath, sizeof(fullPath))) {
		return NULL;
	}
	if (snprintf(buf, size, "%s", fullPath) >= (int)size) {
		return NULL;
	}
	return hfs_inner_path(buf);
}

/* TLS convenience wrapper of hfs_view_path_buf. */
static const char *hfs_view_path(const char *path) {
	static __thread char buf[PATH_MAX];

	return hfs_view_path_buf(path, buf, sizeof(buf));
}

/* Resolve a path relative to an HFS directory fd into an inner HFS path. */
static int hfs_view_at_path(int dirfd, const char *path, char *buf,
				    size_t size) {
	const char *vp;
	char dirPath[PATH_MAX];

	if (path == NULL || buf == NULL || size == 0) {
		errno = EFAULT;
		return -1;
	}
	if (path[0] == '/') {
		/* Absolute paths ignore dirfd, even when dirfd is not AT_FDCWD. */
		vp = hfs_view_path_buf(path, buf, size);
		if (vp == NULL) {
			return 0;
		}
		if (snprintf(buf, size, "%s", vp) >= (int)size) {
			errno = ENAMETOOLONG;
			return -1;
		}
		return 1;
	}
	if (dirfd == AT_FDCWD) {
		/* Always materialize the translated path for callers that reuse buf. */
		vp = hfs_view_path(path);
		if (vp == NULL) {
			return 0;
		}
		if (snprintf(buf, size, "%s", vp) >= (int)size) {
			errno = ENAMETOOLONG;
			return -1;
		}
		return 1;
	}
	if (!hfs_is_hfs_fd(dirfd)) {
		return 0;
	}
	if (path[0] == '\0') {
		buf[0] = '\0';
		return 1;
	}
	if (hfs_lib_get_dir_path(dirfd, dirPath, sizeof(dirPath)) != 0) {
		return -1;
	}
	if (snprintf(buf, size, "%s/%s", dirPath, path) >= (int)size) {
		errno = ENAMETOOLONG;
		return -1;
	}
	return 1;
}

/* Resolve a libc symbol once via dlsym(RTLD_NEXT). */
static void *hfs_real_sym(const char *name) {
	void *sym = dlsym(RTLD_NEXT, name);
	if (sym == NULL) {
		fprintf(stderr, "libhfs: dlsym(%s) failed: %s\n", name, dlerror());
	}
	return sym;
}

/* Return the tracked state slot for a stream, creating it when requested. */
static struct hfs_stdio_state *hfs_stdio_state_get(FILE *stream, bool create) {
	struct hfs_stdio_state *freeState = NULL;
	int i;

	for (i = 0; i < HFS_MAX_STDIO_STREAMS; i++) {
		if (g_hfs_stdio_states[i].stream == stream) {
			return &g_hfs_stdio_states[i];
		}
		if (freeState == NULL && g_hfs_stdio_states[i].stream == NULL) {
			freeState = &g_hfs_stdio_states[i];
		}
	}
	if (create && freeState != NULL) {
		freeState->stream = stream;
		freeState->flags = 0;
	}
	return freeState;
}

/* Update the emulated EOF/error flags for an HFS stdio stream. */
static void hfs_stdio_mark(FILE *stream, unsigned int flags) {
	struct hfs_stdio_state *state;

	pthread_mutex_lock(&g_hfs_stdio_lock);
	state = hfs_stdio_state_get(stream, true);
	if (state != NULL) {
		state->flags |= flags;
	}
	pthread_mutex_unlock(&g_hfs_stdio_lock);
}

/* Return one emulated stdio flag while keeping stream lookup synchronized. */
static int hfs_stdio_has(FILE *stream, unsigned int flag) {
	struct hfs_stdio_state *state;
	int result = 0;

	pthread_mutex_lock(&g_hfs_stdio_lock);
	state = hfs_stdio_state_get(stream, false);
	if (state != NULL) {
		result = (state->flags & flag) != 0;
	}
	pthread_mutex_unlock(&g_hfs_stdio_lock);
	return result;
}

/* Clear the emulated stdio flags, matching clearerr(3). */
static void hfs_stdio_clear(FILE *stream) {
	struct hfs_stdio_state *state;

	pthread_mutex_lock(&g_hfs_stdio_lock);
	state = hfs_stdio_state_get(stream, false);
	if (state != NULL) {
		state->flags = 0;
	}
	pthread_mutex_unlock(&g_hfs_stdio_lock);
}

/* Release a stream state slot after fclose(3) closes the underlying fd. */
static void hfs_stdio_forget(FILE *stream) {
	struct hfs_stdio_state *state;

	pthread_mutex_lock(&g_hfs_stdio_lock);
	state = hfs_stdio_state_get(stream, false);
	if (state != NULL) {
		state->stream = NULL;
		state->flags = 0;
	}
	pthread_mutex_unlock(&g_hfs_stdio_lock);
}

/* Resolve stdio symbols lazily for constructors that run before our ctor. */
static int hfs_real_fileno_call(FILE *stream) {
	if (real_fileno == NULL) {
		real_fileno = hfs_real_sym("fileno");
	}
	return real_fileno != NULL ? real_fileno(stream) : -1;
}

/* Resolve fread lazily before forwarding non-HFS streams. */
static size_t hfs_real_fread_call(void *ptr, size_t size, size_t nmemb,
					  FILE *stream) {
	if (real_fread == NULL) {
		real_fread = hfs_real_sym("fread");
	}
	return real_fread != NULL ? real_fread(ptr, size, nmemb, stream) : 0;
}

/* Resolve fread_unlocked lazily before forwarding regular streams. */
static size_t hfs_real_fread_unlocked_call(void *ptr, size_t size,
						   size_t nmemb, FILE *stream) {
	if (real_fread_unlocked == NULL) {
		real_fread_unlocked = hfs_real_sym("fread_unlocked");
	}
	return real_fread_unlocked != NULL ?
		real_fread_unlocked(ptr, size, nmemb, stream) : 0;
}

/* Resolve the remaining stdio status functions on their first use. */
static int hfs_real_feof_call(FILE *stream) {
	if (real_feof == NULL) {
		real_feof = hfs_real_sym("feof");
	}
	return real_feof != NULL ? real_feof(stream) : 0;
}

static int hfs_real_ferror_call(FILE *stream) {
	if (real_ferror == NULL) {
		real_ferror = hfs_real_sym("ferror");
	}
	return real_ferror != NULL ? real_ferror(stream) : 0;
}

static void hfs_real_clearerr_call(FILE *stream) {
	if (real_clearerr == NULL) {
		real_clearerr = hfs_real_sym("clearerr");
	}
	if (real_clearerr != NULL) {
		real_clearerr(stream);
	}
}

static int hfs_real_fclose_call(FILE *stream) {
	if (real_fclose == NULL) {
		real_fclose = hfs_real_sym("fclose");
	}
	return real_fclose != NULL ? real_fclose(stream) : EOF;
}

/* Resolve statfs64 lazily because dependency constructors can call it first. */
static int hfs_real_statfs64_call(const char *path, struct statfs64 *st) {
	if (real_statfs64 == NULL) {
		real_statfs64 = hfs_real_sym("statfs64");
	}
	if (real_statfs64 != NULL) {
		return real_statfs64(path, st);
	}
	if (real_statfs == NULL) {
		real_statfs = hfs_real_sym("statfs");
	}
	if (real_statfs == NULL) {
		errno = ENOSYS;
		return -1;
	}
	return real_statfs(path, (struct statfs *)st);
}

/* Resolve fstatfs64 lazily and fall back to the native-width ABI. */
static int hfs_real_fstatfs64_call(int fd, struct statfs64 *st) {
	if (real_fstatfs64 == NULL) {
		real_fstatfs64 = hfs_real_sym("fstatfs64");
	}
	if (real_fstatfs64 != NULL) {
		return real_fstatfs64(fd, st);
	}
	if (real_fstatfs == NULL) {
		real_fstatfs = hfs_real_sym("fstatfs");
	}
	if (real_fstatfs == NULL) {
		errno = ENOSYS;
		return -1;
	}
	return real_fstatfs(fd, (struct statfs *)st);
}

/* Resolve statvfs64 lazily because dependency constructors can call it first. */
static int hfs_real_statvfs64_call(const char *path, struct statvfs64 *st) {
	if (real_statvfs64 == NULL) {
		real_statvfs64 = hfs_real_sym("statvfs64");
	}
	if (real_statvfs64 != NULL) {
		return real_statvfs64(path, st);
	}
	if (real_statvfs == NULL) {
		real_statvfs = hfs_real_sym("statvfs");
	}
	if (real_statvfs == NULL) {
		errno = ENOSYS;
		return -1;
	}
	return real_statvfs(path, (struct statvfs *)st);
}

/* Resolve fstatvfs64 lazily and fall back to the native-width ABI. */
static int hfs_real_fstatvfs64_call(int fd, struct statvfs64 *st) {
	if (real_fstatvfs64 == NULL) {
		real_fstatvfs64 = hfs_real_sym("fstatvfs64");
	}
	if (real_fstatvfs64 != NULL) {
		return real_fstatvfs64(fd, st);
	}
	if (real_fstatvfs == NULL) {
		real_fstatvfs = hfs_real_sym("fstatvfs");
	}
	if (real_fstatvfs == NULL) {
		errno = ENOSYS;
		return -1;
	}
	return real_fstatvfs(fd, (struct statvfs *)st);
}

/* Resolve access lazily because dependency constructors can call it first. */
static int hfs_real_access_call(const char *path, int mode) {
	if (real_access == NULL) {
		real_access = hfs_real_sym("access");
	}
	if (real_access == NULL) {
		errno = ENOSYS;
		return -1;
	}
	return real_access(path, mode);
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

/* Return whether open(2) is allowed to consume a variadic mode argument. */
static bool hfs_open_has_mode(int flags) {
	bool hasMode = (flags & O_CREAT) != 0;

#ifdef O_TMPFILE
	hasMode = hasMode || (flags & O_TMPFILE) != 0;
#endif
	return hasMode;
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

	if (hfs_open_has_mode(flags)) {
		va_start(ap, flags);
		mode = va_arg(ap, mode_t);
		va_end(ap);
	}

	rc = hfs_intercept_open(path, flags, mode);
	if (rc != -2) {
		return rc;
	}
	return hfs_open_has_mode(flags) ? real_open(path, flags, mode) :
					      real_open(path, flags);
}

/*
 * Intercepted open64(2) (large-file variant, used by dash/busybox).
 * Same routing as open(2).
 */
int open64(const char *path, int flags, ...) {
	va_list ap;
	mode_t mode = 0;
	int rc;

	if (hfs_open_has_mode(flags)) {
		va_start(ap, flags);
		mode = va_arg(ap, mode_t);
		va_end(ap);
	}

	rc = hfs_intercept_open(path, flags, mode);
	if (rc != -2) {
		return rc;
	}
	return hfs_open_has_mode(flags) ? real_open64(path, flags, mode) :
						 real_open64(path, flags);
}

/* Intercept fortified open variants emitted when no mode argument is used. */
int __open_2(const char *path, int flags) {
	int rc = hfs_intercept_open(path, flags, 0);

	if (rc != -2) {
		return rc;
	}
	return real___open_2 != NULL ? real___open_2(path, flags) :
					 real_open(path, flags);
}

/* Intercept the large-file fortified open variant. */
int __open64_2(const char *path, int flags) {
	int rc = hfs_intercept_open(path, flags, 0);

	if (rc != -2) {
		return rc;
	}
	return real___open64_2 != NULL ? real___open64_2(path, flags) :
					 real_open64(path, flags);
}

/* Intercept fortified openat for HFS directory-relative paths. */
int __openat_2(int dirfd, const char *path, int flags) {
	int rc = -2;

	if (dirfd == AT_FDCWD) {
		rc = hfs_intercept_open(path, flags, 0);
	} else if (path != NULL && path[0] == '/' &&
		   hfs_view_path(path) != NULL) {
		/* Absolute paths ignore dirfd under openat(2) semantics. */
		rc = hfs_intercept_open(path, flags, 0);
	} else if (hfs_is_hfs_fd(dirfd)) {
		if (hfs_lib_init() != 0) {
			errno = ENODEV;
			return -1;
		}
		rc = hfs_lib_openat(dirfd, path, flags, 0);
	}
	if (rc != -2) {
		return rc;
	}
	return real___openat_2 != NULL ? real___openat_2(dirfd, path, flags) :
					 real_openat(dirfd, path, flags);
}

/* Intercept the large-file fortified openat variant. */
int __openat64_2(int dirfd, const char *path, int flags) {
	int rc = -2;

	if (dirfd == AT_FDCWD) {
		rc = hfs_intercept_open(path, flags, 0);
	} else if (path != NULL && path[0] == '/' &&
		   hfs_view_path(path) != NULL) {
		/* Absolute paths ignore dirfd under openat(2) semantics. */
		rc = hfs_intercept_open(path, flags, 0);
	} else if (hfs_is_hfs_fd(dirfd)) {
		if (hfs_lib_init() != 0) {
			errno = ENODEV;
			return -1;
		}
		rc = hfs_lib_openat(dirfd, path, flags, 0);
	}
	if (rc != -2) {
		return rc;
	}
	return real___openat64_2 != NULL ? real___openat64_2(dirfd, path, flags) :
					 real_openat64(dirfd, path, flags);
}

/*
 * Intercepted openat(2). Absolute managed paths ignore dirfd; relative
 * paths are routed only when dirfd identifies an HFS directory.
 */
int openat(int dirfd, const char *path, int flags, ...) {
	va_list ap;
	mode_t mode = 0;

	if (hfs_open_has_mode(flags)) {
		va_start(ap, flags);
		mode = va_arg(ap, mode_t);
		va_end(ap);
	}
	if (dirfd == AT_FDCWD || (path != NULL && path[0] == '/' &&
				       hfs_view_path(path) != NULL)) {
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
	return hfs_open_has_mode(flags) ? real_openat(dirfd, path, flags, mode) :
						 real_openat(dirfd, path, flags);
}

/*
 * Intercepted openat64(2) (large-file variant). Same routing as openat.
 */
int openat64(int dirfd, const char *path, int flags, ...) {
	va_list ap;
	mode_t mode = 0;

	if (hfs_open_has_mode(flags)) {
		va_start(ap, flags);
		mode = va_arg(ap, mode_t);
		va_end(ap);
	}
	if (dirfd == AT_FDCWD || (path != NULL && path[0] == '/' &&
				       hfs_view_path(path) != NULL)) {
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
	return hfs_open_has_mode(flags) ? real_openat64(dirfd, path, flags, mode) :
						   real_openat64(dirfd, path, flags);
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

/* Read an HFS stdio stream while keeping the public and unlocked paths equal. */
static size_t hfsReadStream(void *ptr, size_t size, size_t nmemb,
				    FILE *stream, bool unlocked) {
	int fd;
	ssize_t n;
	size_t total;

	if (size == 0 || nmemb == 0) {
		return 0;
	}
	if (nmemb > SIZE_MAX / size) {
		errno = EOVERFLOW;
		return 0;
	}
	fd = hfs_real_fileno_call(stream);
	if (!hfs_is_hfs_fd(fd)) {
		return unlocked ? hfs_real_fread_unlocked_call(ptr, size, nmemb, stream) :
			hfs_real_fread_call(ptr, size, nmemb, stream);
	}
	total = size * nmemb;
	n = hfs_lib_read(fd, ptr, total);
	if (n < 0) {
		hfs_stdio_mark(stream, HFS_STDIO_ERROR);
		return 0;
	}
	if ((size_t)n < total) {
		hfs_stdio_mark(stream, HFS_STDIO_EOF);
	}
	return (size_t)n / size;
}

/* Intercept stdio reads because glibc may issue its internal read syscall. */
size_t fread(void *ptr, size_t size, size_t nmemb, FILE *stream) {
	return hfsReadStream(ptr, size, nmemb, stream, false);
}

/* Intercept RocksDB's unlocked stdio read path for HFS streams. */
size_t fread_unlocked(void *ptr, size_t size, size_t nmemb, FILE *stream) {
	return hfsReadStream(ptr, size, nmemb, stream, true);
}

/* Intercept feof(3) for HFS streams whose reads bypass glibc FILE buffers. */
int feof(FILE *stream) {
	int fd = hfs_real_fileno_call(stream);

	if (hfs_is_hfs_fd(fd)) {
		return hfs_stdio_has(stream, HFS_STDIO_EOF);
	}
	return hfs_real_feof_call(stream);
}

/* Intercept ferror(3) for HFS streams. */
int ferror(FILE *stream) {
	int fd = hfs_real_fileno_call(stream);

	if (hfs_is_hfs_fd(fd)) {
		return hfs_stdio_has(stream, HFS_STDIO_ERROR);
	}
	return hfs_real_ferror_call(stream);
}

/* Intercept clearerr(3) and reset the emulated HFS stream state. */
void clearerr(FILE *stream) {
	int fd = hfs_real_fileno_call(stream);

	if (hfs_is_hfs_fd(fd)) {
		hfs_stdio_clear(stream);
		return;
	}
	hfs_real_clearerr_call(stream);
}

/* Intercept fclose(3) so recycled FILE pointers cannot reuse stale state. */
int fclose(FILE *stream) {
	int fd = hfs_real_fileno_call(stream);
	bool managed = hfs_is_hfs_fd(fd);
	int rc;

	/* Detach first so libc's internal close cannot leave a stale HFS slot. */
	if (managed && hfs_lib_release_fd(fd) != 0) {
		hfs_stdio_forget(stream);
		return -1;
	}
	rc = hfs_real_fclose_call(stream);
	if (managed) {
		hfs_stdio_forget(stream);
	}
	return rc;
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

/* Intercepted readv(2): HFS fds use bounded request-local DMA buffers. */
ssize_t readv(int fd, const struct iovec *iov, int iovcnt) {
	if (!hfs_is_hfs_fd(fd)) {
		return real_readv(fd, iov, iovcnt);
	}
	return hfs_lib_readv(fd, iov, iovcnt);
}

/* Intercepted writev(2): HFS fds use bounded request-local DMA buffers. */
ssize_t writev(int fd, const struct iovec *iov, int iovcnt) {
	if (!hfs_is_hfs_fd(fd)) {
		return real_writev(fd, iov, iovcnt);
	}
	return hfs_lib_writev(fd, iov, iovcnt);
}

/* Intercepted preadv(2): positioned vector reads stay request-local. */
ssize_t preadv(int fd, const struct iovec *iov, int iovcnt, off_t offset) {
	if (!hfs_is_hfs_fd(fd)) {
		return real_preadv(fd, iov, iovcnt, offset);
	}
	return hfs_lib_preadv(fd, iov, iovcnt, offset);
}

/* Intercepted pwritev(2): positioned vector writes stay request-local. */
ssize_t pwritev(int fd, const struct iovec *iov, int iovcnt, off_t offset) {
	if (!hfs_is_hfs_fd(fd)) {
		return real_pwritev(fd, iov, iovcnt, offset);
	}
	return hfs_lib_pwritev(fd, iov, iovcnt, offset);
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
	return hfs_lib_preadv(fd, iov, iovcnt, offset);
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
	return hfs_lib_pwritev(fd, iov, iovcnt, offset);
}

/* Intercepted glibc large-file pwritev2 alias used by Python and databases. */
ssize_t pwritev64v2(int fd, const struct iovec *iov, int iovcnt,
		    off64_t offset, int flags) {
	return pwritev2(fd, iov, iovcnt, (off_t)offset, flags);
}

/* Intercepted glibc large-file preadv2 alias used by Python and databases. */
ssize_t preadv64v2(int fd, const struct iovec *iov, int iovcnt,
		   off64_t offset, int flags) {
	return preadv2(fd, iov, iovcnt, (off_t)offset, flags);
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

/* Intercepted fallocate(2): HFS reserves storage and supports KEEP_SIZE/punch. */
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

/* Intercept posix_fadvise(3); accepted hints are validated no-ops. */
int posix_fadvise(int fd, off_t offset, off_t len, int advice) {
	if (hfs_is_hfs_fd(fd)) {
		return hfs_lib_posix_fadvise(fd, offset, len, advice);
	}
	return real_posix_fadvise(fd, offset, len, advice);
}

/* Intercept the large-file posix_fadvise64 alias. */
int posix_fadvise64(int fd, off64_t offset, off64_t len, int advice) {
	if (hfs_is_hfs_fd(fd)) {
		return hfs_lib_posix_fadvise(fd, (off_t)offset, (off_t)len, advice);
	}
	return real_posix_fadvise64(fd, offset, len, advice);
}

/* Intercept mmap(2): HFS fds intentionally expose no memory mapping ABI. */
void *mmap(void *addr, size_t length, int prot, int flags, int fd,
	   off_t offset) {
	if (hfs_is_hfs_fd(fd)) {
		errno = ENODEV;
		return MAP_FAILED;
	}
	return real_mmap(addr, length, prot, flags, fd, offset);
}

/* Intercept mmap64(2) with the same explicit HFS limitation. */
void *mmap64(void *addr, size_t length, int prot, int flags, int fd,
	     off64_t offset) {
	if (hfs_is_hfs_fd(fd)) {
		errno = ENODEV;
		return MAP_FAILED;
	}
	return real_mmap64(addr, length, prot, flags, fd, offset);
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

/* Intercepted unlinkat(2), including paths relative to HFS directory fds. */
int unlinkat(int dirfd, const char *path, int flags) {
	char pathBuf[PATH_MAX];
	int managed = hfs_view_at_path(dirfd, path, pathBuf, sizeof(pathBuf));

	if (managed < 0) {
		return -1;
	}
	if (managed > 0) {
		if (hfs_lib_init() != 0) {
			errno = ENODEV;
			return -1;
		}
		if (flags & ~(AT_REMOVEDIR)) {
			errno = EINVAL;
			return -1;
		}
		if (flags & AT_REMOVEDIR) {
			return hfs_lib_rmdir(pathBuf);
		}
		return hfs_lib_unlink(pathBuf);
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

/* Intercepted mkdirat(2), including paths relative to HFS directory fds. */
int mkdirat(int dirfd, const char *path, mode_t mode) {
	char pathBuf[PATH_MAX];
	int managed = hfs_view_at_path(dirfd, path, pathBuf, sizeof(pathBuf));

	if (managed < 0) {
		return -1;
	}
	if (managed > 0) {
		if (hfs_lib_init() != 0) {
			errno = ENODEV;
			return -1;
		}
		return hfs_lib_mkdir(pathBuf, mode);
	}
	return real_mkdirat(dirfd, path, mode);
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

/* Intercepted renameat(2), including HFS directory-fd relative paths. */
int renameat(int olddirfd, const char *oldpath,
	     int newdirfd, const char *newpath) {
	char oldBuf[PATH_MAX];
	char newBuf[PATH_MAX];
	int oldState = hfs_view_at_path(olddirfd, oldpath, oldBuf,
					       sizeof(oldBuf));
	int newState = hfs_view_at_path(newdirfd, newpath, newBuf,
					       sizeof(newBuf));
	bool oldManaged = oldState > 0;
	bool newManaged = newState > 0;

	if (oldState < 0 || newState < 0) {
		return -1;
	}

	if (oldManaged || newManaged) {
		if (oldManaged != newManaged) {
			errno = EXDEV;
			return -1;
		}
		if (hfs_lib_init() != 0) {
			errno = ENODEV;
			return -1;
		}
		return hfs_lib_rename(oldBuf, newBuf);
	}
	return real_renameat(olddirfd, oldpath, newdirfd, newpath);
}

/* Intercepted renameat2(2): only flags=0 (plain rename) is supported. */
int renameat2(int olddirfd, const char *oldpath,
	      int newdirfd, const char *newpath, unsigned int flags) {
	char oldBuf[PATH_MAX];
	char newBuf[PATH_MAX];
	int oldState = hfs_view_at_path(olddirfd, oldpath, oldBuf,
					       sizeof(oldBuf));
	int newState = hfs_view_at_path(newdirfd, newpath, newBuf,
					       sizeof(newBuf));
	bool oldManaged = oldState > 0;
	bool newManaged = newState > 0;

	if (oldState < 0 || newState < 0) {
		return -1;
	}

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
		return hfs_lib_rename(oldBuf, newBuf);
	}
	return real_renameat2(olddirfd, oldpath, newdirfd, newpath, flags);
}

/* Intercepted chdir(2): managed paths switch the tracked HFS cwd. */
int chdir(const char *path) {
	char fullPath[PATH_MAX];
	const char *vp;
	struct stat st;

	if (hfs_full_path_buf(path, fullPath, sizeof(fullPath))) {
		vp = hfs_inner_path(fullPath);
		if (hfs_lib_init() != 0) {
			errno = ENODEV;
			return -1;
		}
		/* Must be an existing directory. */
		if (hfs_lib_stat(vp, &st) != 0 || !S_ISDIR(st.st_mode)) {
			errno = ENOTDIR;
			return -1;
		}
		pthread_rwlock_wrlock(&g_hfs_cwd_lock);
		snprintf(g_hfs_cwd, sizeof(g_hfs_cwd), "%s", fullPath);
		g_hfs_cwd_active = true;
		pthread_rwlock_unlock(&g_hfs_cwd_lock);
		return 0;
	}
	/* Preserve the HFS cwd when the real chdir fails. */
	if (real_chdir(path) != 0) {
		return -1;
	}
	pthread_rwlock_wrlock(&g_hfs_cwd_lock);
	g_hfs_cwd_active = false;
	pthread_rwlock_unlock(&g_hfs_cwd_lock);
	return 0;
}

/* Intercepted fchdir(2): update the tracked cwd for an HFS directory fd. */
int fchdir(int fd) {
	char innerPath[PATH_MAX];
	char fullPath[PATH_MAX];

	if (hfs_is_hfs_fd(fd)) {
		if (hfs_lib_get_dir_path(fd, innerPath, sizeof(innerPath)) != 0) {
			return -1;
		}
		if (!hfs_inner_to_full_path(innerPath, fullPath, sizeof(fullPath))) {
			errno = ENAMETOOLONG;
			return -1;
		}
		pthread_rwlock_wrlock(&g_hfs_cwd_lock);
		snprintf(g_hfs_cwd, sizeof(g_hfs_cwd), "%s", fullPath);
		g_hfs_cwd_active = true;
		pthread_rwlock_unlock(&g_hfs_cwd_lock);
		return 0;
	}
	return real_fchdir(fd);
}

/* Return the virtual HFS cwd while preserving libc allocation semantics. */
char *getcwd(char *buf, size_t size) {
	char cwd[PATH_MAX];
	size_t length;
	bool active;

	pthread_rwlock_rdlock(&g_hfs_cwd_lock);
	active = g_hfs_cwd_active;
	if (active) {
		snprintf(cwd, sizeof(cwd), "%s", g_hfs_cwd);
	}
	pthread_rwlock_unlock(&g_hfs_cwd_lock);
	if (!active) {
		return real_getcwd(buf, size);
	}
	length = strlen(cwd);
	if (buf == NULL) {
		if (size != 0 && length + 1 > size) {
			errno = ERANGE;
			return NULL;
		}
		buf = malloc(length + 1);
		if (buf == NULL) {
			errno = ENOMEM;
			return NULL;
		}
	} else if (size <= length) {
		errno = ERANGE;
		return NULL;
	}
	memcpy(buf, cwd, length + 1);
	return buf;
}

/* Return an allocated virtual HFS cwd for libc callers without a buffer. */
char *get_current_dir_name(void) {
	bool active;

	pthread_rwlock_rdlock(&g_hfs_cwd_lock);
	active = g_hfs_cwd_active;
	pthread_rwlock_unlock(&g_hfs_cwd_lock);
	if (active) {
		return getcwd(NULL, 0);
	}
	if (real_get_current_dir_name != NULL) {
		return real_get_current_dir_name();
	}
	return real_getcwd(NULL, 0);
}

/* Canonicalize a path lexically; HFS exposes no symbolic links. */
static bool hfs_normalize_path(const char *path, char *normalized,
				       size_t size) {
	char copy[PATH_MAX];
	char *component;
	char *saveptr = NULL;
	size_t length = 1;

	if (path == NULL || normalized == NULL || size < 2 ||
	    snprintf(copy, sizeof(copy), "%s", path) >= (int)sizeof(copy) ||
	    copy[0] != '/') {
		return false;
	}
	normalized[0] = '/';
	normalized[1] = '\0';
	for (component = strtok_r(copy, "/", &saveptr);
	     component != NULL;
	     component = strtok_r(NULL, "/", &saveptr)) {
		size_t componentLength;
		if (strcmp(component, ".") == 0 || component[0] == '\0') {
			continue;
		}
		if (strcmp(component, "..") == 0) {
			if (length > 1) {
				char *slash = strrchr(normalized, '/');
				*slash = '\0';
				length = (size_t)(slash - normalized);
				if (length == 0) {
					length = 1;
					normalized[0] = '/';
					normalized[1] = '\0';
				}
			}
			continue;
		}
		componentLength = strlen(component);
		if (length > 1 && length + 1 + componentLength + 1 > size) {
			return false;
		}
		if (length > 1) {
			normalized[length++] = '/';
		}
		if (length + componentLength + 1 > size) {
			return false;
		}
		memcpy(normalized + length, component, componentLength + 1);
		length += componentLength;
	}
	return true;
}

/* Return a canonical path for an existing managed HFS inode. */
static char *hfs_realpath_managed(const char *path, char *resolved,
					  size_t resolvedSize) {
	char fullPath[PATH_MAX];
	char normalized[PATH_MAX];
	struct stat st;
	const char *innerPath;
	char *result;
	size_t length;

	if (!hfs_full_path_buf(path, fullPath, sizeof(fullPath)) ||
	    !hfs_normalize_path(fullPath, normalized, sizeof(normalized))) {
		errno = ENAMETOOLONG;
		return NULL;
	}
	if (hfs_lib_init() != 0) {
		errno = ENODEV;
		return NULL;
	}
	innerPath = hfs_inner_path(normalized);
	if (hfs_lib_stat(innerPath, &st) != 0) {
		return NULL;
	}
	length = strlen(normalized);
	if (resolved == NULL) {
		if (resolvedSize != 0 && length + 1 > resolvedSize) {
			errno = ERANGE;
			return NULL;
		}
		result = malloc(length + 1);
		if (result == NULL) {
			errno = ENOMEM;
			return NULL;
		}
	} else {
		if (resolvedSize <= length) {
			errno = ERANGE;
			return NULL;
		}
		result = resolved;
	}
	memcpy(result, normalized, length + 1);
	return result;
}

/* Intercept realpath and its fortified variant for the HFS namespace. */
char *realpath(const char *path, char *resolved) {
	char fullPath[PATH_MAX];

	if (hfs_full_path_buf(path, fullPath, sizeof(fullPath))) {
		return hfs_realpath_managed(path, resolved, resolved != NULL ? PATH_MAX : 0);
	}
	return real_realpath(path, resolved);
}

/* Intercept __realpath_chk while enforcing the caller's destination size. */
char *__realpath_chk(const char *path, char *resolved, size_t resolvedSize) {
	char fullPath[PATH_MAX];

	if (hfs_full_path_buf(path, fullPath, sizeof(fullPath))) {
		return hfs_realpath_managed(path, resolved, resolvedSize);
	}
	return real___realpath_chk(path, resolved, resolvedSize);
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

/* Return true when fstatat flags are meaningful for HFS's no-symlink tree. */
static bool hfs_fstatat_flags_supported(int flags) {
	int supported = 0;

#ifdef AT_SYMLINK_NOFOLLOW
	supported |= AT_SYMLINK_NOFOLLOW;
#endif
#ifdef AT_NO_AUTOMOUNT
	supported |= AT_NO_AUTOMOUNT;
#endif
#ifdef AT_EMPTY_PATH
	supported |= AT_EMPTY_PATH;
#endif
	return (flags & ~supported) == 0;
}

/* Route one fstatat call; return -2 when libc should handle it. */
static int hfs_intercept_fstatat(int dirfd, const char *path,
					 struct stat *st, int flags) {
	char pathBuf[PATH_MAX];
	int managed;

	if (!hfs_fstatat_flags_supported(flags)) {
		managed = hfs_view_at_path(dirfd, path, pathBuf, sizeof(pathBuf));
		if (managed < 0) {
			return -1;
		}
		if (managed > 0) {
			errno = EINVAL;
			return -1;
		}
		return -2;
	}
#ifdef AT_EMPTY_PATH
	if ((flags & AT_EMPTY_PATH) != 0 && path != NULL && path[0] == '\0' &&
	    hfs_is_hfs_fd(dirfd)) {
		return hfs_lib_fstat(dirfd, st);
	}
#endif
	managed = hfs_view_at_path(dirfd, path, pathBuf, sizeof(pathBuf));
	if (managed < 0) {
		return -1;
	}
	if (managed > 0) {
		if (hfs_lib_init() != 0) {
			errno = ENODEV;
			return -1;
		}
		return hfs_lib_stat(pathBuf, st);
	}
	return -2;
}

/* Intercepted fstatat(2) / newfstatat with HFS directory-fd support. */
int fstatat(int dirfd, const char *path, struct stat *st, int flags) {
	int rc = hfs_intercept_fstatat(dirfd, path, st, flags);

	if (rc != -2) {
		return rc;
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
	int rc = hfs_intercept_fstatat(dirfd, path, st, flags);

	if (rc != -2) {
		return rc;
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
	int rc = hfs_intercept_fstatat(dirfd, path, (struct stat *)st, flags);

	if (rc != -2) {
		return rc;
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
	int rc = hfs_intercept_fstatat(dirfd, path, (struct stat *)st, flags);

	if (rc != -2) {
		return rc;
	}
	return real___fxstatat64(ver, dirfd, path, st, flags);
}

/* Convert the common stat result into the Linux statx representation. */
static void hfs_fill_statx(const struct stat *st, struct statx *stx) {
	memset(stx, 0, sizeof(*stx));
	stx->stx_mask = STATX_TYPE | STATX_MODE | STATX_NLINK | STATX_UID |
			STATX_GID | STATX_INO | STATX_SIZE | STATX_BLOCKS |
			STATX_ATIME | STATX_MTIME | STATX_CTIME | STATX_BTIME;
	stx->stx_blksize = (uint32_t)st->st_blksize;
	stx->stx_nlink = (uint32_t)st->st_nlink;
	stx->stx_uid = (uint32_t)st->st_uid;
	stx->stx_gid = (uint32_t)st->st_gid;
	stx->stx_mode = (uint16_t)st->st_mode;
	stx->stx_ino = (uint64_t)st->st_ino;
	stx->stx_size = (uint64_t)st->st_size;
	stx->stx_blocks = (uint64_t)st->st_blocks;
	stx->stx_atime.tv_sec = st->st_atim.tv_sec;
	stx->stx_atime.tv_nsec = (uint32_t)st->st_atim.tv_nsec;
	stx->stx_mtime.tv_sec = st->st_mtim.tv_sec;
	stx->stx_mtime.tv_nsec = (uint32_t)st->st_mtim.tv_nsec;
	stx->stx_ctime.tv_sec = st->st_ctim.tv_sec;
	stx->stx_ctime.tv_nsec = (uint32_t)st->st_ctim.tv_nsec;
	stx->stx_btime = stx->stx_ctime;
}

/* Return true when statx flags only request supported synchronization hints. */
static bool hfs_statx_flags_supported(int flags) {
	int supported = 0;

#ifdef AT_EMPTY_PATH
	supported |= AT_EMPTY_PATH;
#endif
#ifdef AT_NO_AUTOMOUNT
	supported |= AT_NO_AUTOMOUNT;
#endif
#ifdef AT_STATX_SYNC_AS_STAT
	supported |= AT_STATX_SYNC_AS_STAT;
#endif
#ifdef AT_STATX_FORCE_SYNC
	supported |= AT_STATX_FORCE_SYNC;
#endif
#ifdef AT_STATX_DONT_SYNC
	supported |= AT_STATX_DONT_SYNC;
#endif
	return (flags & ~supported) == 0;
}

/* Intercept statx(2) for managed paths and HFS directory fds. */
int statx(int dirfd, const char *path, int flags, unsigned int mask,
	  struct statx *stx) {
	struct stat st;
	char pathBuf[PATH_MAX];
	int managed;

	(void)mask;

	if (!hfs_statx_flags_supported(flags)) {
		managed = hfs_view_at_path(dirfd, path, pathBuf, sizeof(pathBuf));
		if (managed < 0) {
			return -1;
		}
		if (managed > 0) {
			errno = EINVAL;
			return -1;
		}
		return real_statx(dirfd, path, flags, mask, stx);
	}
	if (stx == NULL) {
		errno = EFAULT;
		return -1;
	}
#ifdef AT_EMPTY_PATH
	if ((flags & AT_EMPTY_PATH) != 0 && path != NULL && path[0] == '\0' &&
	    hfs_is_hfs_fd(dirfd)) {
		if (hfs_lib_fstat(dirfd, &st) != 0) {
			return -1;
		}
		hfs_fill_statx(&st, stx);
		return 0;
	}
#endif
	managed = hfs_view_at_path(dirfd, path, pathBuf, sizeof(pathBuf));
	if (managed < 0) {
		return -1;
	}
	if (managed > 0) {
		if (hfs_lib_init() != 0) {
			errno = ENODEV;
			return -1;
		}
		if (hfs_lib_stat(pathBuf, &st) != 0) {
			return -1;
		}
		hfs_fill_statx(&st, stx);
		return 0;
	}
	return real_statx(dirfd, path, flags, mask, stx);
}

/* Intercept statfs(2) for a managed path. */
int statfs(const char *path, struct statfs *st) {
	const char *vp = hfs_view_path(path);

	if (vp != NULL) {
		if (hfs_lib_init() != 0) {
			errno = ENODEV;
			return -1;
		}
		return hfs_lib_statfs(vp, st);
	}
	return real_statfs(path, st);
}

/* Intercept fstatfs(2) for an HFS fd. */
int fstatfs(int fd, struct statfs *st) {
	if (hfs_is_hfs_fd(fd)) {
		return hfs_lib_fstatfs(fd, st);
	}
	return real_fstatfs(fd, st);
}

/* Intercept statvfs(3) for a managed path. */
int statvfs(const char *path, struct statvfs *st) {
	const char *vp = hfs_view_path(path);

	if (vp != NULL) {
		if (hfs_lib_init() != 0) {
			errno = ENODEV;
			return -1;
		}
		return hfs_lib_statvfs(vp, st);
	}
	return real_statvfs(path, st);
}

/* Intercept fstatvfs(3) for an HFS fd. */
int fstatvfs(int fd, struct statvfs *st) {
	if (hfs_is_hfs_fd(fd)) {
		return hfs_lib_fstatvfs(fd, st);
	}
	return real_fstatvfs(fd, st);
}

/* Intercept the large-file statfs ABI used by database runtimes. */
int statfs64(const char *path, struct statfs64 *st) {
	const char *vp = hfs_view_path(path);

	if (vp != NULL) {
		if (hfs_lib_init() != 0) {
			errno = ENODEV;
			return -1;
		}
		return hfs_lib_statfs(vp, (struct statfs *)st);
	}
	return hfs_real_statfs64_call(path, st);
}

/* Intercept the large-file fd statfs ABI used by database runtimes. */
int fstatfs64(int fd, struct statfs64 *st) {
	if (hfs_is_hfs_fd(fd)) {
		return hfs_lib_fstatfs(fd, (struct statfs *)st);
	}
	return hfs_real_fstatfs64_call(fd, st);
}

/* Intercept the large-file statvfs ABI used by CPython and database tools. */
int statvfs64(const char *path, struct statvfs64 *st) {
	const char *vp = hfs_view_path(path);

	if (vp != NULL) {
		if (hfs_lib_init() != 0) {
			errno = ENODEV;
			return -1;
		}
		return hfs_lib_statvfs(vp, (struct statvfs *)st);
	}
	return hfs_real_statvfs64_call(path, st);
}

/* Intercept the large-file fd statvfs ABI used by CPython and database tools. */
int fstatvfs64(int fd, struct statvfs64 *st) {
	if (hfs_is_hfs_fd(fd)) {
		return hfs_lib_fstatvfs(fd, (struct statvfs *)st);
	}
	return hfs_real_fstatvfs64_call(fd, st);
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
	return hfs_real_access_call(path, mode);
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

/* Intercept chmod(2) for managed paths. */
int chmod(const char *path, mode_t mode) {
	const char *vp = hfs_view_path(path);

	if (vp != NULL) {
		if (hfs_lib_init() != 0) {
			errno = ENODEV;
			return -1;
		}
		return hfs_lib_chmod(vp, mode);
	}
	return real_chmod(path, mode);
}

/* Intercept fchmod(2) for HFS fds. */
int fchmod(int fd, mode_t mode) {
	if (hfs_is_hfs_fd(fd)) {
		return hfs_lib_fchmod(fd, mode);
	}
	return real_fchmod(fd, mode);
}

/* Intercept fchmodat(2) for managed paths and HFS directory fds. */
int fchmodat(int dirfd, const char *path, mode_t mode, int flags) {
	char pathBuf[PATH_MAX];
	int managed;

	managed = hfs_view_at_path(dirfd, path, pathBuf, sizeof(pathBuf));
	if (managed < 0) {
		return -1;
	}
	if (managed > 0) {
#ifdef AT_SYMLINK_NOFOLLOW
		if ((flags & ~AT_SYMLINK_NOFOLLOW) != 0) {
			errno = EOPNOTSUPP;
			return -1;
		}
#else
		if (flags != 0) {
			errno = EOPNOTSUPP;
			return -1;
		}
#endif
		if (hfs_lib_init() != 0) {
			errno = ENODEV;
			return -1;
		}
		return hfs_lib_chmod(pathBuf, mode);
	}
	return real_fchmodat(dirfd, path, mode, flags);
}

/* Intercept chown(2) for managed paths. */
int chown(const char *path, uid_t uid, gid_t gid) {
	const char *vp = hfs_view_path(path);

	if (vp != NULL) {
		if (hfs_lib_init() != 0) {
			errno = ENODEV;
			return -1;
		}
		return hfs_lib_chown(vp, uid, gid);
	}
	return real_chown(path, uid, gid);
}

/* Intercept lchown(2); HFS has no symbolic links. */
int lchown(const char *path, uid_t uid, gid_t gid) {
	const char *vp = hfs_view_path(path);

	if (vp != NULL) {
		if (hfs_lib_init() != 0) {
			errno = ENODEV;
			return -1;
		}
		return hfs_lib_chown(vp, uid, gid);
	}
	return real_lchown(path, uid, gid);
}

/* Intercept fchown(2) for HFS fds. */
int fchown(int fd, uid_t uid, gid_t gid) {
	if (hfs_is_hfs_fd(fd)) {
		return hfs_lib_fchown(fd, uid, gid);
	}
	return real_fchown(fd, uid, gid);
}

/* Intercept fchownat(2) for managed paths and HFS directory fds. */
int fchownat(int dirfd, const char *path, uid_t uid, gid_t gid, int flags) {
	char pathBuf[PATH_MAX];
	int managed;

	managed = hfs_view_at_path(dirfd, path, pathBuf, sizeof(pathBuf));
	if (managed < 0) {
		return -1;
	}
#ifdef AT_EMPTY_PATH
	if (managed > 0 && path[0] == '\0' && (flags & AT_EMPTY_PATH) != 0 &&
	    hfs_is_hfs_fd(dirfd)) {
		if ((flags & ~AT_EMPTY_PATH) != 0) {
			errno = EINVAL;
			return -1;
		}
		return hfs_lib_fchown(dirfd, uid, gid);
	}
#endif
	if (managed > 0) {
#ifdef AT_SYMLINK_NOFOLLOW
		if ((flags & ~AT_SYMLINK_NOFOLLOW) != 0) {
			errno = EINVAL;
			return -1;
		}
#else
		if (flags != 0) {
			errno = EINVAL;
			return -1;
		}
#endif
		if (hfs_lib_init() != 0) {
			errno = ENODEV;
			return -1;
		}
		return hfs_lib_chown(pathBuf, uid, gid);
	}
	return real_fchownat(dirfd, path, uid, gid, flags);
}

/* Intercept utimensat(2) for managed paths and HFS directory fds. */
int utimensat(int dirfd, const char *path, const struct timespec times[2],
	      int flags) {
	char pathBuf[PATH_MAX];
	int managed;

	managed = hfs_view_at_path(dirfd, path, pathBuf, sizeof(pathBuf));
	if (managed < 0) {
		return -1;
	}
#ifdef AT_EMPTY_PATH
	if (managed > 0 && path[0] == '\0' && (flags & AT_EMPTY_PATH) != 0 &&
	    hfs_is_hfs_fd(dirfd)) {
		if ((flags & ~AT_EMPTY_PATH) != 0) {
			errno = EINVAL;
			return -1;
		}
		return hfs_lib_futimens(dirfd, times);
	}
#endif
	if (managed > 0) {
#ifdef AT_SYMLINK_NOFOLLOW
		if ((flags & ~AT_SYMLINK_NOFOLLOW) != 0) {
			errno = EINVAL;
			return -1;
		}
#else
		if (flags != 0) {
			errno = EINVAL;
			return -1;
		}
#endif
		if (hfs_lib_init() != 0) {
			errno = ENODEV;
			return -1;
		}
		return hfs_lib_utimens(pathBuf, times);
	}
	return real_utimensat(dirfd, path, times, flags);
}

/* Intercept futimens(3) for HFS fds. */
int futimens(int fd, const struct timespec times[2]) {
	if (hfs_is_hfs_fd(fd)) {
		return hfs_lib_futimens(fd, times);
	}
	return real_futimens(fd, times);
}

/* Return whether a fcntl command consumes the optional third argument. */
static bool hfs_fcntl_requires_arg(int cmd) {
	switch (cmd) {
	case F_GETFD:
	case F_GETFL:
#ifdef F_GETOWN
	case F_GETOWN:
#endif
#ifdef F_GETSIG
	case F_GETSIG:
#endif
#ifdef F_GETLEASE
	case F_GETLEASE:
#endif
#ifdef F_GETPIPE_SZ
	case F_GETPIPE_SZ:
#endif
#ifdef F_GET_SEALS
	case F_GET_SEALS:
#endif
		return false;
	default:
		return true;
	}
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
	if (hfs_fcntl_requires_arg(cmd)) {
		arg = va_arg(ap, uintptr_t);
	}
	va_end(ap);

	if (hfs_is_hfs_fd(fd)) {
		if (cmd == F_DUPFD || cmd == F_DUPFD_CLOEXEC) {
			return hfs_fcntl_dup(fd, cmd, arg);
		}
		return hfs_lib_fcntl(fd, cmd, (long)arg);
	}
	return hfs_fcntl_requires_arg(cmd) ? real_fcntl(fd, cmd, arg) :
					     real_fcntl(fd, cmd);
}

/*
 * Intercepted fcntl64(2): glibc large-file variant, used by python
 * for F_DUPFD_CLOEXEC among others.
 */
int fcntl64(int fd, int cmd, ...) {
	va_list ap;
	uintptr_t arg = 0;

	va_start(ap, cmd);
	if (hfs_fcntl_requires_arg(cmd)) {
		arg = va_arg(ap, uintptr_t);
	}
	va_end(ap);

	if (hfs_is_hfs_fd(fd)) {
		if (cmd == F_DUPFD || cmd == F_DUPFD_CLOEXEC) {
			return hfs_fcntl_dup(fd, cmd, arg);
		}
		return hfs_lib_fcntl(fd, cmd, (long)arg);
	}
	return hfs_fcntl_requires_arg(cmd) ? real_fcntl64(fd, cmd, arg) :
					       real_fcntl64(fd, cmd);
}

/* Export the glibc version used by Python's internal fcntl64 reference. */
__asm__(".symver fcntl64, fcntl64@GLIBC_2.28");

/*
 * Registered HFS directory handles. DIR is opaque to applications, so
 * readdir/closedir resolve a pointer by scanning this table.
 */
#define HFS_MAX_OPEN_DIRS	512

struct hfs_open_dir {
	struct hfs_dir *hdir;
	int ownedFd;
};

static struct hfs_open_dir g_hfsDirs[HFS_MAX_OPEN_DIRS];
static int g_hfsDirCount;
static pthread_rwlock_t g_hfsDirsLock = PTHREAD_RWLOCK_INITIALIZER;

/* Find a registered HFS directory; caller holds the registry lock. */
static int hfs_find_dir_locked(DIR *dirp) {
	int i;

	for (i = 0; i < g_hfsDirCount; i++) {
		if (g_hfsDirs[i].hdir == (struct hfs_dir *)dirp) {
			return i;
		}
	}
	return -1;
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
	}
	return real_fdopendir;
}

/* Shared readdir logic: HFS handles return the next snapshot entry. */
static struct dirent *hfs_readdir_impl(DIR *dirp,
				       struct dirent *(*real_fn)(DIR *)) {
	struct hfs_dirent *e;
	int index;

	pthread_rwlock_rdlock(&g_hfsDirsLock);
	index = hfs_find_dir_locked(dirp);
	if (index >= 0) {
		e = hfs_lib_readdir(g_hfsDirs[index].hdir);
		if (e == NULL) {
			pthread_rwlock_unlock(&g_hfsDirsLock);
			return NULL;	/* end of directory */
		}
		memset(&hfs_dir_result, 0, sizeof(hfs_dir_result));
		hfs_dir_result.d_ino = (ino_t)e->inodeId;
		hfs_dir_result.d_type = e->d_type;
		strncpy(hfs_dir_result.d_name, e->name,
			sizeof(hfs_dir_result.d_name) - 1);
		pthread_rwlock_unlock(&g_hfsDirsLock);
		return &hfs_dir_result;
	}
	pthread_rwlock_unlock(&g_hfsDirsLock);
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
		pthread_rwlock_wrlock(&g_hfsDirsLock);
		if (g_hfsDirCount >= HFS_MAX_OPEN_DIRS) {
			pthread_rwlock_unlock(&g_hfsDirsLock);
			hfs_lib_closedir(d);
			errno = EMFILE;
			return NULL;
		}
		g_hfsDirs[g_hfsDirCount].hdir = d;
		g_hfsDirs[g_hfsDirCount].ownedFd = -1;
		g_hfsDirCount++;
		pthread_rwlock_unlock(&g_hfsDirsLock);
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
	int index;

	pthread_rwlock_rdlock(&g_hfsDirsLock);
	index = hfs_find_dir_locked(dirp);
	if (index >= 0) {
		e = hfs_lib_readdir(g_hfsDirs[index].hdir);
		if (e == NULL) {
			pthread_rwlock_unlock(&g_hfsDirsLock);
			return NULL;	/* end of directory */
		}
		memset(&hfs_dir64_result, 0, sizeof(hfs_dir64_result));
		hfs_dir64_result.d_ino = (ino_t)e->inodeId;
		hfs_dir64_result.d_type = e->d_type;
		strncpy(hfs_dir64_result.d_name, e->name,
			sizeof(hfs_dir64_result.d_name) - 1);
		pthread_rwlock_unlock(&g_hfsDirsLock);
		return &hfs_dir64_result;
	}
	pthread_rwlock_unlock(&g_hfsDirsLock);
	return hfs_real_readdir64_get()(dirp);
}

/* Intercepted closedir(2): unregister and release HFS handles. */
int closedir(DIR *dirp) {
	int index;
	int ownedFd;
	int rc;

	pthread_rwlock_wrlock(&g_hfsDirsLock);
	index = hfs_find_dir_locked(dirp);
	if (index >= 0) {
		ownedFd = g_hfsDirs[index].ownedFd;
		g_hfsDirs[index] = g_hfsDirs[g_hfsDirCount - 1];
		g_hfsDirCount--;
		rc = hfs_lib_closedir((struct hfs_dir *)dirp);
		pthread_rwlock_unlock(&g_hfsDirsLock);
		if (rc == 0 && ownedFd >= 0) {
			rc = hfs_lib_close(ownedFd);
		}
		return rc;
	}
	pthread_rwlock_unlock(&g_hfsDirsLock);
	return hfs_real_closedir_get()(dirp);
}

/* Intercepted fdopendir(2): turn an HFS directory fd into a snapshot handle. */
DIR *fdopendir(int fd) {
	char path[PATH_MAX];
	struct hfs_dir *dir;
	int index;

	if (hfs_is_hfs_fd(fd)) {
		if (hfs_lib_get_dir_path(fd, path, sizeof(path)) != 0) {
			return NULL;
		}
		dir = hfs_lib_opendir(path);
		if (dir == NULL) {
			return NULL;
		}
		pthread_rwlock_wrlock(&g_hfsDirsLock);
		if (g_hfsDirCount >= HFS_MAX_OPEN_DIRS) {
			pthread_rwlock_unlock(&g_hfsDirsLock);
			hfs_lib_closedir(dir);
			errno = EMFILE;
			return NULL;
		}
		index = g_hfsDirCount++;
		g_hfsDirs[index].hdir = dir;
		g_hfsDirs[index].ownedFd = fd;
		pthread_rwlock_unlock(&g_hfsDirsLock);
		return (DIR *)dir;
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

/* Intercept readlinkat(2) for absolute paths and HFS directory fds. */
ssize_t readlinkat(int dirfd, const char *path, char *buf, size_t bufsiz) {
	char pathBuf[PATH_MAX];
	struct stat st;
	int managed = hfs_view_at_path(dirfd, path, pathBuf, sizeof(pathBuf));

	if (managed < 0) {
		return -1;
	}
	if (managed > 0) {
		if (hfs_lib_init() != 0) {
			errno = ENODEV;
			return -1;
		}
		if (hfs_lib_stat(pathBuf, &st) != 0) {
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
	real___open_2 = hfs_real_sym("__open_2");
	real___open64_2 = hfs_real_sym("__open64_2");
	real___openat_2 = hfs_real_sym("__openat_2");
	real___openat64_2 = hfs_real_sym("__openat64_2");
	real_close = hfs_real_sym("close");
	real_read = hfs_real_sym("read");
	real_fread = hfs_real_sym("fread");
	real_fread_unlocked = hfs_real_sym("fread_unlocked");
	real_fileno = hfs_real_sym("fileno");
	real_feof = hfs_real_sym("feof");
	real_ferror = hfs_real_sym("ferror");
	real_clearerr = hfs_real_sym("clearerr");
	real_fclose = hfs_real_sym("fclose");
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
	real_mkdirat = hfs_real_sym("mkdirat");
	real_rmdir = hfs_real_sym("rmdir");
	real_rename = hfs_real_sym("rename");
	real_renameat = hfs_real_sym("renameat");
	real_renameat2 = hfs_real_sym("renameat2");
	real_chdir = hfs_real_sym("chdir");
	real_fchdir = hfs_real_sym("fchdir");
	real_getcwd = hfs_real_sym("getcwd");
	real_get_current_dir_name = hfs_real_sym("get_current_dir_name");
	real_realpath = hfs_real_sym("realpath");
	real___realpath_chk = hfs_real_sym("__realpath_chk");
	real_stat = hfs_real_sym("stat");
	real_lstat = hfs_real_sym("lstat");
	real_fstat = hfs_real_sym("fstat");
	real_fstatat = hfs_real_sym("fstatat");
	real_statx = hfs_real_sym("statx");
	real_statfs = hfs_real_sym("statfs");
	real_fstatfs = hfs_real_sym("fstatfs");
	real_statfs64 = hfs_real_sym("statfs64");
	real_fstatfs64 = hfs_real_sym("fstatfs64");
	real_statvfs = hfs_real_sym("statvfs");
	real_fstatvfs = hfs_real_sym("fstatvfs");
	real_statvfs64 = hfs_real_sym("statvfs64");
	real_fstatvfs64 = hfs_real_sym("fstatvfs64");
	/* Some libc builds expose only the non-64 statfs entry points. */
	if (real_statfs64 == NULL) {
		real_statfs64 = (int (*)(const char *, struct statfs64 *))real_statfs;
	}
	if (real_fstatfs64 == NULL) {
		real_fstatfs64 = (int (*)(int, struct statfs64 *))real_fstatfs;
	}
	if (real_statvfs64 == NULL) {
		real_statvfs64 = (int (*)(const char *, struct statvfs64 *))real_statvfs;
	}
	if (real_fstatvfs64 == NULL) {
		real_fstatvfs64 = (int (*)(int, struct statvfs64 *))real_fstatvfs;
	}
	real_chmod = hfs_real_sym("chmod");
	real_fchmod = hfs_real_sym("fchmod");
	real_fchmodat = hfs_real_sym("fchmodat");
	real_chown = hfs_real_sym("chown");
	real_lchown = hfs_real_sym("lchown");
	real_fchown = hfs_real_sym("fchown");
	real_fchownat = hfs_real_sym("fchownat");
	real_utimensat = hfs_real_sym("utimensat");
	real_futimens = hfs_real_sym("futimens");
	real_posix_fadvise = hfs_real_sym("posix_fadvise");
	real_posix_fadvise64 = hfs_real_sym("posix_fadvise64");
	real_mmap = hfs_real_sym("mmap");
	real_mmap64 = hfs_real_sym("mmap64");
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
