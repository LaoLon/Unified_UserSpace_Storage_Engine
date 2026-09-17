/* Small RocksDB FileSystem contract smoke for a mounted HFS volume. */

#include <cstdlib>
#include <cstdint>
#include <atomic>
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
    /* Shard-83 exposes the complete async read handle contract. */
    int64_t supportedOps = -1;
    fileSystem->SupportedOps(supportedOps);
    if ((supportedOps & (1LL << rocksdb::FSSupportedOps::kAsyncIO)) == 0) {
        return 4;
    }
    rocksdb::FileOptions options;
    rocksdb::IODebugContext debug;
    uint64_t freeSpace = 0;
    auto status = fileSystem->GetFreeSpace("/", options.io_options, &freeSpace, &debug);
    if (!status.ok() || freeSpace == 0) {
        return 5;
    }
    const uint64_t initialFreeSpace = freeSpace;
    std::unique_ptr<rocksdb::FSWritableFile> writable;
    status = fileSystem->NewWritableFile("/rocksdb-smoke", options, &writable, &debug);
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
    status = fileSystem->GetFreeSpace("/", options.io_options, &freeSpace, &debug);
    if (!status.ok() || freeSpace >= initialFreeSpace) {
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
    /* Verify MultiRead preserves independent request results and statuses. */
    char multiScratchA[12]{};
    char multiScratchB[12]{};
    rocksdb::FSReadRequest multiRequests[2];
    multiRequests[0].offset = 0;
    multiRequests[0].len = sizeof(multiScratchA);
    multiRequests[0].scratch = multiScratchA;
    multiRequests[1].offset = 0;
    multiRequests[1].len = sizeof(multiScratchB);
    multiRequests[1].scratch = multiScratchB;
    status = random->MultiRead(multiRequests, 2, options.io_options, &debug);
    if (!status.ok() || !multiRequests[0].status.ok() ||
        !multiRequests[1].status.ok() || multiRequests[0].result.compare(payload) != 0 ||
        multiRequests[1].result.compare(payload) != 0) {
        return 11;
    }
    std::atomic<bool> callbackCalled = false;
    rocksdb::FSReadRequest request;
    request.offset = 0;
    request.len = sizeof(scratch);
    request.scratch = scratch;
    void *ioHandle = nullptr;
    rocksdb::IOHandleDeleter deleter;
    status = random->ReadAsync(
        request, options.io_options,
        [&callbackCalled](const rocksdb::FSReadRequest &, void *) {
            callbackCalled.store(true);
        }, nullptr, &ioHandle, &deleter, &debug);
    if (!status.ok() || ioHandle == nullptr || !deleter) {
        return 12;
    }
    std::vector<void *> asyncHandles{ioHandle};
    status = fileSystem->Poll(asyncHandles, 1);
    if (!status.ok() || !callbackCalled.load() || request.result.compare(payload) != 0) {
        deleter(ioHandle);
        return 12;
    }
    deleter(ioHandle);
    /* Exercise immediate AbortIO against several in-flight reads. */
    constexpr std::size_t cancelCount = 8;
    char cancelScratch[cancelCount][12]{};
    rocksdb::FSReadRequest cancelRequests[cancelCount];
    void *cancelHandles[cancelCount]{};
    rocksdb::IOHandleDeleter cancelDeleters[cancelCount];
    std::vector<void *> cancelHandleList;
    cancelHandleList.reserve(cancelCount);
    std::atomic<std::size_t> cancelCallbacks = 0;
    for (std::size_t index = 0; index < cancelCount; ++index) {
        cancelRequests[index].offset = 0;
        cancelRequests[index].len = sizeof(cancelScratch[index]);
        cancelRequests[index].scratch = cancelScratch[index];
        status = random->ReadAsync(
            cancelRequests[index], options.io_options,
            [&cancelCallbacks](const rocksdb::FSReadRequest &, void *) {
                cancelCallbacks.fetch_add(1);
            }, nullptr, &cancelHandles[index], &cancelDeleters[index], &debug);
        if (!status.ok() || cancelHandles[index] == nullptr || !cancelDeleters[index]) {
            return 12;
        }
        cancelHandleList.push_back(cancelHandles[index]);
    }
    status = fileSystem->AbortIO(cancelHandleList);
    if (!status.ok() || !fileSystem->Poll(cancelHandleList, cancelCount).ok() ||
        cancelCallbacks.load() > cancelCount) {
        for (std::size_t index = 0; index < cancelCount; ++index) {
            cancelDeleters[index](cancelHandles[index]);
        }
        return 12;
    }
    for (std::size_t index = 0; index < cancelCount; ++index) {
        cancelDeleters[index](cancelHandles[index]);
    }
    std::vector<std::string> children;
    status = fileSystem->GetChildren("/", options.io_options, &children, &debug);
    if (!status.ok()) {
        return 13;
    }
    /* Check idempotent directory creation and the resident directory handle. */
    status = fileSystem->CreateDirIfMissing("/rocksdb-dir", options.io_options, &debug);
    if (!status.ok() ||
        !fileSystem->CreateDirIfMissing("/rocksdb-dir", options.io_options, &debug).ok()) {
        return 14;
    }
    std::unique_ptr<rocksdb::FSDirectory> directory;
    status = fileSystem->NewDirectory("/rocksdb-dir", options.io_options, &directory, &debug);
    if (!status.ok()) {
        return 15;
    }
    directory.reset();
    /* Release the last open inode reference before checking extent reclaim. */
    random.reset();
    status = fileSystem->DeleteFile("/rocksdb-smoke", options.io_options, &debug);
    if (!status.ok()) {
        return 16;
    }
    status = fileSystem->DeleteFile("/rocksdb-link", options.io_options, &debug);
    if (!status.ok() || !fileSystem->DeleteDir("/rocksdb-dir", options.io_options, &debug).ok()) {
        return 16;
    }
    status = fileSystem->GetFreeSpace("/", options.io_options, &freeSpace, &debug);
    if (!status.ok() || freeSpace != initialFreeSpace) {
        return 16;
    }
    return 0;
}
