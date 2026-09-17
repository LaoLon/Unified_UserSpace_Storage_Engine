/* HFS 2.0 RocksDB 8.9.1 buffered FileSystem adapter. */

#include "rocksdb/hfs_filesystem.hpp"

#include <cerrno>
#include <cstdint>
#include <fcntl.h>
#include <memory>
#include <mutex>
#include <string>
#include <sys/file.h>
#include <unistd.h>
#include <utility>
#include <vector>

#include "hfs/config.hpp"
#include "hfs/core.hpp"
#include "hfs/file.hpp"
#include "rocksdb/file_system.h"

namespace hfs::rocksdb_adapter {

namespace {

/* Maps an HFS status into the status vocabulary expected by RocksDB. */
template <typename T>
rocksdb::IOStatus toIoStatus(const hfs::Result<T> &result) {
    if (result.ok()) {
        return rocksdb::IOStatus::OK();
    }
    switch (result.status()) {
    case hfs::Status::NotFound:
        return rocksdb::IOStatus::NotFound(result.message());
    case hfs::Status::AlreadyExists:
        return rocksdb::IOStatus::IOError(result.message());
    case hfs::Status::Busy:
    case hfs::Status::TryAgain:
        return rocksdb::IOStatus::IOError(result.message());
    case hfs::Status::InvalidArgument:
        return rocksdb::IOStatus::InvalidArgument(result.message());
    case hfs::Status::NotSupported:
        return rocksdb::IOStatus::NotSupported(result.message());
    default:
        return rocksdb::IOStatus::IOError(result.message());
    }
}

/* Converts a path while preserving HFS's absolute namespace contract. */
std::string hfsPath(const std::string &path) {
    std::string normalized = path;
    if (normalized.empty()) {
        return normalized;
    }
    if (normalized.front() != '/') {
        normalized.insert(normalized.begin(), '/');
    }
    while (normalized.size() > 1 && normalized.back() == '/') {
        normalized.pop_back();
    }
    return normalized;
}

/* Builds a stable host-side lock path for a database in the HFS namespace. */
std::string hostLockPath(const std::string &path) {
    std::uint64_t hash = 1469598103934665603ULL;
    for (unsigned char byte : path) {
        hash ^= byte;
        hash *= 1099511628211ULL;
    }
    return "/var/tmp/hfs-rocksdb-lock-" + std::to_string(hash);
}

struct HfsContext {
    std::shared_ptr<hfs::Engine> engine;
    std::shared_ptr<hfs::Client> client;
};

/* Opens one HFS file and returns the nested coroutine result consistently. */
rocksdb::IOStatus openFile(const std::shared_ptr<HfsContext> &context,
                           const std::string &path, hfs::OpenFlags flags,
                           std::shared_ptr<hfs::File> *file) {
    if (context == nullptr || context->client == nullptr) {
        return rocksdb::IOStatus::InvalidArgument("HFS context is not initialized");
    }
    auto outer = context->client->open(hfsPath(path), flags).wait();
    if (!outer.ok()) {
        return toIoStatus(outer);
    }
    auto inner = std::move(outer).value();
    if (!inner.ok()) {
        return toIoStatus(inner);
    }
    *file = std::move(inner).value();
    return rocksdb::IOStatus::OK();
}

class HfsSequentialFile final : public rocksdb::FSSequentialFile {
public:
    HfsSequentialFile(std::shared_ptr<hfs::File> file) : file_(std::move(file)) {}

    /* Reads from the current sequential offset into RocksDB scratch storage. */
    rocksdb::IOStatus Read(size_t n, const rocksdb::IOOptions &, rocksdb::Slice *result,
                           char *scratch, rocksdb::IODebugContext *) override {
        if (scratch == nullptr && n != 0) {
            return rocksdb::IOStatus::InvalidArgument("null sequential read buffer");
        }
        hfs::IoVec buffer{reinterpret_cast<std::byte *>(scratch), n};
        auto read = file_->read(std::span<hfs::IoVec>(&buffer, 1), offset_).wait();
        if (!read.ok()) {
            return toIoStatus(read);
        }
        auto value = std::move(read).value();
        if (!value.ok()) {
            return toIoStatus(value);
        }
        offset_ += value.value().bytes;
        *result = rocksdb::Slice(scratch, value.value().bytes);
        return rocksdb::IOStatus::OK();
    }

