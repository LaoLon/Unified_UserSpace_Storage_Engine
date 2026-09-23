#pragma once

namespace hfs::log {

/* Process-wide logging to stderr; HFS_LOG_LEVEL=error suppresses info messages. */
void info(const char *format, ...) __attribute__((format(printf, 1, 2)));
void error(const char *format, ...) __attribute__((format(printf, 1, 2)));

} // namespace hfs::log
