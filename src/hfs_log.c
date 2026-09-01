#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <spawn.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "hfs_log.h"

/*
 * Rotation and retention limits: rotate at 1 MiB, keep at most 300
 * tar.gz archives, ring queue of 1024 formatted lines, 10 ms poll.
 */
#define HFS_LOG_MAX_SIZE	(1024U * 1024U)
#define HFS_LOG_MAX_BACKUPS	300U
#define HFS_LOG_QUEUE_SIZE	1024U
#define HFS_LOG_LINE_SIZE	1024U
#define HFS_LOG_POLL_MS	10U
#define HFS_LOG_FLUSH_MS	1000U
#define HFS_LOG_TAR_TIMEOUT_S	30
#define HFS_LOG_BATCH_MAX	64U

#define HFS_LOG_ROOT_DIR	"/var/log/hfs"
#define HFS_LOG_RUN_DIR		HFS_LOG_ROOT_DIR "/run"
#define HFS_LOG_BAK_DIR		HFS_LOG_ROOT_DIR "/bak"
#define HFS_LOG_FILE_NAME	"hfs_base_spdk.log"
#define HFS_LOG_FILE_PREFIX	"hfs_base_spdk"

/* One pre-formatted log line waiting in the ring queue. */
struct hfs_log_entry {
	uint32_t len;
	char data[HFS_LOG_LINE_SIZE];
};

/* Ring queue shared by producers (hfs_log_write) and the writer thread. */
static struct hfs_log_entry g_queue[HFS_LOG_QUEUE_SIZE];
static uint32_t g_queueHead;
static uint32_t g_queueTail;
static uint32_t g_queueCount;
static pthread_mutex_t g_queueLock = PTHREAD_MUTEX_INITIALIZER;

static pthread_t g_logThread;
static atomic_bool g_running;
static atomic_uint_fast64_t g_dropped;
static atomic_uint_fast32_t g_seq;

static enum hfs_log_level g_minLevel = HFS_LEVEL_INFO;

static FILE *g_logFile;
static uint64_t g_fileSize;
static uint64_t g_reopenAfterMs;

/* Monotonic clock in milliseconds (scheduling/flush timing). */
static uint64_t hfsLogNowMs(void) {
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000U + (uint64_t)ts.tv_nsec / 1000000U;
}

/* Map a log level to its printable name. */
static const char *hfsLogLevelName(enum hfs_log_level level) {
	switch (level) {
	case HFS_LEVEL_DEBUG:
		return "DEBUG";
	case HFS_LEVEL_INFO:
		return "INFO";
	case HFS_LEVEL_WARN:
		return "WARN";
	case HFS_LEVEL_ERROR:
		return "ERROR";
	}
	return "INFO";
}

/* Format the current wall-clock time as YYYY-MM-DD HH:MM:SS.mmmZ. */
static void hfsLogTimestamp(char *buf, size_t size) {
	struct timespec ts;
	struct tm tm;

	clock_gettime(CLOCK_REALTIME, &ts);
	localtime_r(&ts.tv_sec, &tm);
	snprintf(buf, size, "%04d-%02d-%02d %02d:%02d:%02d.%03ldZ",
		 tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
		 tm.tm_hour, tm.tm_min, tm.tm_sec, ts.tv_nsec / 1000000);
}

/* Replace newlines/carriage returns with escaped forms, keeping one line per entry. */
static void hfsLogEscapeNewlines(char *msg) {
	char tmp[HFS_LOG_LINE_SIZE];
	size_t src = 0;
	size_t dst = 0;

	while (msg[src] != '\0' && dst + 3 < sizeof(tmp)) {
		if (msg[src] == '\n') {
			tmp[dst++] = '\\';
			tmp[dst++] = 'n';
		} else if (msg[src] == '\r') {
			tmp[dst++] = '\\';
			tmp[dst++] = 'r';
		} else {
			tmp[dst++] = msg[src];
		}
		src++;
	}
	tmp[dst] = '\0';
	memcpy(msg, tmp, dst + 1);
}