    /* Advances the sequential cursor without reading user-visible data. */
    rocksdb::IOStatus Skip(uint64_t n) override {
        if (offset_ > UINT64_MAX - n) {
            return rocksdb::IOStatus::InvalidArgument("sequential offset overflow");
        }
        offset_ += n;
        return rocksdb::IOStatus::OK();
    }

private:
    std::shared_ptr<hfs::File> file_;
    std::uint64_t offset_ = 0;
};

class HfsRandomAccessFile final : public rocksdb::FSRandomAccessFile {
public:
    HfsRandomAccessFile(std::shared_ptr<hfs::File> file) : file_(std::move(file)) {}

    /* Performs a buffered random read through the HFS File API. */
    rocksdb::IOStatus Read(uint64_t offset, size_t n, const rocksdb::IOOptions &,
                           rocksdb::Slice *result, char *scratch,
                           rocksdb::IODebugContext *) const override {
        if (scratch == nullptr && n != 0) {
            return rocksdb::IOStatus::InvalidArgument("null random read buffer");
        }
        hfs::IoVec buffer{reinterpret_cast<std::byte *>(scratch), n};
        auto read = file_->read(std::span<hfs::IoVec>(&buffer, 1), offset).wait();
        if (!read.ok()) {
            return toIoStatus(read);
        }
        auto value = std::move(read).value();
        if (!value.ok()) {
            return toIoStatus(value);
        }
        *result = rocksdb::Slice(scratch, value.value().bytes);
        return rocksdb::IOStatus::OK();
    }

private:
    std::shared_ptr<hfs::File> file_;
};

class HfsWritableFile final : public rocksdb::FSWritableFile {
public:
    HfsWritableFile(std::shared_ptr<hfs::File> file) : file_(std::move(file)) {}

    /* Appends buffered-mode bytes at the current HFS logical EOF. */
    rocksdb::IOStatus Append(const rocksdb::Slice &data, const rocksdb::IOOptions &,
                             rocksdb::IODebugContext *) override {
        hfs::IoVec buffer{reinterpret_cast<std::byte *>(const_cast<char *>(data.data())),
                          data.size()};
        auto written = file_->append(std::span<const hfs::IoVec>(&buffer, 1)).wait();
        if (!written.ok()) {
            return toIoStatus(written);
        }
        auto value = std::move(written).value();
        return value.ok() ? rocksdb::IOStatus::OK() : toIoStatus(value);
    }

    /* Closes the HFS handle; there is no host descriptor to flush. */
    rocksdb::IOStatus Close(const rocksdb::IOOptions &, rocksdb::IODebugContext *) override {
        file_.reset();
        return rocksdb::IOStatus::OK();
    }

    /* Buffered writes are immediately submitted by the current core API. */
    rocksdb::IOStatus Flush(const rocksdb::IOOptions &, rocksdb::IODebugContext *) override {
        return rocksdb::IOStatus::OK();
    }

    /* Persists user data and file metadata through the HFS durable epoch path. */
    rocksdb::IOStatus Sync(const rocksdb::IOOptions &, rocksdb::IODebugContext *) override {
        auto synced = file_->sync().wait();
        return synced.ok() ? toIoStatus(std::move(synced).value()) : toIoStatus(synced);
    }

private:
    std::shared_ptr<hfs::File> file_;
};

class HfsDirectory final : public rocksdb::FSDirectory {
public:
    HfsDirectory(std::shared_ptr<hfs::File> file) : file_(std::move(file)) {}

    /* Commits directory metadata through the HFS durable epoch path. */
    rocksdb::IOStatus Fsync(const rocksdb::IOOptions &, rocksdb::IODebugContext *) override {
        auto synced = file_->sync().wait();
        return synced.ok() ? toIoStatus(std::move(synced).value()) : toIoStatus(synced);
    }

private:
    std::shared_ptr<hfs::File> file_;
};

struct HfsFileLock final : public rocksdb::FileLock {
    int fd = -1;
};

class HfsFileSystem final : public rocksdb::FileSystem {
public:
    explicit HfsFileSystem(std::shared_ptr<HfsContext> context)
        : context_(std::move(context)) {}

    /* Identifies this implementation to RocksDB's Customizable registry. */
    const char *Name() const override { return "HfsFileSystem"; }

    /* Do not advertise async I/O until the core reactor ownership contract is complete. */
    void SupportedOps(int64_t &supported_ops) override { supported_ops = 0; }

