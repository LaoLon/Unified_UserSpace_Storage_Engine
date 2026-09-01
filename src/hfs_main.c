#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <spdk/log.h>

#include "hfs.h"
#include "hfs_internal.h"
#include "hfs_lib.h"
#include "hfs_log.h"
#include "hfs_meta.h"

/*
 * Self-test program: exercises the full HFS lifecycle.
 * Runs mount -> open -> write -> offset read/write -> close -> umount,
 * plus metadata (super blob + inode table) persistence checks.
 */

/* Log a self-test assertion: INFO on pass, ERROR on failure. */
#define HFS_LOG_CHECK(fmt, ok, ...) do {		\
	if (ok) {					\
		HFS_LOG_INFO(fmt, __VA_ARGS__);		\
	} else {					\
		HFS_LOG_ERROR(fmt " [CHECK FAILED]",	\
			      __VA_ARGS__);		\
	}						\
} while (0)

int main(int argc, char *argv[]) {
	if (hfs_lib_init() != 0) {
		fprintf(stderr, "hfs_lib_init failed\n");
		return -1;
	}

	Hfs *fs = hfs_lib_get_fs();

	/*
	 * Reload the super blob id within the same process: verifies that
	 * set_super persisted (malloc bdev loses data across restarts).
	 */
	int loadRc = hfsMetaLoad(fs);
	SPDK_NOTICELOG("reload meta rc=%d\n", loadRc);
	HFS_LOG_CHECK("reload meta rc=%d", loadRc == 0, loadRc);

	/*
	 * Inode table round-trip: the root inode created at format time
	 * must survive saveAll -> reset -> loadAll.
	 */
	if (hfsMetaSaveAll(fs) != 0) {
		HFS_LOG_ERROR("saveAll failed");
		SPDK_NOTICELOG("saveAll failed\n");
		return -1;
	}
	fs->inodeCount = 0;
	if (hfsMetaLoadAll(fs) != 0) {
		HFS_LOG_ERROR("loadAll failed");
		SPDK_NOTICELOG("loadAll failed\n");
		return -1;
	}
	SPDK_NOTICELOG("inodeCount=%u first=%s id=%llu\n", fs->inodeCount,
		       fs->inodeCount > 0 ? fs->inodes[0].name : "?",
		       (unsigned long long)(fs->inodeCount > 0 ? fs->inodes[0].id : 0));
	HFS_LOG_CHECK("inodeCount=%u first=%s id=%llu",
		      fs->inodeCount > 0,
		      fs->inodeCount, fs->inodeCount > 0 ? fs->inodes[0].name : "?",
		      (unsigned long long)(fs->inodeCount > 0 ? fs->inodes[0].id : 0));

	/* Path resolution: "/" resolves to the root inode. */
	struct hfs_inode *root = hfsMetaResolvePath(fs, "/");
	SPDK_NOTICELOG("resolve / -> %s id=%llu\n",
		       root ? root->name : "(null)",
		       (unsigned long long)(root ? root->id : 0));
	HFS_LOG_CHECK("resolve / -> %s id=%llu", root != NULL,
		      root ? root->name : "?", (unsigned long long)(root ? root->id : 0));

	/* A missing path must resolve to NULL with errno=ENOENT. */
	errno = 0;
	struct hfs_inode *missing = hfsMetaResolvePath(fs, "/nonexist");
	SPDK_NOTICELOG("resolve /nonexist -> %s errno=%d\n",
		       missing ? missing->name : "(null)", errno);
	HFS_LOG_CHECK("resolve /nonexist errno=%d",
		      missing == NULL && errno == ENOENT, errno);

	/* mkdir /d1 -> stat exists -> rmdir -> stat gone. */
	if (hfs_lib_mkdir("/d1", 0755) != 0) {
		HFS_LOG_ERROR("mkdir /d1 failed errno=%d", errno);
		SPDK_NOTICELOG("mkdir /d1 failed errno=%d\n", errno);
		return -1;
	}
	struct stat dstat;
	int sr = hfs_lib_stat("/d1", &dstat);
	SPDK_NOTICELOG("stat /d1 rc=%d mode=%o\n", sr,
		       (unsigned int)(sr == 0 ? dstat.st_mode : 0));
	HFS_LOG_CHECK("stat /d1 mode=%o", sr == 0,
		      (unsigned int)(sr == 0 ? dstat.st_mode : 0));

	/* Duplicate mkdir must fail with EEXIST. */
	errno = 0;
	int dupRc = hfs_lib_mkdir("/d1", 0755);
	SPDK_NOTICELOG("mkdir /d1 (dup) rc=%d errno=%d\n", dupRc, errno);
	HFS_LOG_CHECK("mkdir /d1 (dup) errno=%d", dupRc != 0 && errno == EEXIST, errno);

	/* rmdir on the (empty) directory. */
	errno = 0;
	int rmRc = hfs_lib_rmdir("/d1");
	SPDK_NOTICELOG("rmdir /d1 rc=%d errno=%d\n", rmRc, errno);
	HFS_LOG_CHECK("rmdir /d1 rc=%d", rmRc == 0, rmRc);

	/* After removal the path must be gone. */
	errno = 0;
	sr = hfs_lib_stat("/d1", &dstat);
	SPDK_NOTICELOG("stat /d1 after rmdir rc=%d errno=%d\n", sr, errno);
	HFS_LOG_CHECK("stat /d1 after rmdir errno=%d", sr != 0 && errno == ENOENT, errno);

	/* Create /a.txt with O_CREAT, stat size=0, reopen without O_CREAT. */
	errno = 0;
	int fd1 = hfs_lib_open("/a.txt", O_CREAT | O_RDWR, 0644);
	SPDK_NOTICELOG("open /a.txt (O_CREAT) fd=%d errno=%d\n", fd1, errno);
	HFS_LOG_CHECK("open /a.txt (O_CREAT) fd=%d", fd1 >= 0, fd1);

	errno = 0;
	sr = hfs_lib_stat("/a.txt", &dstat);
	SPDK_NOTICELOG("stat /a.txt rc=%d size=%lld mode=%o\n", sr,
		       (long long)(sr == 0 ? dstat.st_size : -1),
		       (unsigned int)(sr == 0 ? dstat.st_mode : 0));
	HFS_LOG_CHECK("stat /a.txt size=%lld mode=%o", sr == 0,
		      (long long)(sr == 0 ? dstat.st_size : -1),
		      (unsigned int)(sr == 0 ? dstat.st_mode : 0));

	errno = 0;
	int fd2 = hfs_lib_open("/a.txt", O_RDONLY, 0);
	SPDK_NOTICELOG("reopen /a.txt fd=%d errno=%d\n", fd2, errno);
	HFS_LOG_CHECK("reopen /a.txt fd=%d", fd2 >= 0, fd2);
	if (fd1 >= 0) {
		hfs_lib_close(fd1);
	}
	if (fd2 >= 0) {
		hfs_lib_close(fd2);
	}

	/* unlink: create /b.txt -> unlink -> stat ENOENT -> recreate ok. */
	int fd3 = hfs_lib_open("/b.txt", O_CREAT | O_RDWR, 0644);
	SPDK_NOTICELOG("open /b.txt (O_CREAT) fd=%d\n", fd3);
	HFS_LOG_CHECK("open /b.txt (O_CREAT) fd=%d", fd3 >= 0, fd3);
	if (fd3 >= 0) {
		hfs_lib_close(fd3);
	}

	errno = 0;
	int urc = hfs_lib_unlink("/b.txt");
	SPDK_NOTICELOG("unlink /b.txt rc=%d errno=%d\n", urc, errno);
	HFS_LOG_CHECK("unlink /b.txt rc=%d", urc == 0, urc);

	errno = 0;
	sr = hfs_lib_stat("/b.txt", &dstat);
	SPDK_NOTICELOG("stat /b.txt after unlink rc=%d errno=%d\n", sr, errno);
	HFS_LOG_CHECK("stat /b.txt after unlink errno=%d", sr != 0 && errno == ENOENT, errno);

	int fd4 = hfs_lib_open("/b.txt", O_CREAT | O_RDWR, 0644);
	SPDK_NOTICELOG("recreate /b.txt fd=%d\n", fd4);
	HFS_LOG_CHECK("recreate /b.txt fd=%d", fd4 >= 0, fd4);
	if (fd4 >= 0) {
		hfs_lib_close(fd4);
	}

	/* rename: /a.txt -> /c.txt (same dir), old path ENOENT. */
	errno = 0;
	int rrc = hfs_lib_rename("/a.txt", "/c.txt");
	SPDK_NOTICELOG("rename /a.txt -> /c.txt rc=%d errno=%d\n", rrc, errno);
	HFS_LOG_CHECK("rename /a.txt -> /c.txt rc=%d", rrc == 0, rrc);

	errno = 0;
	sr = hfs_lib_stat("/a.txt", &dstat);
	SPDK_NOTICELOG("stat /a.txt after rename rc=%d errno=%d\n", sr, errno);
	HFS_LOG_CHECK("stat /a.txt after rename errno=%d", sr != 0 && errno == ENOENT, errno);

	sr = hfs_lib_stat("/c.txt", &dstat);
	SPDK_NOTICELOG("stat /c.txt rc=%d size=%lld\n", sr,
		       (long long)(sr == 0 ? dstat.st_size : -1));
	HFS_LOG_CHECK("stat /c.txt size=%lld", sr == 0,
		      (long long)(sr == 0 ? dstat.st_size : -1));

	/* Cross-directory rename: mkdir /d2, move /c.txt into it. */
	if (hfs_lib_mkdir("/d2", 0755) != 0) {
		HFS_LOG_ERROR("mkdir /d2 failed errno=%d", errno);
		SPDK_NOTICELOG("mkdir /d2 failed errno=%d\n", errno);
		return -1;
	}
	rrc = hfs_lib_rename("/c.txt", "/d2/c.txt");
	SPDK_NOTICELOG("rename /c.txt -> /d2/c.txt rc=%d errno=%d\n", rrc, errno);
	HFS_LOG_CHECK("rename /c.txt -> /d2/c.txt rc=%d", rrc == 0, rrc);

	sr = hfs_lib_stat("/d2/c.txt", &dstat);
	SPDK_NOTICELOG("stat /d2/c.txt rc=%d\n", sr);
	HFS_LOG_CHECK("stat /d2/c.txt rc=%d", sr == 0, sr);

	/* rmdir /d2 must now fail with ENOTEMPTY (c.txt inside). */
	errno = 0;
	int rr = hfs_lib_rmdir("/d2");
	SPDK_NOTICELOG("rmdir /d2 (non-empty) rc=%d errno=%d\n", rr, errno);
	HFS_LOG_CHECK("rmdir /d2 (non-empty) errno=%d",
		      rr != 0 && errno == ENOTEMPTY, errno);

	/* ftruncate: open /e.txt, write 4 bytes, stat size=4, truncate 100. */
	int fd5 = hfs_lib_open("/e.txt", O_CREAT | O_RDWR, 0644);
	SPDK_NOTICELOG("open /e.txt fd=%d\n", fd5);
	HFS_LOG_CHECK("open /e.txt fd=%d", fd5 >= 0, fd5);
	if (fd5 >= 0) {
		hfs_lib_write(fd5, "data", 4);
		sr = hfs_lib_stat("/e.txt", &dstat);
		SPDK_NOTICELOG("stat /e.txt after write size=%lld\n",
			       (long long)(sr == 0 ? dstat.st_size : -1));
		HFS_LOG_CHECK("stat /e.txt after write size=%lld", sr == 0,
			      (long long)(sr == 0 ? dstat.st_size : -1));

		int trc = hfs_lib_ftruncate(fd5, 100);
		SPDK_NOTICELOG("ftruncate /e.txt 100 rc=%d errno=%d\n", trc, errno);
		HFS_LOG_CHECK("ftruncate /e.txt 100 rc=%d", trc == 0, trc);

		sr = hfs_lib_stat("/e.txt", &dstat);
		SPDK_NOTICELOG("stat /e.txt after truncate size=%lld\n",
			       (long long)(sr == 0 ? dstat.st_size : -1));
		HFS_LOG_CHECK("stat /e.txt after truncate size=%lld", sr == 0,
			      (long long)(sr == 0 ? dstat.st_size : -1));

		errno = 0;
		int frc = hfs_lib_fstat(fd5, &dstat);
		SPDK_NOTICELOG("fstat /e.txt rc=%d size=%lld errno=%d\n", frc,
			       (long long)(frc == 0 ? dstat.st_size : -1), errno);
		HFS_LOG_CHECK("fstat /e.txt size=%lld", frc == 0,
			      (long long)(frc == 0 ? dstat.st_size : -1));

		hfs_lib_close(fd5);
	}

	SPDK_NOTICELOG("hfsEnvSetup success\n");

	SPDK_NOTICELOG("\n\n hfs mount start \n\n");
	HFS_LOG_INFO("hfs mount start");
	hfsMount(fs);

	SPDK_NOTICELOG("\n\n hfs open start \n\n");
	HFS_LOG_INFO("hfs open start");
	HfsFile *file = calloc(1, sizeof(HfsFile));
	file->fs = fs;
	hfsOpen(file);

	SPDK_NOTICELOG("\n\n hfs write start \n\n");
	HFS_LOG_INFO("hfs write start");
	char *buffer = "ABCDEFGHIJKLMNOPQRSTUVWXYZ";
	hfsWrite(file, buffer, strlen(buffer));

	SPDK_NOTICELOG("\n\n hfs offset write/read start \n\n");
	HFS_LOG_INFO("hfs offset write/read start");
	hfsWriteAt(file, (uint8_t *)"BBBB", 4, 100);
	char oBuffer[16] = {0};
	ssize_t on = hfsReadAt(file, (uint8_t *)oBuffer, 4, 100);
	SPDK_NOTICELOG("OFFSET READ: %.*s (n=%zd)\n", (int)on, oBuffer, on);
	HFS_LOG_INFO("OFFSET READ: %.*s (n=%zd)", (int)on, oBuffer, on);

	SPDK_NOTICELOG("\n\n hfs read start \n\n");
	HFS_LOG_INFO("hfs read start");
	char rBuffer[BUFFER_SIZE] = {0};
	hfsReadAt(file, rBuffer, strlen(buffer), 0);
	SPDK_NOTICELOG("READ BUFFER: %s\n", rBuffer);
	HFS_LOG_INFO("READ BUFFER: %s", rBuffer);

	SPDK_NOTICELOG("\n\n hfs close start \n\n");
	HFS_LOG_INFO("hfs close start");
	hfsClose(file);

	free(file);

	SPDK_NOTICELOG("\n\n hfs umount start \n\n");
	HFS_LOG_INFO("hfs umount start");
	hfsUmount(fs);

	HFS_LOG_INFO("hfs exiting");
	hfs_lib_fini();
}