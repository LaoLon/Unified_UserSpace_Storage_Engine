#ifndef HFS_LOG_H
#define HFS_LOG_H

#include <stdbool.h>

/* Log severity levels, lowest to highest. */
enum hfs_log_level {
	HFS_LEVEL_DEBUG,	/* verbose diagnostics (off by default) */
	HFS_LEVEL_INFO,		/* normal operational messages */
	HFS_LEVEL_WARN,		/* recoverable problems */
	HFS_LEVEL_ERROR		/* failures */
};

/*
 * Start the logging subsystem: opens /var/log/hfs, spawns the
 * background writer thread. Returns 0 on success.
 */
int hfs_log_init(void);
/* Stop logging: drain the queue, flush and close the log file. */
void hfs_log_fini(void);
/* True if messages at 'level' are currently logged. */
bool hfs_log_enabled(enum hfs_log_level level);

/*
 * Format and enqueue one log line (never blocks the caller).
 * The line is written to the log file by the background thread.
 */
void hfs_log_write(enum hfs_log_level level,
		   const char *file, int line,
		   const char *fmt, ...);

/* Check the level, then format and enqueue the message. */
#define HFS_LOG(level, ...) do {			\
	if (hfs_log_enabled(level)) {			\
		hfs_log_write(level, __FILE__, __LINE__,	\
			      __VA_ARGS__);		\
	}						\
} while (0)

#define HFS_LOG_DEBUG(...) HFS_LOG(HFS_LEVEL_DEBUG, __VA_ARGS__)
#define HFS_LOG_INFO(...)  HFS_LOG(HFS_LEVEL_INFO,  __VA_ARGS__)
#define HFS_LOG_WARN(...)  HFS_LOG(HFS_LEVEL_WARN,  __VA_ARGS__)
#define HFS_LOG_ERROR(...) HFS_LOG(HFS_LEVEL_ERROR, __VA_ARGS__)

#endif /* HFS_LOG_H */