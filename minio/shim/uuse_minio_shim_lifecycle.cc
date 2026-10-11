/* Lifecycle cases for the UUSE MinIO C ABI shim (Phase 9, Shard-9.1):
 * reference-counted engine state under thread exit, engine-handle drop with
 * open files, repeated open/close (weak registry), shared handles, dir handle
 * lifetime, stats calls and fork-child fail-closed.
 * Usage: uuse_minio_shim_lifecycle <uuse.conf> [path] */

#include "uuse_minio_shim.h"

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <thread>

#include <sys/wait.h>
#include <unistd.h>

#define CHECK(condition, message)                                              \
    do {                                                                       \
        if (!(condition)) {                                                    \
            std::fprintf(stderr, "MINIO_SHIM_LIFECYCLE_FAIL: %s (line %d)\n",  \
                         message, __LINE__);                                   \
            return 1;                                                          \
        }                                                                      \
    } while (0)

namespace {

constexpr char kPayload[] = "minio-lifecycle";

/* Writes the fixed payload at offset 0 and returns the byte count. */
long long writePayload(uuse_minio_file *file) {
    return uuse_minio_pwrite(file, kPayload, sizeof(kPayload), 0);
}

/* Reads back the payload and compares it; returns false on mismatch. */
bool readPayloadMatches(uuse_minio_file *file) {
    char buffer[32] = {};
    const long long bytes = uuse_minio_pread(file, buffer, sizeof(kPayload), 0);
    return bytes == static_cast<long long>(sizeof(kPayload)) &&
           std::memcmp(buffer, kPayload, sizeof(kPayload)) == 0;
}

} // namespace

