#pragma once

#include <array>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#include "common/utils/Result.h"
#include "fbs/storage/Common.h"
#include "storage/store/ChunkMetadata.h"

namespace uuse {
class Client;
class Engine;
class File;
struct CompletionToken;
}  // namespace uuse

namespace hf3fs::storage {

/* One append-only journal record (72 bytes, little endian, CRC32C tail). */
struct UuseJournalRecord {
  uint32_t magic;
  uint8_t op;
  uint8_t chunkIdLen;
  uint16_t reserved;
  uint8_t chunkId[16];
  uint32_t commitVer;
  uint32_t updateVer;
  uint32_t chainVer;
  uint32_t size;
  uint8_t chunkState;
  uint8_t checksumType;
  uint16_t reserved2;
  uint32_t checksumValue;
  uint32_t fileId;
  uint64_t innerOffset;
  uint32_t recordCrc;
} __attribute__((packed));

/* UUSE-backed local chunk store (design M2.1).
 *
 * Layout: per chunk-size class (64 KiB / 512 KiB / 4 MiB) two fixed slot files
 * where slot N lives at offset N * classSize, plus an append-only metadata
 * journal. `load` replays the journal, so a process restart recovers the store.
 * M2.1 runs the data plane synchronously (stepping stone to the async client). */
class UuseChunkStore {
 public:
  struct SlotRef {
    uint32_t fileId = 0;
    uint32_t slot = 0;
  };

  struct ChunkInfo {
    ChunkMetadata meta;
    SlotRef slot;
  };

  struct Options {
    std::string uuseConfPath; /* UUSE .conf for the target device set */
    std::string targetDir;    /* UUSE namespace directory for this target */
    uint64_t targetId = 0;
    bool create = false;
  };

  /* Input of one update (mirrors the UpdateIO fields the storage service
   * passes to ChunkReplica::update). */
  struct UpdateInput {
    ChunkId chunkId;
    uint32_t chunkSize = 0;
    uint32_t offset = 0;
    uint32_t length = 0;
    uint64_t updateVer = 0; /* 0 = auto-increment */
    uint32_t chainVer = 0;
    const uint8_t *data = nullptr;
    ChecksumInfo checksum;
    bool isSyncing = false; /* commit the write in this request */
    bool isRemove = false;
    bool isTruncate = false;
    bool isExtend = false;
  };

  struct UpdateOutput {
    ChunkVer commitVer;
    ChunkVer updateVer;
    ChainVer chainVer;
    ChecksumInfo checksum;
    uint32_t length = 0;
  };

  static constexpr uint32_t kClassCount = 4;
  /* Slot files per chunk-size class; the 1 MiB class (3FS default chunk size)
   * gets the largest share. */
  static constexpr uint32_t kClassFiles[kClassCount] = {8, 8, 24, 8};
  static constexpr uint32_t kSlotsPerFile = 128;
  static constexpr uint64_t kClassSize[kClassCount] = {64u << 10, 512u << 10, 1u << 20, 4u << 20};
  static constexpr uint32_t kFileCount = 8 + 8 + 24 + 8;

  static Result<std::unique_ptr<UuseChunkStore>> open(const Options &options);
  ~UuseChunkStore();

  UuseChunkStore(const UuseChunkStore &) = delete;
  UuseChunkStore &operator=(const UuseChunkStore &) = delete;

  /* Metadata lookup; error StorageCode::kChunkMetadataNotFound when absent. */
  Result<ChunkInfo> get(const ChunkId &chunkId);

  /* Allocates a fresh slot for a new or COW-overwritten chunk (not persisted
   * until set()). */
  Result<ChunkInfo> createChunk(const ChunkId &chunkId, uint32_t chunkSize);

  /* Slot data I/O through UUSE. */
  Result<Void> writeChunk(ChunkInfo &chunkInfo, const uint8_t *data, uint32_t size, uint32_t offset);
  Result<Void> readChunk(const ChunkInfo &chunkInfo, uint8_t *data, uint32_t size, uint32_t offset);

  /* Persists metadata (journal append); a changed slot releases the old one. */
  Result<Void> set(const ChunkId &chunkId, const ChunkInfo &chunkInfo);

  /* Journal delete + slot release. */
  Result<Void> remove(const ChunkId &chunkId, ChunkInfo &chunkInfo);

  /* Physically removes chunks marked REMOVAL_IN_PROGRESS and releases their
   * slots (called by the storage PunchHoleWorker). Returns true when no
   * removable chunk remains. */
  Result<bool> punchHole();