    /* Opens an existing HFS regular file for sequential reads. */
    rocksdb::IOStatus NewSequentialFile(const std::string &fname,
                                         const rocksdb::FileOptions &,
                                         std::unique_ptr<rocksdb::FSSequentialFile> *result,
                                         rocksdb::IODebugContext *) override {
        std::shared_ptr<hfs::File> file;
        auto status = openFile(context_, fname, hfs::OpenFlags::None, &file);
        if (!status.ok()) return status;
        *result = std::make_unique<HfsSequentialFile>(std::move(file));
        return rocksdb::IOStatus::OK();
    }

    /* Opens an existing HFS regular file for concurrent random reads. */
    rocksdb::IOStatus NewRandomAccessFile(
        const std::string &fname, const rocksdb::FileOptions &,
        std::unique_ptr<rocksdb::FSRandomAccessFile> *result,
        rocksdb::IODebugContext *) override {
        std::shared_ptr<hfs::File> file;
        auto status = openFile(context_, fname, hfs::OpenFlags::None, &file);
        if (!status.ok()) return status;
        *result = std::make_unique<HfsRandomAccessFile>(std::move(file));
        return rocksdb::IOStatus::OK();
    }

    /* Creates or replaces a buffered HFS writable file. */
    rocksdb::IOStatus NewWritableFile(
        const std::string &fname, const rocksdb::FileOptions &,
        std::unique_ptr<rocksdb::FSWritableFile> *result,
        rocksdb::IODebugContext *) override {
        std::shared_ptr<hfs::File> file;
        auto status = openFile(context_, fname,
                               hfs::OpenFlags::Create | hfs::OpenFlags::Truncate, &file);
        if (!status.ok()) return status;
        *result = std::make_unique<HfsWritableFile>(std::move(file));
        return rocksdb::IOStatus::OK();
    }

    /* Opens a resident HFS directory handle. */
    rocksdb::IOStatus NewDirectory(const std::string &name, const rocksdb::IOOptions &,
                                    std::unique_ptr<rocksdb::FSDirectory> *result,
                                    rocksdb::IODebugContext *) override {
        std::shared_ptr<hfs::File> file;
        auto status = openFile(context_, name, hfs::OpenFlags::Directory, &file);
        if (!status.ok()) return status;
        *result = std::make_unique<HfsDirectory>(std::move(file));
        return rocksdb::IOStatus::OK();
    }

    /* Checks existence without using a host filesystem descriptor. */
    rocksdb::IOStatus FileExists(const std::string &fname, const rocksdb::IOOptions &,
                                 rocksdb::IODebugContext *) override {
        std::shared_ptr<hfs::File> file;
        return openFile(context_, fname, hfs::OpenFlags::None, &file);
    }

    /* Lists one HFS directory page and continues until all children are seen. */
    rocksdb::IOStatus GetChildren(const std::string &dir, const rocksdb::IOOptions &,
                                  std::vector<std::string> *result,
                                  rocksdb::IODebugContext *) override {
        result->clear();
        std::string cursor;
        do {
            auto page = context_->client->getChildren(hfsPath(dir), cursor, 4096).wait();
            if (!page.ok()) return toIoStatus(page);
            auto value = std::move(page).value();
            if (!value.ok()) return toIoStatus(value);
            for (const auto &entry : value.value().entries) result->push_back(entry.name);
            cursor = value.value().nextCursor;
        } while (!cursor.empty());
        return rocksdb::IOStatus::OK();
    }

    /* Removes a regular HFS namespace entry. */
    rocksdb::IOStatus DeleteFile(const std::string &fname, const rocksdb::IOOptions &,
                                 rocksdb::IODebugContext *) override {
        auto removed = context_->client->remove(hfsPath(fname)).wait();
        return removed.ok() ? toIoStatus(std::move(removed).value()) : toIoStatus(removed);
    }

    /* Creates one HFS directory entry and rejects an existing name. */
    rocksdb::IOStatus CreateDir(const std::string &dirname, const rocksdb::IOOptions &,
                                rocksdb::IODebugContext *) override {
        std::shared_ptr<hfs::File> file;
        return openFile(context_, dirname,
                        hfs::OpenFlags::Create | hfs::OpenFlags::Exclusive |
                            hfs::OpenFlags::Directory,
                        &file);
    }