/* Reopen the log file after a write failure, with a 1 s backoff. */
static void hfsLogTryReopen(void) {
	char path[PATH_MAX];
	struct stat st;

	if (g_logFile != NULL) {
		return;
	}
	if (hfsLogNowMs() < g_reopenAfterMs) {
		return;
	}

	snprintf(path, sizeof(path), "%s/%s", HFS_LOG_RUN_DIR, HFS_LOG_FILE_NAME);
	g_logFile = fopen(path, "a");
	if (g_logFile == NULL) {
		g_reopenAfterMs = hfsLogNowMs() + 1000;
		return;
	}

	g_fileSize = 0;
	if (stat(path, &st) == 0) {
		g_fileSize = (uint64_t)st.st_size;
	}
}

static void hfsLogCloseFile(void) {
	if (g_logFile != NULL) {
		fclose(g_logFile);
		g_logFile = NULL;
	}
}

/* Run tar via posix_spawnp with a timeout; capture stdout into 'out'. */
static int hfsLogRunTar(char *const argv[], char *out, size_t outSize) {
	posix_spawn_file_actions_t actions;
	struct pollfd pfd;
	pid_t pid;
	uint64_t deadline;
	size_t got = 0;
	int status = -1;
	int devnull;
	int pr;
	int pfdFds[2];

	if (pipe(pfdFds) != 0) {
		return -1;
	}

	posix_spawn_file_actions_init(&actions);
	posix_spawn_file_actions_adddup2(&actions, pfdFds[1], STDOUT_FILENO);
	posix_spawn_file_actions_addclose(&actions, pfdFds[0]);

	devnull = open("/dev/null", O_WRONLY);
	if (devnull >= 0) {
		posix_spawn_file_actions_adddup2(&actions, devnull, STDERR_FILENO);
	}

	if (posix_spawnp(&pid, "tar", &actions, NULL, argv, environ) != 0) {
		close(pfdFds[0]);
		close(pfdFds[1]);
		posix_spawn_file_actions_destroy(&actions);
		if (devnull >= 0) {
			close(devnull);
		}
		return -1;
	}
	posix_spawn_file_actions_destroy(&actions);
	close(pfdFds[1]);
	if (devnull >= 0) {
		close(devnull);
	}

	deadline = hfsLogNowMs() + (uint64_t)HFS_LOG_TAR_TIMEOUT_S * 1000U;
	pfd.fd = pfdFds[0];
	pfd.events = POLLIN;

	for (;;) {
		pr = poll(&pfd, 1, 100);
		if (pr > 0 && (pfd.revents & (POLLIN | POLLHUP))) {
			ssize_t r = read(pfdFds[0], out + got, outSize - got - 1);
			if (r > 0) {
				got += (size_t)r;
			} else if (r == 0) {
				break;
			} else if (errno != EAGAIN && errno != EINTR) {
				break;
			}
		} else if (hfsLogNowMs() >= deadline) {
			kill(pid, SIGKILL);
			waitpid(pid, &status, 0);
			close(pfdFds[0]);
			return -1;
		}
	}

	waitpid(pid, &status, 0);
	close(pfdFds[0]);
	if (got < outSize) {
		out[got] = '\0';
	} else {
		out[outSize - 1] = '\0';
	}

	if (WIFEXITED(status)) {
		return WEXITSTATUS(status);
	}
	return -1;
}

struct hfs_backup {
	char name[NAME_MAX + 1];
	time_t mtime;
};

static int hfsLogBackupCmp(const void *a, const void *b) {
	const struct hfs_backup *ba = (const struct hfs_backup *)a;
	const struct hfs_backup *bb = (const struct hfs_backup *)b;

	if (ba->mtime != bb->mtime) {
		return ba->mtime < bb->mtime ? -1 : 1;
	}
	return strcmp(ba->name, bb->name);
}

