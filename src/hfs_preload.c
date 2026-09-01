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
static int (*real_close)(int fd);

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

	if (hfs_path_managed(path)) {
		if (hfs_lib_init() != 0) {
			errno = ENODEV;
			return -1;
		}
		va_start(ap, flags);
		mode = va_arg(ap, mode_t);
		va_end(ap);
		return hfs_lib_open(path, flags, mode);
	}

	va_start(ap, flags);
	mode = va_arg(ap, mode_t);
	va_end(ap);
	return real_open(path, flags, mode);
}

/* Intercepted close(2): HFS fds are closed via hfs_lib_close. */
int close(int fd) {
	if (hfs_is_hfs_fd(fd)) {
		return hfs_lib_close(fd);
	}
	return real_close(fd);
}

/*
 * Runs before main(): capture the real libc symbols and the mount
 * prefix. SPDK initialization is deferred to the first managed call:
 * running it here would deadlock with glibc's loader lock. Children
 * of an initialized process (fork+exec) run in pass-through mode.
 */
__attribute__((constructor))
static void hfs_preload_ctor(void) {
	fprintf(stderr, "libhfs preload active\n");
	HFS_LOG_INFO("libhfs preload active");

	hfs_prefix = getenv("HFS_MOUNT_PREFIX");
	if (hfs_prefix == NULL) {
		hfs_prefix = "/hfs/";
	}
	hfs_prefix_len = strlen(hfs_prefix);

	real_open = hfs_real_sym("open");
	real_close = hfs_real_sym("close");

	if (getenv("_HFS_INIT_DONE") != NULL) {
		hfs_inherited = true;
		fprintf(stderr, "libhfs pass-through mode (inherited)\n");
		HFS_LOG_INFO("libhfs pass-through mode (inherited)");
	}
}

/* Runs at process exit: unmount and free the filesystem. */
__attribute__((destructor))
static void hfs_preload_dtor(void) {
	hfs_lib_fini();
}