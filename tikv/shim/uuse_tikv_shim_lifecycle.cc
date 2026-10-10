/* Lifecycle cases for the UUSE C ABI shim (Shard-118): refcounted engine state
 * under thread exit, engine-handle drop with open files, repeated open/close,
 * shared handles and fork-child fail-closed.
 * Usage: uuse_tikv_shim_lifecycle <uuse.conf> [path] */

#include "uuse_tikv_shim.h"

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <thread>

#include <sys/wait.h>
#include <unistd.h>

#define CHECK(condition, message)                                              \
    do {                                                                       \
        if (!(condition)) {                                                    \
            std::fprintf(stderr, "SHIM_LIFECYCLE_FAIL: %s (line %d)\n",        \
                         message, __LINE__);                                   \
            return 1;                                                          \
        }                                                                      \
    } while (0)

namespace {

constexpr char kPayload[] = "uuse-lifecycle";

/* Writes the fixed payload at offset 0 and returns the byte count. */
long long writePayload(uuse_tikv_file *file) {
    return uuse_tikv_pwrite(file, kPayload, sizeof(kPayload), 0);
}

/* Reads back the payload and compares it; returns false on mismatch. */
bool readPayloadMatches(uuse_tikv_file *file) {
    char buffer[32] = {};
    const long long bytes = uuse_tikv_pread(file, buffer, sizeof(kPayload), 0);
    return bytes == static_cast<long long>(sizeof(kPayload)) &&
           std::memcmp(buffer, kPayload, sizeof(kPayload)) == 0;
}

} // namespace

int main(int argc, char **argv) {
    const char *conf = argc > 1 ? argv[1] : "/root/code/config/uuse_nvme.conf";
    const char *path = argc > 2 ? argv[2] : "/shim-lifecycle.dat";

    /* Case 1: engine handle dropped while a file is still open; the file keeps
     * the engine state alive and the engine stops with the last handle. */
    {
        uuse_tikv_engine *engine = nullptr;
        CHECK(uuse_tikv_engine_open(conf, &engine) == 0, "engine_open");
        (void)uuse_tikv_remove(engine, path);
        uuse_tikv_file *file = nullptr;
        CHECK(uuse_tikv_open(engine, path,
                             UUSE_TIKV_OPEN_CREATE | UUSE_TIKV_OPEN_TRUNCATE,
                             &file) == 0,
              "open");
        CHECK(writePayload(file) == static_cast<long long>(sizeof(kPayload)),
              "pwrite");
        /* Namespace metadata becomes durable with the file's durable epoch. */
        CHECK(uuse_tikv_sync(file) == 0, "sync");
        uuse_tikv_engine_close(engine); /* drop the owner reference only */
        CHECK(readPayloadMatches(file), "read after engine handle dropped");
        uuse_tikv_close(file); /* last reference: engine stops here */
        std::printf("case1 engine-drop-with-open-file ok\n");
    }

    /* Case 2: a worker thread opens a file and exits without closing it; the
     * handle is closed on the main thread and still serves data. */
    {
        uuse_tikv_file *threadFile = nullptr;
        std::thread worker([&]() {
            uuse_tikv_engine *engine = nullptr;
            if (uuse_tikv_engine_open(conf, &engine) != 0) return;
            (void)uuse_tikv_remove(engine, path);
            uuse_tikv_file *file = nullptr;
            if (uuse_tikv_open(engine, path,
                               UUSE_TIKV_OPEN_CREATE | UUSE_TIKV_OPEN_TRUNCATE,
                               &file) != 0) {
                uuse_tikv_engine_close(engine);
                return;
            }
            if (writePayload(file) != static_cast<long long>(sizeof(kPayload))) {
                uuse_tikv_close(file);
                uuse_tikv_engine_close(engine);
                return;
            }
            if (uuse_tikv_sync(file) != 0) {
                uuse_tikv_close(file);
                uuse_tikv_engine_close(engine);
                return;
            }
            threadFile = file;       /* ownership moves to the main thread */
            uuse_tikv_engine_close(engine);
        });
        worker.join();
        CHECK(threadFile != nullptr, "worker thread open");
        CHECK(readPayloadMatches(threadFile), "read from another thread");
        uuse_tikv_close(threadFile);
        std::printf("case2 thread-exit-with-open-file ok\n");
    }

    /* Case 3: repeated open/close cycles exercise the weak singleton registry
     * (a fully released engine is recreated on the next open). */
    for (int cycle = 0; cycle < 8; ++cycle) {
        uuse_tikv_engine *engine = nullptr;
        CHECK(uuse_tikv_engine_open(conf, &engine) == 0, "cycle engine_open");
        int exists = 0;
        CHECK(uuse_tikv_exists(engine, path, &exists) == 0 && exists == 1,
              "cycle exists");
        uuse_tikv_engine_close(engine);
    }
    std::printf("case3 repeated open/close ok\n");

    /* Case 4: two live handles share one engine; closing one keeps the other
     * usable. */
    {
        uuse_tikv_engine *first = nullptr;
        uuse_tikv_engine *second = nullptr;
        CHECK(uuse_tikv_engine_open(conf, &first) == 0, "shared open a");
        CHECK(uuse_tikv_engine_open(conf, &second) == 0, "shared open b");
        uuse_tikv_engine_close(first);
        int exists = 0;
        CHECK(uuse_tikv_exists(second, path, &exists) == 0 && exists == 1,
              "second handle after first closed");
        uuse_tikv_engine_close(second);
        std::printf("case4 shared engine handles ok\n");
    }

    /* Case 5: a forked child fails closed with -ECHILD (ForkedProcess). */
    {
        uuse_tikv_engine *engine = nullptr;
        CHECK(uuse_tikv_engine_open(conf, &engine) == 0, "fork engine_open");
        const pid_t pid = fork();
        CHECK(pid >= 0, "fork");
        if (pid == 0) {
            int exists = 0;
            const int rc = uuse_tikv_exists(engine, path, &exists);
            _exit(rc == -ECHILD ? 0 : 1);
        }
        int status = 0;
        CHECK(waitpid(pid, &status, 0) == pid, "waitpid");
        CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0, "child -ECHILD");
        uuse_tikv_engine_close(engine);
        std::printf("case5 fork-child fail-closed ok\n");
    }

    /* Case 6: argument guards; NULL close calls are no-ops. */
    {
        uuse_tikv_engine *engine = nullptr;
        CHECK(uuse_tikv_engine_open(conf, &engine) == 0, "guard engine_open");
        uuse_tikv_file *file = nullptr;
        CHECK(uuse_tikv_engine_open(nullptr, &engine) == -EINVAL,
              "null conf rejected");
        CHECK(uuse_tikv_open(engine, nullptr, 0, &file) == -EINVAL,
              "null path rejected");
        uuse_tikv_close(nullptr);
        uuse_tikv_engine_close(nullptr);
        uuse_tikv_engine_close(engine);
        std::printf("case6 argument guards ok\n");
    }

    std::printf("SHIM_LIFECYCLE_PASS\n");
    return 0;
}