/* Delete oldest archives so at most HFS_LOG_MAX_BACKUPS-1 remain before publish. */
static int hfsLogCleanupBackups(void) {
	DIR *dir;
	struct dirent *de;
	struct hfs_backup *list = NULL;
	size_t count = 0;
	size_t cap = 0;
	int rc = 0;

	dir = opendir(HFS_LOG_BAK_DIR);
	if (dir == NULL) {
		return -1;
	}

	while ((de = readdir(dir)) != NULL) {
		size_t l = strlen(de->d_name);
		char path[PATH_MAX];
		struct stat st;

		if (strncmp(de->d_name, HFS_LOG_FILE_PREFIX,
			    strlen(HFS_LOG_FILE_PREFIX)) != 0) {
			continue;
		}
		if (l <= 7 || strcmp(de->d_name + l - 7, ".tar.gz") != 0) {
			continue;
		}
		snprintf(path, sizeof(path), "%s/%s", HFS_LOG_BAK_DIR, de->d_name);
		if (stat(path, &st) != 0 || !S_ISREG(st.st_mode)) {
			continue;
		}
		if (count == cap) {
			struct hfs_backup *nl;
			cap = cap ? cap * 2 : 64;
			nl = realloc(list, cap * sizeof(*nl));
			if (nl == NULL) {
				free(list);
				closedir(dir);
				return -1;
			}
			list = nl;
		}
		snprintf(list[count].name, sizeof(list[count].name), "%s", de->d_name);
		list[count].mtime = st.st_mtime;
		count++;
	}
	closedir(dir);

	if (list == NULL) {
		return 0;
	}

	qsort(list, count, sizeof(*list), hfsLogBackupCmp);

	while (count > HFS_LOG_MAX_BACKUPS - 1) {
		char path[PATH_MAX];
		snprintf(path, sizeof(path), "%s/%s", HFS_LOG_BAK_DIR, list[0].name);
		if (unlink(path) != 0) {
			rc = -1;
			break;
		}
		memmove(&list[0], &list[1], (count - 1) * sizeof(*list));
		count--;
	}
	free(list);
	return rc;
}

/* Compress one rotated .log segment into a verified .tar.gz archive. */
static int hfsLogArchiveOne(const char *name) {
	char src[PATH_MAX];
	char tmp[PATH_MAX];
	char final[PATH_MAX];
	char out[4096];
	char *argvCreate[] = {"tar", "-czf", tmp, "-C", HFS_LOG_BAK_DIR, "--", (char *)name, NULL};
	char *argvVerify[] = {"tar", "-tzf", tmp, NULL};

	snprintf(src, sizeof(src), "%s/%s", HFS_LOG_BAK_DIR, name);
	snprintf(tmp, sizeof(tmp), "%s/%s.tar.gz.tmp", HFS_LOG_BAK_DIR, name);
	snprintf(final, sizeof(final), "%s/%s.tar.gz", HFS_LOG_BAK_DIR, name);

	unlink(tmp);

	if (hfsLogRunTar(argvCreate, out, sizeof(out)) != 0) {
		unlink(tmp);
		return -1;
	}
	if (hfsLogRunTar(argvVerify, out, sizeof(out)) != 0 ||
	    strstr(out, name) == NULL) {
		unlink(tmp);
		return -1;
	}
	if (hfsLogCleanupBackups() != 0) {
		unlink(tmp);
		return -1;
	}
	if (rename(tmp, final) != 0) {
		unlink(tmp);
		return -1;
	}
	unlink(src);
	return 0;
}

/* Finish any rotated-but-uncompressed .log segments left in bak/. */
static int hfsLogArchiveOldSegments(void) {
	DIR *dir;
	struct dirent *de;
	int rc = 0;

	dir = opendir(HFS_LOG_BAK_DIR);
	if (dir == NULL) {
		return -1;
	}

	while ((de = readdir(dir)) != NULL) {
		size_t l = strlen(de->d_name);

		if (strncmp(de->d_name, HFS_LOG_FILE_PREFIX,
			    strlen(HFS_LOG_FILE_PREFIX)) != 0) {
			continue;
		}
		if (l <= 4 || strcmp(de->d_name + l - 4, ".log") != 0) {
			continue;
		}
		if (hfsLogArchiveOne(de->d_name) != 0) {
			rc = -1;
			break;
		}
	}
	closedir(dir);
	return rc;
}

/* Build a unique archive base name: prefix.timestamp.pid.seq. */
static int hfsLogMakeArchiveName(char *buf, size_t size) {
	uint32_t i;

	for (i = 0; i < 100; i++) {
		struct timespec ts;
		struct tm tm;
		char stamp[64];
		char probe[PATH_MAX];
		uint32_t seq = atomic_fetch_add(&g_seq, 1) + 1;

		clock_gettime(CLOCK_REALTIME, &ts);
		localtime_r(&ts.tv_sec, &tm);
		snprintf(stamp, sizeof(stamp), "%04d%02d%02d_%02d%02d%02d_%06ld",
			 tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
			 tm.tm_hour, tm.tm_min, tm.tm_sec, ts.tv_nsec / 1000);
		snprintf(buf, size, "%s.%s.%d.%03u",
			 HFS_LOG_FILE_PREFIX, stamp, (int)getpid(), seq);
		snprintf(probe, sizeof(probe), "%s/%s.log", HFS_LOG_BAK_DIR, buf);
		if (access(probe, F_OK) != 0) {
			return 0;
		}
	}
	return -1;
}

