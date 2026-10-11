/* Smoke for the UUSE MinIO C ABI shim (Phase 9, Shard-9.1): engine open,
 * file create/write/read/sync/truncate/allocate/stat, path ops, bounded
 * directory iteration and recursive delete on a formatted UUSE volume.
 * Usage: uuse_minio_shim_smoke <uuse.conf> [base-dir] */

#include "uuse_minio_shim.h"

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <string>

#define CHECK(condition, message)                                                 \
    do {                                                                          \
        if (!(condition)) {                                                       \
            std::fprintf(stderr, "MINIO_SHIM_SMOKE_FAIL: %s (line %d): %s\n",     \
                         message, __LINE__, errbuf);                              \
            return 1;                                                             \
        }                                                                         \
    } while (0)

namespace {

std::string joinPath(const std::string &base, const std::string &name) {
    if (base.empty() || base == "/") return "/" + name;
    return base + "/" + name;
}

/* Creates <base>/dirA/dirB/file and returns 0 or a negative errno. */
int createNestedFile(uuse_minio_engine *engine, const std::string &base,
                     char *errbuf, std::size_t errlen) {
    int rc = uuse_minio_mkdir_all(engine, joinPath(base, "dirA/dirB").c_str(),
                                  errbuf, errlen);
    if (rc != 0) return rc;
    uuse_minio_file *file = nullptr;
    rc = uuse_minio_open(engine, joinPath(base, "dirA/dirB/file").c_str(),
                         UUSE_MINIO_OPEN_CREATE, &file, errbuf, errlen);
    if (rc != 0) return rc;
    const char payload[] = "nested";
    const long long written =
        uuse_minio_pwrite(file, payload, sizeof(payload), 0);
    uuse_minio_close(file);
    return written == static_cast<long long>(sizeof(payload)) ? 0 : -EIO;
}

} // namespace

int main(int argc, char **argv) {
    const char *conf = argc > 1 ? argv[1] : "/root/code/config/uuse_nvme.conf";
    const std::string base = argc > 2 ? argv[2] : "/minio-shim-smoke";
    char errbuf[UUSE_MINIO_ERRBUF] = {};

    uuse_minio_engine *engine = nullptr;
    CHECK(uuse_minio_engine_open(conf, &engine, errbuf, sizeof(errbuf)) == 0,
          "engine_open");
    (void)uuse_minio_remove_all(engine, base.c_str(), errbuf, sizeof(errbuf));
    /* Run 1: the base directory must exist before Create opens a file in it. */
    CHECK(uuse_minio_mkdir_all(engine, base.c_str(), errbuf, sizeof(errbuf)) == 0,
          "mkdir_all base");

    /* File data path: create/write/read/sync/stat/truncate/allocate. */
    const std::string filePath = joinPath(base, "part.1");
    uuse_minio_file *file = nullptr;
    const int openRc = uuse_minio_open(engine, filePath.c_str(),
                                       UUSE_MINIO_OPEN_CREATE |
                                           UUSE_MINIO_OPEN_EXCLUSIVE,
                                       &file, errbuf, sizeof(errbuf));
    if (openRc != 0) {
        std::fprintf(stderr, "debug: open %s rc=%d errbuf=%s\n", filePath.c_str(),
                     openRc, errbuf);
    }
    CHECK(openRc == 0, "open create");
    const char payload[] = "uuse-minio-shim";
    const long long payloadBytes = static_cast<long long>(sizeof(payload));
    CHECK(uuse_minio_pwrite(file, payload, sizeof(payload), 0) == payloadBytes,
          "pwrite");
    CHECK(uuse_minio_fdatasync(file) == 0, "fdatasync");

    char buffer[64] = {};
    CHECK(uuse_minio_pread(file, buffer, sizeof(payload), 0) == payloadBytes,
          "pread");
    CHECK(std::memcmp(buffer, payload, sizeof(payload)) == 0, "pread content");

    uuse_minio_stat statValue = {};
    CHECK(uuse_minio_fstat(file, &statValue) == 0, "fstat");
    CHECK(statValue.logical_size == sizeof(payload), "fstat size");
    CHECK(uuse_minio_ftruncate(file, 4) == 0, "truncate");
    CHECK(uuse_minio_fstat(file, &statValue) == 0 && statValue.logical_size == 4,
          "fstat after truncate");
    CHECK(uuse_minio_allocate(file, 1 << 20, 4096) == 0, "allocate");
    uuse_minio_close(file);

    /* Path ops: exists/stat/rename/remove. */
    int exists = 0;
    CHECK(uuse_minio_exists(engine, filePath.c_str(), &exists, errbuf,
                            sizeof(errbuf)) == 0 &&
              exists == 1,
          "exists");
    CHECK(uuse_minio_path_stat(engine, filePath.c_str(), &statValue, errbuf,
                               sizeof(errbuf)) == 0 &&
              statValue.directory == 0,
          "stat file");
    const std::string renamedPath = joinPath(base, "part.1.renamed");
    CHECK(uuse_minio_rename(engine, filePath.c_str(), renamedPath.c_str(), errbuf,
                            sizeof(errbuf)) == 0,
          "rename");
    CHECK(uuse_minio_remove(engine, renamedPath.c_str(), &statValue, errbuf,
                            sizeof(errbuf)) == 0,
          "remove");
    CHECK(uuse_minio_exists(engine, renamedPath.c_str(), &exists, errbuf,
                            sizeof(errbuf)) == 0 &&
              exists == 0,
          "exists after remove");

    /* Directory iteration: nested tree, bounded pages with eof flag. */
    CHECK(createNestedFile(engine, base, errbuf, sizeof(errbuf)) == 0,
          "create nested file");
    CHECK(uuse_minio_path_stat(engine, joinPath(base, "dirA").c_str(), &statValue,
                               errbuf, sizeof(errbuf)) == 0 &&
              statValue.directory == 1,
          "stat dir");

    uuse_minio_dir *dir = nullptr;
    CHECK(uuse_minio_dir_open(engine, base.c_str(), &dir, errbuf,
                              sizeof(errbuf)) == 0,
          "dir_open");
    bool sawDirA = false;
    std::size_t pages = 0;
    for (;;) {
        uuse_minio_dirent entries[2]; /* page bound 2 exercises resume */
        std::size_t count = 0;
        int eof = 0;
        CHECK(uuse_minio_dir_read(dir, entries, 2, &count, &eof) == 0,
              "dir_read");
        for (std::size_t i = 0; i < count; ++i) {
            if (std::strcmp(entries[i].name, "dirA") == 0) {
                sawDirA = true;
                CHECK((entries[i].flags & 1u) == 1u, "dirA is directory");
            }
        }
        ++pages;
        if (eof) break;
    }
    uuse_minio_dir_close(dir);
    CHECK(sawDirA, "dirA listed");

    /* Recursive delete: removes the whole tree. */
    CHECK(uuse_minio_remove_all(engine, base.c_str(), errbuf, sizeof(errbuf)) ==
              0,
          "remove_all");
    CHECK(uuse_minio_exists(engine, base.c_str(), &exists, errbuf,
                            sizeof(errbuf)) == 0 &&
              exists == 0,
          "base removed");

    char version[64] = {};
    uuse_minio_version(version, sizeof(version));
    std::printf("version: %s\n", version);
    uuse_minio_engine_close(engine);
    std::printf("MINIO_SHIM_SMOKE_PASS\n");
    return 0;
}
