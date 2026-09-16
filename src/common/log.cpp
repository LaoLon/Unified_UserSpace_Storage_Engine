#include "common/log.hpp"

#include <cstdarg>
#include <cstdio>

namespace hfs::log {

namespace {

void emit(const char *prefix, const char *format, va_list args) {
    std::fputs(prefix, stderr);
    std::vfprintf(stderr, format, args);
    std::fputc('\n', stderr);
    std::fflush(stderr);
}

} // namespace

void info(const char *format, ...) {
    va_list args;
    va_start(args, format);
    emit("hfs: ", format, args);
    va_end(args);
}

void error(const char *format, ...) {
    va_list args;
    va_start(args, format);
    emit("hfs error: ", format, args);
    va_end(args);
}

} // namespace hfs::log