/* Rotate the active log: close, rename to bak, reopen, then archive the old segment. */
static bool hfsLogRotate(void) {
	char name[NAME_MAX + 1];
	char src[PATH_MAX];
	char dst[PATH_MAX];
	char dstName[NAME_MAX + 8];

	if (hfsLogArchiveOldSegments() != 0) {
		return false;
	}
	if (g_logFile == NULL) {
		return true;
	}

	fflush(g_logFile);
	if (fclose(g_logFile) != 0) {
		g_logFile = NULL;
		hfsLogTryReopen();
		return true;
	}
	g_logFile = NULL;

	if (hfsLogMakeArchiveName(name, sizeof(name)) != 0) {
		hfsLogTryReopen();
		return true;
	}

	snprintf(src, sizeof(src), "%s/%s", HFS_LOG_RUN_DIR, HFS_LOG_FILE_NAME);
	snprintf(dst, sizeof(dst), "%s/%s.log", HFS_LOG_BAK_DIR, name);
	snprintf(dstName, sizeof(dstName), "%s.log", name);
	if (rename(src, dst) != 0) {
		hfsLogTryReopen();
		return true;
	}

	hfsLogTryReopen();

	hfsLogArchiveOne(dstName);

	return true;
}

/* Write a batch of queued lines to the log file, rotating when the size limit is hit. */
static void hfsLogWriteBatch(struct hfs_log_entry *batch, uint32_t n) {
	uint32_t i;
	bool rotated = false;

	if (g_logFile == NULL) {
		hfsLogTryReopen();
	}

	for (i = 0; i < n; i++) {
		size_t w;

		if (g_logFile == NULL) {
			atomic_fetch_add(&g_dropped, n - i);
			return;
		}
		w = fwrite(batch[i].data, 1, batch[i].len, g_logFile);
		if (w != batch[i].len || ferror(g_logFile)) {
			atomic_fetch_add(&g_dropped, n - i);
			hfsLogCloseFile();
			return;
		}
		g_fileSize += w;
		if (!rotated && g_fileSize >= HFS_LOG_MAX_SIZE) {
			if (hfsLogRotate()) {
				rotated = true;
			} else {
				atomic_fetch_add(&g_dropped, n - i - 1);
				return;
			}
		}
	}
}

/* Direct write from the writer thread (drops summary), bypassing the queue. */
static void hfsLogBackendWrite(enum hfs_log_level level, const char *fmt, ...) {
	char ts[64];
	char msg[HFS_LOG_LINE_SIZE];
	char line[HFS_LOG_LINE_SIZE + 96];
	va_list ap;
	int n;

	if (g_logFile == NULL) {
		return;
	}

	hfsLogTimestamp(ts, sizeof(ts));
	va_start(ap, fmt);
	vsnprintf(msg, sizeof(msg), fmt, ap);
	va_end(ap);
	hfsLogEscapeNewlines(msg);

	n = snprintf(line, sizeof(line), "%s [%s] [tid=%ld] hfs_log.c:0 %s\n",
		     ts, hfsLogLevelName(level), syscall(SYS_gettid), msg);
	if (n < 0) {
		return;
	}
	if (n >= (int)sizeof(line)) {
		n = (int)sizeof(line) - 1;
	}

	if (fwrite(line, 1, (size_t)n, g_logFile) != (size_t)n ||
	    ferror(g_logFile)) {
		hfsLogCloseFile();
		return;
	}
	g_fileSize += (uint64_t)n;
}

