#ifndef HFS_LOG_H
#define HFS_LOG_H

#include <stdbool.h>

enum hfs_log_level {
	HFS_LEVEL_DEBUG,
	HFS_LEVEL_INFO,
	HFS_LEVEL_WARN,
	HFS_LEVEL_ERROR
};

int hfs_log_init(void);
void hfs_log_fini(void);
bool hfs_log_enabled(enum hfs_log_level level);

void hfs_log_write(enum hfs_log_level level,
		   const char *file, int line,
		   const char *fmt, ...);

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