    /* Creates a directory or accepts an already existing directory only. */
    rocksdb::IOStatus CreateDirIfMissing(const std::string &dirname,
                                         const rocksdb::IOOptions &options,
                                         rocksdb::IODebugContext *dbg) override {
        std::shared_ptr<hfs::File> existing;
        auto status = openFile(context_, dirname, hfs::OpenFlags::Directory, &existing);
        if (status.ok()) {
            return rocksdb::IOStatus::OK();
        }
        if (!status.IsNotFound()) {
            return status;
        }
        return CreateDir(dirname, options, dbg);
    }

    /* Removes an empty HFS directory entry. */
    rocksdb::IOStatus DeleteDir(const std::string &dirname, const rocksdb::IOOptions &,
                                rocksdb::IODebugContext *) override {
        auto removed = context_->client->remove(hfsPath(dirname)).wait();
        return removed.ok() ? toIoStatus(std::move(removed).value()) : toIoStatus(removed);
    }

    /* Returns the logical HFS size, not a host-file size. */
    rocksdb::IOStatus GetFileSize(const std::string &fname, const rocksdb::IOOptions &,
                                  uint64_t *fileSize, rocksdb::IODebugContext *) override {
        std::shared_ptr<hfs::File> file;
        auto status = openFile(context_, fname, hfs::OpenFlags::None, &file);
        if (!status.ok()) return status;
        auto stat = file->stat();
        if (!stat.ok()) return rocksdb::IOStatus::IOError(stat.message());
        *fileSize = stat.value().logicalSize;
        return rocksdb::IOStatus::OK();
    }

    /* Reports HFS's namespace modification timestamp. */
    rocksdb::IOStatus GetFileModificationTime(const std::string &fname,
                                               const rocksdb::IOOptions &, uint64_t *mtime,
                                               rocksdb::IODebugContext *) override {
        std::shared_ptr<hfs::File> file;
        auto status = openFile(context_, fname, hfs::OpenFlags::None, &file);
        if (!status.ok()) return status;
        auto stat = file->stat();
        if (!stat.ok()) return rocksdb::IOStatus::IOError(stat.message());
        *mtime = stat.value().mtimeNs;
        return rocksdb::IOStatus::OK();
    }

    /* Performs an atomic HFS namespace rename. */
    rocksdb::IOStatus RenameFile(const std::string &src, const std::string &target,
                                 const rocksdb::IOOptions &, rocksdb::IODebugContext *) override {
        auto renamed = context_->client->rename(hfsPath(src), hfsPath(target)).wait();
        return renamed.ok() ? toIoStatus(std::move(renamed).value()) : toIoStatus(renamed);
    }

    /* Creates a native HFS hard link without duplicating file contents. */
    rocksdb::IOStatus LinkFile(const std::string &src, const std::string &target,
                               const rocksdb::IOOptions &, rocksdb::IODebugContext *) override {
        auto linked = context_->client->link(hfsPath(src), hfsPath(target)).wait();
        return linked.ok() ? toIoStatus(std::move(linked).value()) : toIoStatus(linked);
    }

    /* Reports the shared inode link count for a regular HFS file. */
    rocksdb::IOStatus NumFileLinks(const std::string &fname, const rocksdb::IOOptions &,
                                   uint64_t *count, rocksdb::IODebugContext *) override {
        std::shared_ptr<hfs::File> file;
        auto status = openFile(context_, fname, hfs::OpenFlags::None, &file);
        if (!status.ok()) return status;
        auto stat = file->stat();
        if (!stat.ok()) return toIoStatus(stat);
        *count = stat.value().linkCount;
        return rocksdb::IOStatus::OK();
    }

    /* Compares stable inode identity rather than path strings. */
    rocksdb::IOStatus AreFilesSame(const std::string &first, const std::string &second,
                                   const rocksdb::IOOptions &, bool *same,
                                   rocksdb::IODebugContext *) override {
        std::shared_ptr<hfs::File> left;
        std::shared_ptr<hfs::File> right;
        auto leftStatus = openFile(context_, first, hfs::OpenFlags::None, &left);
        if (!leftStatus.ok()) return leftStatus;
        auto rightStatus = openFile(context_, second, hfs::OpenFlags::None, &right);
        if (!rightStatus.ok()) return rightStatus;
        auto leftStat = left->stat();
        auto rightStat = right->stat();
        if (!leftStat.ok()) return toIoStatus(leftStat);
        if (!rightStat.ok()) return toIoStatus(rightStat);
        *same = leftStat.value().inodeId == rightStat.value().inodeId &&
                leftStat.value().generation == rightStat.value().generation;
        return rocksdb::IOStatus::OK();
    }