/* Writer thread: drains the queue, flushes, rotates and reports drops. */
static void *hfsLogThread(void *arg) {
	struct hfs_log_entry batch[HFS_LOG_BATCH_MAX];
	uint64_t lastFlush = hfsLogNowMs();
	uint64_t lastDropReport = lastFlush;
	uint64_t lastDropped = 0;

	while (atomic_load(&g_running)) {
		uint32_t n = 0;
		uint64_t now;

		pthread_mutex_lock(&g_queueLock);
		while (n < HFS_LOG_BATCH_MAX && g_queueCount > 0) {
			batch[n] = g_queue[g_queueHead];
			g_queueHead = (g_queueHead + 1) % HFS_LOG_QUEUE_SIZE;
			g_queueCount--;
			n++;
		}
		pthread_mutex_unlock(&g_queueLock);

		if (n > 0) {
			hfsLogWriteBatch(batch, n);
		}

		now = hfsLogNowMs();
		if (g_logFile != NULL && now - lastFlush >= HFS_LOG_FLUSH_MS) {
			fflush(g_logFile);
			lastFlush = now;
		}
		if (atomic_load(&g_dropped) != lastDropped &&
		    now - lastDropReport >= 10000) {
			hfsLogBackendWrite(HFS_LEVEL_WARN,
					   "hfs log: %llu message(s) dropped",
					   (unsigned long long)atomic_load(&g_dropped));
			lastDropped = atomic_load(&g_dropped);
			lastDropReport = now;
		}

		if (n == 0) {
			usleep(HFS_LOG_POLL_MS * 1000);
		}
	}

	for (;;) {
		uint32_t n = 0;

		pthread_mutex_lock(&g_queueLock);
		while (n < HFS_LOG_BATCH_MAX && g_queueCount > 0) {
			batch[n] = g_queue[g_queueHead];
			g_queueHead = (g_queueHead + 1) % HFS_LOG_QUEUE_SIZE;
			g_queueCount--;
			n++;
		}
		pthread_mutex_unlock(&g_queueLock);
		if (n == 0) {
			break;
		}
		hfsLogWriteBatch(batch, n);
	}

	if (g_logFile != NULL) {
		fflush(g_logFile);
	}
	return NULL;
}

/* Trim a partial trailing line left by a crash so appends start on a clean line. */
static void hfsLogTrimTail(void) {
	char path[PATH_MAX];
	struct stat st;
	char *buf;
	off_t keep = 0;
	int fd;
	ssize_t rd;
	off_t i;

	snprintf(path, sizeof(path), "%s/%s", HFS_LOG_RUN_DIR, HFS_LOG_FILE_NAME);
	fd = open(path, O_RDWR);
	if (fd < 0) {
		return;
	}
	if (fstat(fd, &st) != 0 || st.st_size <= 0) {
		close(fd);
		return;
	}

	buf = malloc((size_t)st.st_size);
	if (buf == NULL) {
		close(fd);
		return;
	}
	rd = read(fd, buf, (size_t)st.st_size);
	if (rd > 0) {
		for (i = rd - 1; i >= 0; i--) {
			if (buf[i] == '\n') {
				keep = i + 1;
				break;
			}
		}
		if (keep < rd && ftruncate(fd, keep) != 0) {
			HFS_LOG_WARN("trim tail ftruncate to %lld failed errno=%d",
				     (long long)keep, errno);
		}
	}
	free(buf);
	close(fd);
}

/* Remove leftover *.tar.gz.tmp files from previous runs. */
static void hfsLogCleanupTmp(void) {
	DIR *dir;
	struct dirent *de;

	dir = opendir(HFS_LOG_BAK_DIR);
	if (dir == NULL) {
		return;
	}
	while ((de = readdir(dir)) != NULL) {
		size_t l = strlen(de->d_name);

		if (strncmp(de->d_name, HFS_LOG_FILE_PREFIX,
			    strlen(HFS_LOG_FILE_PREFIX)) != 0) {
			continue;
		}
		if (l > 8 && strcmp(de->d_name + l - 8, ".tar.gz.tmp") == 0) {
			char path[PATH_MAX];
			snprintf(path, sizeof(path), "%s/%s", HFS_LOG_BAK_DIR, de->d_name);
			unlink(path);
		}
	}
	closedir(dir);
}

