#pragma once

/* Public symbol export marker; the core is built with hidden visibility. */
#if defined(_WIN32)
#define HFS_API
#else
#define HFS_API __attribute__((visibility("default")))
#endif