    /* Acquires a process lock for RocksDB's DB identity. */
    rocksdb::IOStatus LockFile(const std::string &fname, const rocksdb::IOOptions &,
                               rocksdb::FileLock **lock, rocksdb::IODebugContext *) override {
        auto *value = new HfsFileLock;
        value->fd = ::open(hostLockPath(fname).c_str(), O_CREAT | O_RDWR | O_CLOEXEC, 0644);
        if (value->fd < 0 || ::flock(value->fd, LOCK_EX | LOCK_NB) != 0) {
            if (value->fd >= 0) ::close(value->fd);
            delete value;
            return rocksdb::IOStatus::IOError("RocksDB lock is already held");
        }
        *lock = value;
        return rocksdb::IOStatus::OK();
    }

    /* Releases only locks returned by this adapter. */
    rocksdb::IOStatus UnlockFile(rocksdb::FileLock *lock, const rocksdb::IOOptions &,
                                 rocksdb::IODebugContext *) override {
        auto *value = static_cast<HfsFileLock *>(lock);
        const int rc = value == nullptr ? -1 : ::close(value->fd);
        delete value;
        return rc == 0 ? rocksdb::IOStatus::OK()
                       : rocksdb::IOStatus::IOError("RocksDB lock close failed");
    }

    /* Exposes a stable HFS test namespace path. */
    rocksdb::IOStatus GetTestDirectory(const rocksdb::IOOptions &, std::string *path,
                                       rocksdb::IODebugContext *) override {
        *path = "/rocksdb-test";
        return rocksdb::IOStatus::OK();
    }

    /* HFS paths are already canonical absolute namespace paths. */
    rocksdb::IOStatus GetAbsolutePath(const std::string &dbPath, const rocksdb::IOOptions &,
                                      std::string *output, rocksdb::IODebugContext *) override {
        *output = hfsPath(dbPath);
        return rocksdb::IOStatus::OK();
    }

    /* Reports aggregate configured device capacity as the buffered free-space bound. */
    rocksdb::IOStatus GetFreeSpace(const std::string &, const rocksdb::IOOptions &,
                                   uint64_t *freeSpace, rocksdb::IODebugContext *) override {
        *freeSpace = 0;
        for (std::size_t i = 0; i < context_->engine->deviceGeometries().size(); ++i) {
            *freeSpace += context_->engine->deviceCapacityBytes(i);
        }
        return rocksdb::IOStatus::OK();
    }

    /* Distinguishes HFS directory handles from regular file handles. */
    rocksdb::IOStatus IsDirectory(const std::string &path, const rocksdb::IOOptions &,
                                   bool *isDirectory, rocksdb::IODebugContext *) override {
        std::shared_ptr<hfs::File> file;
        auto status = openFile(context_, path, hfs::OpenFlags::Directory, &file);
        if (status.ok()) {
            *isDirectory = true;
            return rocksdb::IOStatus::OK();
        }
        *isDirectory = false;
        return rocksdb::IOStatus::OK();
    }

private:
    std::shared_ptr<HfsContext> context_;
};

} // namespace

/* Builds and mounts one shared HFS engine for the RocksDB FileSystem. */
std::shared_ptr<rocksdb::FileSystem>
createFileSystem(const std::string &configPath) {
    auto config = hfs::parseConfigFile(configPath);
    if (!config.ok()) return nullptr;
    auto engine = hfs::Engine::create(config.value());
    if (!engine.ok()) return nullptr;
    auto owned = std::shared_ptr<hfs::Engine>(std::move(engine).value());
    auto mounted = owned->mount().wait();
    if (!mounted.ok() || !mounted.value().ok()) return nullptr;
    auto client = hfs::Client::create(*owned);
    if (!client.ok()) return nullptr;
    auto context = std::make_shared<HfsContext>();
    context->engine = std::move(owned);
    context->client = std::move(client).value();
    return std::make_shared<HfsFileSystem>(std::move(context));
}

/* Injects HFS into DBOptions::env without transferring raw ownership to HFS. */
std::unique_ptr<rocksdb::Env> createCompositeEnv(const std::string &configPath) {
    auto fileSystem = createFileSystem(configPath);
    if (fileSystem == nullptr) {
        return nullptr;
    }
    return rocksdb::NewCompositeEnv(fileSystem);
}

} // namespace hfs::rocksdb_adapter
