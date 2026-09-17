/* Small RocksDB FileSystem contract smoke for a mounted HFS volume. */

#include <cstdlib>
#include <cstdint>
#include <atomic>
#include <memory>
#include <string>
#include <thread>
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
    /* The database identity lock must reject a concurrent opener and recover
     * after the first process releases its lock. */
    rocksdb::FileLock *firstLock = nullptr;
    if (!fileSystem->LockFile("/rocksdb-lock-db", options.io_options, nullptr, &debug)
             .IsInvalidArgument()) {
        return 5;
    }
    if (!fileSystem->LockFile("/rocksdb-lock-db", options.io_options, &firstLock, &debug).ok() ||
        firstLock == nullptr) {
        return 5;
    }
    rocksdb::FileLock *secondLock = nullptr;
    auto secondLockStatus =
        fileSystem->LockFile("/rocksdb-lock-db", options.io_options, &secondLock, &debug);
    if (secondLockStatus.ok() || !secondLockStatus.IsBusy() ||
        secondLock != nullptr ||
        !fileSystem->UnlockFile(firstLock, options.io_options, &debug).ok()) {
        if (secondLock != nullptr) {
            fileSystem->UnlockFile(secondLock, options.io_options, &debug);
        }
        if (firstLock != nullptr) {
            fileSystem->UnlockFile(firstLock, options.io_options, &debug);
        }
        return 5;
    }
    firstLock = nullptr;
    if (!fileSystem->LockFile("/rocksdb-lock-db", options.io_options, &firstLock, &debug).ok() ||
        firstLock == nullptr || !fileSystem->UnlockFile(firstLock, options.io_options, &debug).ok()) {
        if (firstLock != nullptr) {
            fileSystem->UnlockFile(firstLock, options.io_options, &debug);
        }
        return 5;
    }
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
    const uint64_t allocatedFreeSpace = freeSpace;
    uint64_t missingSize = 0;
    if (fileSystem->GetFileSize("/rocksdb-missing", options.io_options, &missingSize,
                                &debug).ok()) {
        return 7;
    }
    uint64_t missingMtime = 0;
    if (fileSystem->GetFileModificationTime("/rocksdb-missing", options.io_options,
                                            &missingMtime, &debug).ok()) {
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
    if (!random->MultiRead(nullptr, 0, options.io_options, &debug).ok() ||
        random->MultiRead(nullptr, 1, options.io_options, &debug).ok()) {
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
    /* Exercise concurrent SST-style positioned reads through one shared
     * random-access handle and preserve the payload on every worker. */
    constexpr std::size_t concurrentReaders = 8;
    constexpr std::size_t readsPerReader = 32;
    std::atomic<bool> concurrentReadsOk = true;
    std::vector<std::thread> readerThreads;
    readerThreads.reserve(concurrentReaders);
    for (std::size_t reader = 0; reader < concurrentReaders; ++reader) {
        readerThreads.emplace_back([&, reader] {
            char readerScratch[12]{};
            rocksdb::IODebugContext readerDebug;
            for (std::size_t iteration = 0; iteration < readsPerReader; ++iteration) {
                rocksdb::Slice readerResult;
                auto readerStatus = random->Read(
                    0, sizeof(readerScratch), options.io_options,
                    &readerResult, readerScratch, &readerDebug);
                if (!readerStatus.ok() || readerResult.compare(payload) != 0) {
                    concurrentReadsOk.store(false);
                    return;
                }
            }
        });
    }
    for (auto &readerThread : readerThreads) {
        readerThread.join();
    }
    if (!concurrentReadsOk.load()) {
        return 11;
    }
    /* A batch reports the first per-request failure while preserving each
     * request's individual status for callers that inspect the array. */
    rocksdb::FSReadRequest mixedRequests[2];
    mixedRequests[0].offset = 0;
    mixedRequests[0].len = sizeof(multiScratchA);
    mixedRequests[0].scratch = multiScratchA;
    mixedRequests[1].offset = 0;
    mixedRequests[1].len = 1;
    mixedRequests[1].scratch = nullptr;
    status = random->MultiRead(mixedRequests, 2, options.io_options, &debug);
    if (status.ok() || !mixedRequests[0].status.ok() || mixedRequests[1].status.ok()) {
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
    std::vector<void *> emptyHandles;
    std::vector<void *> invalidHandles{nullptr};
    if (!fileSystem->Poll(emptyHandles, 0).ok() ||
        fileSystem->Poll(invalidHandles, 1).ok() ||
        !fileSystem->AbortIO(emptyHandles).ok()) {
        deleter(ioHandle);
        return 12;
    }
    std::vector<void *> asyncHandles{ioHandle};
    status = fileSystem->Poll(asyncHandles, 1);
    if (!status.ok() || !callbackCalled.load() || request.result.compare(payload) != 0) {
        deleter(ioHandle);
        return 12;
    }
    deleter(ioHandle);
    /* Abort after callback entry must not rewrite callback-visible status. */
    char boundaryScratch[12]{};
    rocksdb::FSReadRequest boundaryRequest;
    boundaryRequest.offset = 0;
    boundaryRequest.len = sizeof(boundaryScratch);
    boundaryRequest.scratch = boundaryScratch;
    std::atomic<bool> boundaryEntered = false;
    std::atomic<bool> boundaryRelease = false;
    std::atomic<bool> boundaryCallback = false;
    void *boundaryHandle = nullptr;
    rocksdb::IOHandleDeleter boundaryDeleter;
    status = random->ReadAsync(
        boundaryRequest, options.io_options,
        [&](const rocksdb::FSReadRequest &, void *) {
            boundaryEntered.store(true);
            while (!boundaryRelease.load()) {
                std::this_thread::yield();
            }
            boundaryCallback.store(true);
        }, nullptr, &boundaryHandle, &boundaryDeleter, &debug);
    if (!status.ok() || boundaryHandle == nullptr || !boundaryDeleter) {
        boundaryRelease.store(true);
        if (boundaryHandle != nullptr && boundaryDeleter) boundaryDeleter(boundaryHandle);
        return 12;
    }
    for (std::size_t spin = 0; spin < 100000 && !boundaryEntered.load(); ++spin) {
        std::this_thread::yield();
    }
    if (!boundaryEntered.load()) {
        boundaryRelease.store(true);
        boundaryDeleter(boundaryHandle);
        return 12;
    }
    std::vector<void *> boundaryHandles{boundaryHandle};
    if (!fileSystem->AbortIO(boundaryHandles).ok()) {
        boundaryRelease.store(true);
        boundaryDeleter(boundaryHandle);
        return 12;
    }
    boundaryRelease.store(true);
    if (!fileSystem->Poll(boundaryHandles, 1).ok() || !boundaryCallback.load() ||
        !boundaryRequest.status.ok() || boundaryRequest.result.compare(payload) != 0) {
        boundaryDeleter(boundaryHandle);
        return 12;
    }
    boundaryDeleter(boundaryHandle);
    rocksdb::FSReadRequest invalidRequest;
    invalidRequest.offset = 0;
    invalidRequest.len = 1;
    invalidRequest.scratch = nullptr;
    void *invalidHandle = nullptr;
    rocksdb::IOHandleDeleter invalidDeleter;
    if (random->ReadAsync(invalidRequest, options.io_options,
                          [](const rocksdb::FSReadRequest &, void *) {}, nullptr,
                          &invalidHandle, &invalidDeleter, &debug).ok()) {
        return 12;
    }
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
    bool isDirectory = false;
    auto directoryStatus =
        fileSystem->IsDirectory("/rocksdb-dir", options.io_options, &isDirectory, &debug);
    if (!directoryStatus.ok() || !isDirectory) {
        std::fprintf(stderr, "IsDirectory dir failed: %s\n", directoryStatus.ToString().c_str());
        return 14;
    }
    auto regularStatus =
        fileSystem->IsDirectory("/rocksdb-smoke", options.io_options, &isDirectory, &debug);
    if (!regularStatus.ok() || isDirectory) {
        std::fprintf(stderr, "IsDirectory regular failed: %s\n", regularStatus.ToString().c_str());
        return 14;
    }
    auto missingStatus =
        fileSystem->IsDirectory("/rocksdb-missing", options.io_options, &isDirectory, &debug);
    if (!missingStatus.ok() || isDirectory) {
        std::fprintf(stderr, "IsDirectory missing failed: %s\n", missingStatus.ToString().c_str());
        return 14;
    }
    std::unique_ptr<rocksdb::FSDirectory> directory;
    status = fileSystem->NewDirectory("/rocksdb-dir", options.io_options, &directory, &debug);
    if (!status.ok()) {
        return 15;
    }
    status = directory->Fsync(options.io_options, &debug);
    if (!status.ok() ||
        !directory->FsyncWithDirOptions(options.io_options, &debug,
                                        rocksdb::DirFsyncOptions{}).ok()) {
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
    std::printf("free-space reclaim: initial=%llu allocated=%llu final=%llu\n",
                static_cast<unsigned long long>(initialFreeSpace),
                static_cast<unsigned long long>(allocatedFreeSpace),
                static_cast<unsigned long long>(freeSpace));
    return 0;
}
