#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "hfs/file.hpp"
#include "hfs/journal.hpp"
#include "hfs/types.hpp"
#include "metadata/allocator.hpp"
#include "metadata/inode.hpp"
#include "metadata/inode_map.hpp"

namespace hfs::metadata {

/* Limits and memory-budget accounting for the in-memory namespace. */
struct NamespaceConfig {
    std::uint64_t maxInodes = 1'000'000;
    std::uint64_t maxNamespaceBytes = 1ULL << 30;
    /* Memory charged per inode (hfs2.0.md §13.3 target is <=1 KiB per inode). */
    std::uint64_t bytesPerInode = 1024;
    /* Memory charged per visible directory entry (name bytes are extra). */
    std::uint64_t bytesPerDirectoryEntry = 64;
    /* Data placement (Shard-77); empty deviceExtents disables allocation. */
    std::uint64_t extentBytes = 1ULL << 20;
    std::vector<std::uint64_t> deviceExtents;      /* region extents per data device */
    std::vector<std::uint64_t> deviceStartExtents; /* absolute data region base */
    PlacementPolicy placement = PlacementPolicy::CapacityWeighted;
    std::uint64_t maxReserveExtents = 64;
    std::uint64_t capacityOverrideExtents = 0; /* test-only usable-extents cap */
};

/* One reserved-but-not-yet-committed logical extent run. */
struct ExtentAllocation {
    ExtentRun run;
    std::uint64_t logicalExtent = 0;
};

/* Resident in-memory namespace: inode table, directory indexes and the
 * create/open/remove/rename state machine (hfs2.0.md §13.2/§13.3). Durability
 * (journal/checkpoint wiring) lands with the metadata transaction path. */
class Namespace {
public:
    /* Injectable nanosecond clock for deterministic tests. */
    using Clock = std::uint64_t (*)(void *context);

    struct Stats {
        std::uint64_t inodes = 0;        /* live (non-freed) inodes */
        std::uint64_t orphanInodes = 0;  /* unlinked but still open */
        std::uint64_t namespaceBytes = 0;
        std::uint64_t creates = 0;
        std::uint64_t opens = 0;
        std::uint64_t removes = 0;
        std::uint64_t renames = 0;
        std::uint64_t limitRejects = 0; /* NoMemory rejections */
        std::uint64_t allocatedExtents = 0;
        std::uint64_t freedExtents = 0;
    };

    static Result<std::unique_ptr<Namespace>> create(const NamespaceConfig &config,
                                                     Clock clock = nullptr,
                                                     void *clockContext = nullptr);
    ~Namespace();
    Namespace(const Namespace &) = delete;
    Namespace &operator=(const Namespace &) = delete;

    /* Create/open one path component chain. Non-exclusive create of an existing
     * regular file returns its current handle; Truncate resets logical size. */
    Result<FileHandle> create(std::string_view path, bool exclusive, bool truncate,
                              bool directory);
    Result<FileHandle> open(std::string_view path);
    Result<void> remove(std::string_view path);
    /* POSIX-like atomic replace; never exposes an intermediate state. */
    Result<void> rename(std::string_view from, std::string_view to);
    /* Adds a second name for a regular inode without copying its data. */
    Result<void> link(std::string_view from, std::string_view to);
    Result<void> setTimes(FileHandle handle, std::uint64_t mtimeNs, std::uint64_t ctimeNs);
    Result<DirectoryPage> getChildren(std::string_view path, std::string_view cursor,
                                      std::size_t maxEntries) const;
    Result<FileStat> stat(FileHandle handle) const;
    Result<FileStat> stat(std::string_view path) const;
    /* Drops one open reference; frees inodes unlinked while open. */
    Result<void> close(FileHandle handle);
    Stats stats() const;

    /* Reserves extents covering the unmapped parts of [offset, offset+length);
     * callers zero-initialize the runs and then commit (or release). */
    Result<std::vector<ExtentAllocation>> reserveExtents(FileHandle handle,
                                                         std::uint64_t offset,
                                                         std::uint64_t length);
    Result<void> commitExtents(FileHandle handle,
                               std::span<const ExtentAllocation> allocations);
    Result<void> releaseExtents(FileHandle handle,
                                std::span<const ExtentAllocation> allocations);
    Result<std::vector<ExtentDescriptor>> extents(FileHandle handle) const;
    /* Shrinks or grows the logical size; shrinking releases extents beyond. */
    Result<void> truncateFile(FileHandle handle, std::uint64_t size);
    /* Grows the logical size (never shrinks). */
    Result<void> setSize(FileHandle handle, std::uint64_t size);

