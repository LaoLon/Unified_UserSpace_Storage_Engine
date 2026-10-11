/* Minimal C++ probe: call the shim functions directly. */
#include "uuse_minio_shim.h"

#include <cstdio>
#include <cstring>

int main(int argc, char **argv) {
    const char *conf = argv[1];
    const char *path = argv[2];
    char errbuf[UUSE_MINIO_ERRBUF] = {};
    uuse_minio_engine *engine = nullptr;
    if (uuse_minio_engine_open(conf, &engine, errbuf, sizeof(errbuf)) != 0) {
        std::printf("engine-open-fail: %s\n", errbuf);
        return 1;
    }
    uuse_minio_file *file = nullptr;
    int rc = uuse_minio_open(engine, path,
                             UUSE_MINIO_OPEN_CREATE | UUSE_MINIO_OPEN_TRUNCATE,
                             &file, errbuf, sizeof(errbuf));
    std::printf("open rc=%d err=%s\n", rc, errbuf);
    if (rc != 0) return 1;
    const char payload[] = "cpp-probe-payload";
    long long wrote = uuse_minio_pwrite(file, payload, sizeof(payload), 0);
    std::printf("pwrite wrote=%lld\n", wrote);
    int syncRc = uuse_minio_fdatasync(file);
    std::printf("sync rc=%d\n", syncRc);
    uuse_minio_close(file);
    uuse_minio_engine_close(engine);
    return wrote == (long long)sizeof(payload) ? 0 : 1;
}
