#pragma once

/* Public symbol export marker; the core is built with hidden visibility. */
#if defined(_WIN32)
#define UUSE_API
#else
#define UUSE_API __attribute__((visibility("default")))
#endif