/* Override the minimum level via the HFS_LOG_LEVEL environment variable. */
static void hfsLogSetLevelFromEnv(void) {
	const char *s = getenv("HFS_LOG_LEVEL");

	if (s == NULL) {
		return;
	}
	if (strcasecmp(s, "debug") == 0) {
		g_minLevel = HFS_LEVEL_DEBUG;
	} else if (strcasecmp(s, "info") == 0) {
		g_minLevel = HFS_LEVEL_INFO;
	} else if (strcasecmp(s, "warn") == 0) {
		g_minLevel = HFS_LEVEL_WARN;
	} else if (strcasecmp(s, "error") == 0) {
		g_minLevel = HFS_LEVEL_ERROR;
	}
}

/*
 * Initialize logging: create directories, repair leftovers, open the
 * active log and start the writer thread.
 */
int hfs_log_init(void) {
	char path[PATH_MAX];
	struct stat st;

	if (atomic_load(&g_running)) {
		return 0;
	}

	hfsLogSetLevelFromEnv();

	if (mkdir(HFS_LOG_ROOT_DIR, 0755) != 0 && errno != EEXIST) {
		return -1;
	}
	if (mkdir(HFS_LOG_RUN_DIR, 0755) != 0 && errno != EEXIST) {
		return -1;
	}
	if (mkdir(HFS_LOG_BAK_DIR, 0755) != 0 && errno != EEXIST) {
		return -1;
	}

	hfsLogTrimTail();
	hfsLogCleanupTmp();
	hfsLogArchiveOldSegments();

	snprintf(path, sizeof(path), "%s/%s", HFS_LOG_RUN_DIR, HFS_LOG_FILE_NAME);
	g_logFile = fopen(path, "a");
	if (g_logFile == NULL) {
		return -1;
	}
	if (stat(path, &st) == 0) {
		g_fileSize = (uint64_t)st.st_size;
	}

	atomic_store(&g_running, true);
	if (pthread_create(&g_logThread, NULL, hfsLogThread, NULL) != 0) {
		atomic_store(&g_running, false);
		hfsLogCloseFile();
		return -1;
	}
	return 0;
}

/* Stop the writer thread, drain the queue and close the log file. */
void hfs_log_fini(void) {
	if (!atomic_load(&g_running)) {
		return;
	}
	atomic_store(&g_running, false);
	pthread_join(g_logThread, NULL);
	if (g_logFile != NULL) {
		fflush(g_logFile);
		fclose(g_logFile);
		g_logFile = NULL;
	}
}

/* True if messages at this level should be logged. */
bool hfs_log_enabled(enum hfs_log_level level) {
	return level >= g_minLevel;
}

/* Format a line (non-blocking) and enqueue it; drop on contention/full queue. */
void hfs_log_write(enum hfs_log_level level, const char *file, int line,
		   const char *fmt, ...) {
	char ts[64];
	char msg[HFS_LOG_LINE_SIZE];
	char lineBuf[HFS_LOG_LINE_SIZE + 96];
	const char *base;
	const char *slash;
	va_list ap;
	int n;
	int savedErrno;
	bool enqueued = false;

	if (!atomic_load(&g_running)) {
		return;
	}

	savedErrno = errno;

	hfsLogTimestamp(ts, sizeof(ts));

	slash = strrchr(file, '/');
	base = (slash != NULL) ? slash + 1 : file;

	va_start(ap, fmt);
	vsnprintf(msg, sizeof(msg), fmt, ap);
	va_end(ap);
	hfsLogEscapeNewlines(msg);

	n = snprintf(lineBuf, sizeof(lineBuf), "%s [%s] [tid=%ld] %s:%d %s\n",
		     ts, hfsLogLevelName(level), syscall(SYS_gettid), base, line, msg);
	if (n < 0) {
		n = 0;
	}
	if (n >= (int)sizeof(lineBuf)) {
		n = (int)sizeof(lineBuf) - 1;
	}

	if (pthread_mutex_trylock(&g_queueLock) == 0) {
		if (g_queueCount < HFS_LOG_QUEUE_SIZE) {
			struct hfs_log_entry *e = &g_queue[g_queueTail];
			e->len = (uint32_t)n;
			memcpy(e->data, lineBuf, (size_t)n);
			g_queueTail = (g_queueTail + 1) % HFS_LOG_QUEUE_SIZE;
			g_queueCount++;
			enqueued = true;
		}
		pthread_mutex_unlock(&g_queueLock);
	}
	if (!enqueued) {
		atomic_fetch_add(&g_dropped, 1);
	}

	errno = savedErrno;
}