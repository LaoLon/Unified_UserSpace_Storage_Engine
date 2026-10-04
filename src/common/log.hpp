#pragma once

namespace uuse::log {

/* Process-wide logging to stderr; UUSE_LOG_LEVEL=error suppresses info messages. */
void info(const char *format, ...) __attribute__((format(printf, 1, 2)));
void error(const char *format, ...) __attribute__((format(printf, 1, 2)));

} // namespace uuse::log
