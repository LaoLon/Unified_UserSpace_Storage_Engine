#pragma once

namespace hfs::log {

/* Process-wide logging to stderr; frontends may replace this in later shards. */
void info(const char *format, ...) __attribute__((format(printf, 1, 2)));
void error(const char *format, ...) __attribute__((format(printf, 1, 2)));

} // namespace hfs::log
