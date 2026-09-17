/* Small RocksDB FileSystem contract smoke for a mounted HFS volume. */

#include <cstdlib>
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
    rocksdb::FileOptions options;
    rocksdb::IODebugContext debug;
    std::unique_ptr<rocksdb::FSWritableFile> writable;
    auto status = fileSystem->NewWritableFile("/rocksdb-smoke", options, &writable, &debug);
    if (!status.ok()) {
        return 4;
    }
    const rocksdb::Slice payload("hfs-rocksdb", 12);
    status = writable->Append(payload, options.io_options, &debug);
    if (!status.ok()) {
        return 5;
    }
    status = writable->Close(options.io_options, &debug);
    if (!status.ok()) {
        return 6;
    }
    std::unique_ptr<rocksdb::FSRandomAccessFile> random;
    status = fileSystem->NewRandomAccessFile("/rocksdb-smoke", options, &random, &debug);
    if (!status.ok()) {
        return 7;
    }
    char scratch[12]{};
    rocksdb::Slice result;
    status = random->Read(0, sizeof(scratch), options.io_options, &result, scratch, &debug);
    if (!status.ok() || result.compare(payload) != 0) {
        return 8;
    }
    std::vector<std::string> children;
    status = fileSystem->GetChildren("/", options.io_options, &children, &debug);
    if (!status.ok()) {
        return 9;
    }
    status = fileSystem->DeleteFile("/rocksdb-smoke", options.io_options, &debug);
    return status.ok() ? 0 : 10;
}