    /* Epoch state is published only after data flush and metadata transaction
     * work complete (Shard-78). */
    std::uint64_t logicalEpoch() const;
    /* Lock-free invalidation key for immutable frontend snapshots. */
    std::uint64_t logicalEpochFast() const noexcept;
    std::uint64_t durableEpoch() const;
    Result<void> publishDurableEpoch(std::uint64_t epoch);
    /* Stages durable sizes in the snapshot without publishing the epoch yet. */
    Result<void> prepareDurableEpoch(std::uint64_t epoch);
    /* Restores durable sizes when the checkpoint transaction fails. */
    void rollbackPreparedDurableEpoch() noexcept;

    /* Metadata journaling (hfs2.0.md §9.3): mutations stage compact records
     * that the background journal writer appends and the next durable barrier
     * flushes; mutation types without a record force a full checkpoint
     * instead. The barrier and writer run on other threads, so these accessors
     * take the namespace lock. */
    bool checkpointRequired() const;
    void clearPendingRecords();
    /* Falls back to a full checkpoint for mutations without a record. */
    void requireCheckpoint();
    /* One staged batch and the epoch its records cover. */
    struct StagedBatch {
        std::vector<JournalRecord> records;
        std::uint64_t epoch = 0;
    };
    StagedBatch takeStagedRecords();
    /* Applies one replayed journal record to the resident state. */
    Result<void> applyJournalRecord(const JournalRecord &record);

    /* Serializes the complete metadata state for a journal checkpoint. */
    Result<void> saveSnapshot(std::uint64_t txid, std::vector<std::byte> &out);
    /* Restores a checkpoint and drops non-durable open-handle references. */
    Result<void> loadSnapshot(std::span<const std::byte> data);
    std::uint64_t snapshotTxid() const noexcept;

    std::uint64_t extentBytes() const noexcept { return config_.extentBytes; }
    std::uint64_t deviceStartExtent(std::uint32_t deviceId) const noexcept;
    /* Returns the currently allocatable data capacity in bytes. */
    std::uint64_t freeDataBytes() const noexcept;

private:
    Namespace(const NamespaceConfig &config, Clock clock, void *clockContext);

    struct Resolved {
        std::uint64_t parent = 0;
        std::string_view name;
        std::uint64_t inode = 0;
    };

    struct ParentRef {
        std::uint64_t inode = 0;
        std::string_view name;
    };

    /* All helpers run with mutex_ held. */
    std::uint64_t nowNs() const noexcept;
    void touch(Inode &inode, std::uint64_t now, bool content) noexcept;
    /* All of the following run with mutex_ held. */
    void stageRecord(JournalRecord record);
    void requireCheckpointLocked() noexcept {
        checkpointRequired_ = true;
        pendingRecords_.clear();
        pendingEpoch_ = 0;
    }
    Inode *inodeById(std::uint64_t inodeId) noexcept;
    /* Shared by truncateFile and journal replay; mutex_ held by the caller. */
    Result<void> applyTruncateLocked(Inode &inode, std::uint64_t size);
    Result<FileHandle> makeHandle(std::uint64_t inodeId) const;
    Result<Inode *> lookupInode(FileHandle handle) noexcept;
    Result<const Inode *> lookupInode(FileHandle handle) const noexcept;
    Result<std::uint64_t> resolveDirectory(std::string_view path) const;
    Result<ParentRef> resolveParent(std::string_view path) const;
    Result<Resolved> resolveEntry(std::string_view path) const;
    Result<Inode *> createInode(bool directory, std::uint64_t parent,
                                std::uint64_t now) noexcept;
    void freeInode(std::uint64_t inodeId) noexcept;
    void linkEntry(Inode &parent, std::string_view name, std::uint64_t inodeId);
    std::uint64_t unlinkEntry(Inode &parent, std::string_view name);
    bool isAncestor(const Inode &maybeAncestor, std::uint64_t inodeId) const noexcept;
    std::uint64_t entryBytes(std::string_view name) const noexcept;
    void releaseDescriptor(const ExtentDescriptor &descriptor) noexcept;
    static Result<std::vector<std::string_view>> splitPath(std::string_view path);

    mutable std::mutex mutex_;
    NamespaceConfig config_;
    Clock clock_ = nullptr;
    void *clockContext_ = nullptr;
    std::vector<Inode> inodes_; /* slot 0 unused, slot 1 is the root directory */
    std::vector<std::uint64_t> freeSlots_;
    std::uint64_t namespaceBytes_ = 0;
    std::uint64_t logicalEpoch_ = 0;
    std::atomic<std::uint64_t> logicalEpochPublished_{0};
    std::uint64_t durableEpoch_ = 0;
    std::uint64_t snapshotTxid_ = 0;
    std::vector<std::uint64_t> preparedDurableSizes_;
    /* Staged metadata records, their epoch, the writer wake-up hook and the
     * no-record fallback flag (§9.3). */
    std::vector<JournalRecord> pendingRecords_;
    std::uint64_t pendingEpoch_ = 0;
    bool checkpointRequired_ = false;
    Stats stats_;
    std::vector<std::unique_ptr<Allocator>> allocators_;
    PlacementPicker picker_;
};

} // namespace hfs::metadata
