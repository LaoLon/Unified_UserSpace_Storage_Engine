/* Shard-118 probe: does File::sync (through the shim) make a grown file's data
 * and size durable across a process exit that skips destructors?
 * Usage: uuse_tikv_shim_sync_probe <conf> <path> <write-sync|write-nosync|check> */

#include "uuse_tikv_shim.h"

#include <cstdint>
#include <cstdio>
#include <cstring>

#include <unistd.h>

namespace {

constexpr int kChunks = 64;
constexpr std::size_t kChunkSize = 4096;

/* Writes kChunks of 'A' growing the file, optionally syncs, then exits without
 * running any destructor (the process-exit path under test). */
int writeMode(const char *conf, const char *path, bool sync) {
    uuse_tikv_engine *engine = nullptr;
    if (uuse_tikv_engine_open(conf, &engine) != 0) {
        std::fprintf(stderr, "PROBE engine_open failed\n");
        return 2;
    }
    (void)uuse_tikv_remove(engine, path);
    uuse_tikv_file *file = nullptr;
    if (uuse_tikv_open(engine, path,
                       UUSE_TIKV_OPEN_CREATE | UUSE_TIKV_OPEN_TRUNCATE,
                       &file) != 0) {
        std::fprintf(stderr, "PROBE open failed\n");
        return 3;
    }
    char buffer[kChunkSize];
    std::memset(buffer, 'A', sizeof(buffer));
    for (int i = 0; i < kChunks; ++i) {
        const long long rc = uuse_tikv_pwrite(file, buffer, sizeof(buffer),
                                              static_cast<std::uint64_t>(i) * sizeof(buffer));
        if (rc != static_cast<long long>(sizeof(buffer))) {
            std::fprintf(stderr, "PROBE pwrite failed at %d rc=%lld\n", i, rc);
            return 4;
        }
    }
    if (sync && uuse_tikv_sync(file) != 0) {
        std::fprintf(stderr, "PROBE sync failed\n");
        return 5;
    }
    std::fflush(nullptr);
    _exit(sync ? 10 : 11); /* skip destructors and the shim engine stop */
}

/* Reopens the file in a fresh process and reports size and readable chunks. */
int checkMode(const char *conf, const char *path) {
    uuse_tikv_engine *engine = nullptr;
    if (uuse_tikv_engine_open(conf, &engine) != 0) {
        std::printf("CHECK_FAIL engine_open\n");
        return 2;
    }
    uuse_tikv_file *file = nullptr;
    if (uuse_tikv_open(engine, path, 0, &file) != 0) {
        std::printf("CHECK_FAIL open\n");
        return 3;
    }
    const long long size = uuse_tikv_file_size(file);
    char buffer[kChunkSize];
    long long good = 0;
    for (int i = 0; i < kChunks; ++i) {
        if (uuse_tikv_pread(file, buffer, sizeof(buffer),
                            static_cast<std::uint64_t>(i) * sizeof(buffer)) !=
            static_cast<long long>(sizeof(buffer))) {
            break;
        }
        bool ok = true;
        for (std::size_t j = 0; j < sizeof(buffer); ++j) {
            if (buffer[j] != 'A') { ok = false; break; }
        }
        if (!ok) break;
        ++good;
    }
    std::printf("CHECK size=%lld expected=%d good_chunks=%lld/%d\n", size,
                kChunks * static_cast<int>(kChunkSize), good, kChunks);
    uuse_tikv_close(file);
    uuse_tikv_engine_close(engine);
    return (size == static_cast<long long>(kChunks) * kChunkSize && good == kChunks) ? 0 : 1;
}

} // namespace

int main(int argc, char **argv) {
    if (argc < 4) {
        std::fprintf(stderr,
                     "usage: %s <conf> <path> <write-sync|write-nosync|check>\n",
                     argv[0]);
        return 1;
    }
    const char *mode = argv[3];
    if (std::strcmp(mode, "write-sync") == 0) return writeMode(argv[1], argv[2], true);
    if (std::strcmp(mode, "write-nosync") == 0) return writeMode(argv[1], argv[2], false);
    if (std::strcmp(mode, "check") == 0) return checkMode(argv[1], argv[2]);
    std::fprintf(stderr, "unknown mode %s\n", mode);
    return 1;
}
