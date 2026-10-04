#include "common/log.hpp"

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace uuse::log {

namespace {

/* Keep normal benchmark output quiet while retaining diagnostics by default. */
bool infoEnabled() {
    static const bool enabled = [] {
        const char *level = std::getenv("UUSE_LOG_LEVEL");
        if (level == nullptr) return true;
        return std::strcmp(level, "error") != 0 && std::strcmp(level, "quiet") != 0 &&
               std::strcmp(level, "off") != 0;
    }();
    return enabled;
}

void emit(const char *prefix, const char *format, va_list args) {
    std::fputs(prefix, stderr);
    std::vfprintf(stderr, format, args);
    std::fputc('\n', stderr);
    std::fflush(stderr);
}

} // namespace

void info(const char *format, ...) {
    if (!infoEnabled()) return;
    va_list args;
    va_start(args, format);
    emit("uuse: ", format, args);
    va_end(args);
}

void error(const char *format, ...) {
    va_list args;
    va_start(args, format);
    emit("uuse error: ", format, args);
    va_end(args);
}

} // namespace uuse::log
