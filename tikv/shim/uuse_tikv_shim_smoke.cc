/* Smoke for the UUSE C ABI shim (Shard-116): create/write/read/append/truncate/
 * sync/rename/remove on a formatted UUSE volume.
 * Usage: uuse_tikv_shim_smoke <uuse.conf> [path] */

#include "uuse_tikv_shim.h"

#include <cstdio>
#include <cstring>

#define CHECK(condition, message)                                              \
    do {                                                                       \
        if (!(condition)) {                                                    \
            std::fprintf(stderr, "SHIM_SMOKE_FAIL: %s (line %d)\n", message,   \
                         __LINE__);                                            \
            return 1;                                                          \
        }                                                                      \
    } while (0)

int main(int argc, char **argv) {
    const char *conf = argc > 1 ? argv[1] : "/root/code/config/uuse_nvme.conf";
    const char *path = argc > 2 ? argv[2] : "/shim-smoke.dat";

    uuse_tikv_engine *engine = nullptr;
    CHECK(uuse_tikv_engine_open(conf, &engine) == 0, "engine_open");

    (void)uuse_tikv_remove(engine, path); /* fresh state */

    uuse_tikv_file *file = nullptr;
    CHECK(uuse_tikv_open(engine, path,
                         UUSE_TIKV_OPEN_CREATE | UUSE_TIKV_OPEN_TRUNCATE,
                         &file) == 0,
          "open");

    const char payload[] = "uuse-tikv-shim";
    const long long payloadBytes = static_cast<long long>(sizeof(payload));
    CHECK(uuse_tikv_pwrite(file, payload, sizeof(payload), 0) == payloadBytes,
          "pwrite");
    CHECK(uuse_tikv_append(file, payload, sizeof(payload)) == payloadBytes,
          "append");
    CHECK(uuse_tikv_file_size(file) == 2 * payloadBytes, "file_size");

    char buffer[64] = {};
    CHECK(uuse_tikv_pread(file, buffer, sizeof(payload), 0) == payloadBytes,
          "pread");
    CHECK(std::memcmp(buffer, payload, sizeof(payload)) == 0, "pread content");

    CHECK(uuse_tikv_sync(file) == 0, "sync");
    CHECK(uuse_tikv_truncate(file, 4) == 0, "truncate");
    CHECK(uuse_tikv_file_size(file) == 4, "file_size after truncate");
    uuse_tikv_close(file);

    int exists = 0;
    CHECK(uuse_tikv_exists(engine, path, &exists) == 0 && exists == 1, "exists");
    CHECK(uuse_tikv_rename(engine, path, "/shim-smoke-renamed.dat") == 0, "rename");
    CHECK(uuse_tikv_remove(engine, "/shim-smoke-renamed.dat") == 0, "remove");
    CHECK(uuse_tikv_exists(engine, "/shim-smoke-renamed.dat", &exists) == 0 &&
              exists == 0,
          "exists after remove");

    uuse_tikv_engine_close(engine);
    std::printf("SHIM_SMOKE_PASS\n");
    return 0;
}