  /* ChunkReplica::update semantics on UUSE (M2.2 step 1): version checks,
   * COW allocation for new chunks, gap zero-fill, checksum verification and
   * full-chunk CRC32C recompute, optional commit in the same request. */
  Result<UpdateOutput> update(const UpdateInput &input);

  /* Commit an update version (commitVer/chainVer) and persist it.
   * Idempotent: a missing chunk is treated as already removed (returns success),
   * matching the stock engines and avoiding GC remove-retry loops. */
  Result<Void> commit(const ChunkId &chunkId, uint64_t commitVer, uint32_t chainVer);

  /* Chunks inside [range.begin, range.end], reverse lexicographic order. */
  Result<std::vector<std::pair<ChunkId, ChunkMetadata>>> queryChunks(const ChunkIdRange &range) const;
  std::vector<std::pair<ChunkId, ChunkMetadata>> allChunks() const;

  uint64_t usedSize() const;
  std::vector<ChunkId> uncommitted() const;
  Result<Void> resetUncommitted(ChainVer chainVer);
  Result<Void> sync();

  std::size_t liveChunks() const;
  std::size_t freeSlotCount() const;

 private:
  UuseChunkStore() = default;

  Result<Void> createLayout();
  Result<Void> openFiles();
  Result<Void> replay();
  void applyRecord(const UuseJournalRecord &record);
  void rebuildFreeSlots();
  /* Deletes chunks marked for removal and releases their slots; caller must
   * hold mutex_. Returns true when no removable chunk remains. */
  Result<bool> recycleRemovedLocked();
  Result<Void> appendRecord(const UuseJournalRecord &record);
  Result<SlotRef> allocateSlot(std::size_t cls);
  void releaseSlot(const SlotRef &slot);
  static std::size_t classOfFile(uint32_t fileId);
  static SlotRef slotRefOf(const ChunkMetadata &meta);
  std::string slotPath(uint32_t fileId) const;
  std::string metaPath() const;

  /* All UUSE sync-facade calls (journal append, sync, fallback read/write) are
   * serialized on one dedicated I/O thread: the UUSE sync API is not safe for
   * concurrent use from multiple storage worker threads. The data fast path
   * below submits through per-thread clients instead. */
  void startIoThread();
  void stopIoThread();
  Result<Void> runIo(std::function<Result<Void>()> fn);

  /* Worker-direct async fast path (uuse.md §20.4 / Shard-106): the owner
   * client holds the open Files, each calling thread submits through its own
   * per-thread client and polls completions on that thread. Falls back to the
   * serialized sync facade when the async frontend is unavailable. */
  struct SubmissionClient {
    std::shared_ptr<uuse::Client> client;
    std::atomic<uint32_t> tokenSeq{1};
  };
  std::shared_ptr<SubmissionClient> submissionClient();
  Result<Void> asyncWriteAt(const std::shared_ptr<uuse::File> &file, const uint8_t *data, uint32_t size,
                            uint64_t offset);
  Result<Void> asyncReadAt(const std::shared_ptr<uuse::File> &file, uint8_t *data, uint32_t size,
                           uint64_t offset);
  Result<Void> waitForCompletion(SubmissionClient &submission, const uuse::CompletionToken &token,
                                 uint64_t expectedBytes);
  Result<Void> syncWriteAt(uuse::File &file, const uint8_t *data, uint32_t size, uint64_t offset);
  Result<Void> syncReadAt(uuse::File &file, uint8_t *data, uint32_t size, uint64_t offset);
  Result<Void> ioSyncFile(uuse::File &file, const char *what);

  struct IoTask {
    std::function<Result<Void>()> fn;
    Result<Void> result = makeError(StatusCode::kInvalidArg, "io task not run");
    bool done = false;
  };
  std::thread ioThread_;
  std::mutex ioMutex_;
  std::condition_variable ioCv_;
  std::condition_variable ioDoneCv_;
  std::deque<IoTask *> ioQueue_;
  bool ioStop_ = false;

  /* Per-thread submission clients (kept alive until the store is destroyed). */
  std::mutex clientsMutex_;
  std::unordered_map<std::thread::id, std::shared_ptr<SubmissionClient>> submissionClients_;

  Options options_;
  std::unique_ptr<uuse::Engine> engine_;
  std::shared_ptr<uuse::Client> client_;
  std::shared_ptr<uuse::File> metaFile_;
  std::unordered_map<uint32_t, std::shared_ptr<uuse::File>> slotFiles_;
  std::unordered_map<std::string, ChunkInfo> chunks_;
  std::array<std::vector<SlotRef>, kClassCount> freeSlots_;
  mutable std::mutex mutex_;
};

}  // namespace hf3fs::storage