int main(int argc, char **argv) {
    const char *conf = argc > 1 ? argv[1] : "/root/code/config/uuse_nvme.conf";
    const char *path = argc > 2 ? argv[2] : "/minio-shim-lifecycle.dat";
    char errbuf[UUSE_MINIO_ERRBUF] = {};

    /* Case 1: engine handle dropped while a file is still open; the file keeps
     * the engine state alive and the engine stops with the last handle. */
    {
        uuse_minio_engine *engine = nullptr;
        CHECK(uuse_minio_engine_open(conf, &engine, errbuf, sizeof(errbuf)) == 0,
              "engine_open");
        (void)uuse_minio_remove(engine, path, nullptr, errbuf, sizeof(errbuf));
        uuse_minio_file *file = nullptr;
        CHECK(uuse_minio_open(engine, path, UUSE_MINIO_OPEN_CREATE, &file, errbuf,
                              sizeof(errbuf)) == 0,
              "open");
        CHECK(writePayload(file) == static_cast<long long>(sizeof(kPayload)),
              "pwrite");
        CHECK(uuse_minio_fdatasync(file) == 0, "fdatasync");
        uuse_minio_engine_close(engine); /* drop the owner reference only */
        CHECK(readPayloadMatches(file), "read after engine handle dropped");
        uuse_minio_close(file); /* last reference: engine stops here */
        std::printf("case1 engine-drop-with-open-file ok\n");
    }

    /* Case 2: a worker thread opens a file and exits without closing it; the
     * handle is closed on the main thread and still serves data. */
    {
        uuse_minio_file *threadFile = nullptr;
        std::thread worker([&]() {
            uuse_minio_engine *engine = nullptr;
            if (uuse_minio_engine_open(conf, &engine, errbuf, sizeof(errbuf)) != 0)
                return;
            (void)uuse_minio_remove(engine, path, nullptr, errbuf, sizeof(errbuf));
            uuse_minio_file *file = nullptr;
            if (uuse_minio_open(engine, path, UUSE_MINIO_OPEN_CREATE, &file,
                                errbuf, sizeof(errbuf)) != 0) {
                uuse_minio_engine_close(engine);
                return;
            }
            if (writePayload(file) != static_cast<long long>(sizeof(kPayload))) {
                uuse_minio_close(file);
                uuse_minio_engine_close(engine);
                return;
            }
            if (uuse_minio_fdatasync(file) != 0) {
                uuse_minio_close(file);
                uuse_minio_engine_close(engine);
                return;
            }
            threadFile = file; /* ownership moves to the main thread */
            uuse_minio_engine_close(engine);
        });
        worker.join();
        CHECK(threadFile != nullptr, "worker thread open");
        CHECK(readPayloadMatches(threadFile), "read from another thread");
        uuse_minio_close(threadFile);
        std::printf("case2 thread-exit-with-open-file ok\n");
    }

    /* Case 3: repeated open/close cycles exercise the weak singleton registry
     * (a fully released engine is recreated on the next open). */
    for (int cycle = 0; cycle < 8; ++cycle) {
        uuse_minio_engine *engine = nullptr;
        CHECK(uuse_minio_engine_open(conf, &engine, errbuf, sizeof(errbuf)) == 0,
              "cycle engine_open");
        int exists = 0;
        CHECK(uuse_minio_exists(engine, path, &exists, errbuf, sizeof(errbuf)) ==
                  0 &&
                  exists == 1,
              "cycle exists");
        uuse_minio_engine_close(engine);
    }
    std::printf("case3 repeated open/close ok\n");

    /* Case 4: two live handles share one engine; closing one keeps the other
     * usable. */
    {
        uuse_minio_engine *first = nullptr;
        uuse_minio_engine *second = nullptr;
        CHECK(uuse_minio_engine_open(conf, &first, errbuf, sizeof(errbuf)) == 0,
              "shared open a");
        CHECK(uuse_minio_engine_open(conf, &second, errbuf, sizeof(errbuf)) == 0,
              "shared open b");
        uuse_minio_engine_close(first);
        int exists = 0;
        CHECK(uuse_minio_exists(second, path, &exists, errbuf, sizeof(errbuf)) ==
                  0 &&
                  exists == 1,
              "second handle after first closed");
        uuse_minio_engine_close(second);
        std::printf("case4 shared engine handles ok\n");
    }

    /* Case 5: a dir handle keeps the engine state alive after the engine
     * handle is dropped; iteration still works, then the engine stops. */
    {
        uuse_minio_engine *engine = nullptr;
        CHECK(uuse_minio_engine_open(conf, &engine, errbuf, sizeof(errbuf)) == 0,
              "dir engine_open");
        uuse_minio_dir *dir = nullptr;
        CHECK(uuse_minio_dir_open(engine, "/", &dir, errbuf, sizeof(errbuf)) == 0,
              "dir_open");
        uuse_minio_engine_close(engine);
        uuse_minio_dirent entries[4];
        std::size_t count = 0;
        int eof = 0;
        CHECK(uuse_minio_dir_read(dir, entries, 4, &count, &eof) == 0,
              "dir_read after engine drop");
        uuse_minio_dir_close(dir);
        std::printf("case5 dir-handle keeps engine ok\n");
    }

    /* Case 6: stats calls answer on a live engine. */
    {
        uuse_minio_engine *engine = nullptr;
        CHECK(uuse_minio_engine_open(conf, &engine, errbuf, sizeof(errbuf)) == 0,
              "stats engine_open");
        uint64_t total = 0;
        uint64_t freeBytes = 0;
        uint64_t used = 0;
        CHECK(uuse_minio_disk_stats(engine, &total, &freeBytes, &used, errbuf,
                                    sizeof(errbuf)) == 0,
              "disk_stats");
        std::fprintf(stderr, "debug: disk_stats total=%llu free=%llu used=%llu\n",
                     static_cast<unsigned long long>(total),
                     static_cast<unsigned long long>(freeBytes),
                     static_cast<unsigned long long>(used));
        CHECK(total > 0 && freeBytes <= total, "disk_stats values");
        uint64_t usedInodes = 0;
        uint64_t maxInodes = 0;
        CHECK(uuse_minio_inode_stats(engine, &usedInodes, &maxInodes, errbuf,
                                     sizeof(errbuf)) == 0,
              "inode_stats");
        CHECK(maxInodes > 0 && usedInodes <= maxInodes, "inode_stats values");
        uuse_minio_engine_close(engine);
        std::printf("case6 stats ok (total=%llu free=%llu inodes=%llu/%llu)\n",
                    static_cast<unsigned long long>(total),
                    static_cast<unsigned long long>(freeBytes),
                    static_cast<unsigned long long>(usedInodes),
                    static_cast<unsigned long long>(maxInodes));
    }

    /* Case 7: a forked child fails closed with -ECHILD (ForkedProcess). */
    {
        uuse_minio_engine *engine = nullptr;
        CHECK(uuse_minio_engine_open(conf, &engine, errbuf, sizeof(errbuf)) == 0,
              "fork engine_open");
        const pid_t pid = fork();
        CHECK(pid >= 0, "fork");
        if (pid == 0) {
            int exists = 0;
            const int rc = uuse_minio_exists(engine, path, &exists, errbuf,
                                             sizeof(errbuf));
            _exit(rc == -ECHILD ? 0 : 1);
        }
        int status = 0;
        CHECK(waitpid(pid, &status, 0) == pid, "waitpid");
        CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0, "child -ECHILD");
        uuse_minio_engine_close(engine);
        std::printf("case7 fork-child fail-closed ok\n");
    }

    /* Case 8: argument guards; NULL close calls are no-ops. */
    {
        uuse_minio_engine *engine = nullptr;
        CHECK(uuse_minio_engine_open(conf, &engine, errbuf, sizeof(errbuf)) == 0,
              "guard engine_open");
        uuse_minio_file *file = nullptr;
        CHECK(uuse_minio_engine_open(nullptr, &engine, errbuf, sizeof(errbuf)) ==
                  -EINVAL,
              "null conf rejected");
        CHECK(uuse_minio_open(engine, nullptr, 0, &file, errbuf,
                              sizeof(errbuf)) == -EINVAL,
              "null path rejected");
        CHECK(uuse_minio_open(engine, "/no-such-file", 0, &file, errbuf,
                              sizeof(errbuf)) == -ENOENT,
              "missing path is ENOENT");
        uuse_minio_close(nullptr);
        uuse_minio_dir_close(nullptr);
        uuse_minio_engine_close(nullptr);
        uuse_minio_engine_close(engine);
        std::printf("case8 argument guards ok\n");
    }

    std::printf("MINIO_SHIM_LIFECYCLE_PASS\n");
    return 0;
}
