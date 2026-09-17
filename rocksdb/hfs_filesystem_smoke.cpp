/* Small RocksDB FileSystem contract smoke for a mounted HFS volume. */

#include <cstdlib>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "rocksdb/hfs_filesystem.hpp"
#include "rocksdb/file_system.h"

int main(int argc, char **argv) {
    if (argc != 2) {
        return 2;
    }
    auto fileSystem = hfs::rocksdb_adapter::createFileSystem(argv[1]);
    if (fileSystem == nullptr) {
        return 3;
    }
    /* Buffered Shard-82 intentionally exposes no unimplemented async capability. */
    int64_t supportedOps = -1;
    fileSystem->SupportedOps(supportedOps);
    if (supportedOps != 0) {
        return 4;
    }
    rocksdb::FileOptions options;
    rocksdb::IODebugContext debug;
    std::unique_ptr<rocksdb::FSWritableFile> writable;
    auto status = fileSystem->NewWritableFile("/rocksdb-smoke", options, &writable, &debug);
    if (!status.ok()) {
        return 5;
    }
    const rocksdb::Slice payload("hfs-rocksdb", 12);
    status = writable->Append(payload, options.io_options, &debug);
    if (!status.ok()) {
        return 6;
    }
    status = writable->Close(options.io_options, &debug);
    if (!status.ok()) {
        return 7;
    }
    /* Verify LinkFile preserves the same inode before deleting both names. */
    status = fileSystem->LinkFile("/rocksdb-smoke", "/rocksdb-link", options.io_options,
                                  &debug);
    if (!status.ok()) {
        return 8;
    }
    uint64_t linkCount = 0;
    bool sameFile = false;
    if (!fileSystem->NumFileLinks("/rocksdb-smoke", options.io_options, &linkCount, &debug).ok() ||
        !fileSystem->AreFilesSame("/rocksdb-smoke", "/rocksdb-link", options.io_options,
                                  &sameFile, &debug).ok() ||
        linkCount != 2 || !sameFile) {
        return 9;
    }
    std::unique_ptr<rocksdb::FSRandomAccessFile> random;
    status = fileSystem->NewRandomAccessFile("/rocksdb-link", options, &random, &debug);
    if (!status.ok()) {
        return 10;
    }
    char scratch[12]{};
    rocksdb::Slice result;
    status = random->Read(0, sizeof(scratch), options.io_options, &result, scratch, &debug);
    if (!status.ok() || result.compare(payload) != 0) {
        return 11;
    }
    std::vector<std::string> children;
    status = fileSystem->GetChildren("/", options.io_options, &children, &debug);
    if (!status.ok()) {
        return 12;
    }
    /* Check idempotent directory creation and the resident directory handle. */
    status = fileSystem->CreateDirIfMissing("/rocksdb-dir", options.io_options, &debug);
    if (!status.ok() ||
        !fileSystem->CreateDirIfMissing("/rocksdb-dir", options.io_options, &debug).ok()) {
        return 13;
    }
    std::unique_ptr<rocksdb::FSDirectory> directory;
    status = fileSystem->NewDirectory("/rocksdb-dir", options.io_options, &directory, &debug);
    if (!status.ok()) {
        return 14;
    }
    directory.reset();
    status = fileSystem->DeleteFile("/rocksdb-smoke", options.io_options, &debug);
    if (!status.ok()) {
        return 15;
    }
    status = fileSystem->DeleteFile("/rocksdb-link", options.io_options, &debug);
    if (!status.ok() || !fileSystem->DeleteDir("/rocksdb-dir", options.io_options, &debug).ok()) {
        return 15;
    }
    return 0;
}
