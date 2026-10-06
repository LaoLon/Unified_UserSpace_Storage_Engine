#include "storage/store/UuseChunkStore.h"

#include <algorithm>
#include <cstring>
#include <set>
#include <span>
#include <stop_token>
#include <string>

#include <folly/logging/xlog.h>

#include "uuse/config.hpp"
#include "uuse/core.hpp"
#include "uuse/crc.hpp"
#include "uuse/file.hpp"

namespace hf3fs::storage {
namespace {

constexpr uint32_t kRecordMagic = 0x55535243u; /* 'USRC' */
constexpr uint8_t kOpPut = 1;
constexpr uint8_t kOpDelete = 2;
constexpr std::size_t kRecordSize = sizeof(UuseJournalRecord);

std::size_t classForSize(uint32_t size) {
  for (std::size_t i = 0; i < UuseChunkStore::kClassCount; ++i) {
    if (size <= UuseChunkStore::kClassSize[i]) {
      return i;
    }
  }
  return UuseChunkStore::kClassCount;
}

Result<Void> writeAt(uuse::File &file, const uint8_t *data, uint32_t size, uint64_t offset) {
  uuse::IoVec vec{reinterpret_cast<std::byte *>(const_cast<uint8_t *>(data)), size};
  auto result = file.write(std::span<const uuse::IoVec>(&vec, 1), offset, std::stop_token{}).wait().value();
  if (!result.ok()) {
    return makeError(StatusCode::kIOError, "uuse write: " + result.message());
  }
  if (result.value().bytes != size) {
    return makeError(StatusCode::kIOError, "uuse short write");
  }
  return Void{};
}

Result<Void> readAt(uuse::File &file, uint8_t *data, uint32_t size, uint64_t offset) {
  uuse::IoVec vec{reinterpret_cast<std::byte *>(data), size};
  auto result = file.read(std::span<uuse::IoVec>(&vec, 1), offset, std::stop_token{}).wait().value();
  if (!result.ok()) {
    return makeError(StatusCode::kIOError, "uuse read: " + result.message());
  }
  if (result.value().bytes != size) {
    return makeError(StatusCode::kIOError, "uuse short read");
  }
  return Void{};
}

UuseJournalRecord makeRecord(uint8_t op, const ChunkId &chunkId, const UuseChunkStore::ChunkInfo &info) {
  UuseJournalRecord record{};
  record.magic = kRecordMagic;
  record.op = op;
  record.chunkIdLen = 16;
  std::memcpy(record.chunkId, chunkId.data().data(), 16);
  record.commitVer = info.meta.commitVer.toUnderType();
  record.updateVer = info.meta.updateVer.toUnderType();
  record.chainVer = info.meta.chainVer.toUnderType();
  record.size = info.meta.size;
  record.chunkState = static_cast<uint8_t>(info.meta.chunkState);
  record.checksumType = static_cast<uint8_t>(info.meta.checksumType);
  record.reserved2 = static_cast<uint16_t>(info.meta.recycleState);
  record.checksumValue = info.meta.checksumValue;
  record.fileId = info.slot.fileId;
  record.innerOffset = info.meta.innerOffset;
  record.recordCrc = uuse::crc32c(&record, kRecordSize - 4);
  return record;
}

}  // namespace

Result<std::unique_ptr<UuseChunkStore>> UuseChunkStore::open(const Options &options) {
  auto parsed = uuse::parseConfigFile(options.uuseConfPath);
  if (!parsed.ok()) {
    return makeError(StatusCode::kInvalidArg, "uuse config: " + parsed.message());
  }
  auto store = std::unique_ptr<UuseChunkStore>(new UuseChunkStore());
  store->options_ = options;

  auto created = uuse::Engine::create(parsed.value());
  if (!created.ok()) {
    return makeError(StatusCode::kInvalidArg, "uuse engine: " + created.message());
  }
  store->engine_ = std::move(created).value();

  if (options.create) {
    auto formatted = store->engine_->format({true}).wait().value();
    if (!formatted.ok()) {
      return makeError(StatusCode::kIOError, "uuse format: " + formatted.message());
    }
  }
  auto mounted = store->engine_->mount().wait().value();
  if (!mounted.ok()) {
    return makeError(StatusCode::kIOError, "uuse mount: " + mounted.message());
  }
  auto clientResult = uuse::Client::create(*store->engine_, uuse::ClientOptions{});
  if (!clientResult.ok()) {
    return makeError(StatusCode::kIOError, "uuse client: " + clientResult.message());
  }
  store->client_ = clientResult.value();
  store->startIoThread();

  if (options.create) {
    auto layout = store->createLayout();
    if (!layout) {
      return folly::makeUnexpected(layout.error());
    }
  }
  auto opened = store->openFiles();
  if (!opened) {
    return folly::makeUnexpected(opened.error());
  }
  auto replayed = store->replay();
  if (!replayed) {
    return folly::makeUnexpected(replayed.error());
  }
  store->rebuildFreeSlots();
  if (options.create) {
    /* Namespace changes are durable only after a metadata checkpoint. */
    auto synced = store->sync();
    if (!synced) {
      return folly::makeUnexpected(synced.error());
    }
  }
  return store;
}

UuseChunkStore::~UuseChunkStore() {
  stopIoThread();
  slotFiles_.clear();
  metaFile_.reset();
  if (client_) {
    (void)client_->close().wait();
  }
  client_.reset();
  if (engine_) {
    (void)engine_->stop().wait();
  }
  engine_.reset();
}

void UuseChunkStore::startIoThread() {
  ioThread_ = std::thread([this]() {
    while (true) {
      IoTask *task = nullptr;
      {
        std::unique_lock lock(ioMutex_);
        ioCv_.wait(lock, [this] { return ioStop_ || !ioQueue_.empty(); });
        if (ioStop_ && ioQueue_.empty()) {
          return;
        }
        task = ioQueue_.front();
        ioQueue_.pop_front();
      }
      task->result = task->fn();
      {
        std::lock_guard lock(ioMutex_);
        task->done = true;
      }
      ioDoneCv_.notify_all();
    }
  });
}

void UuseChunkStore::stopIoThread() {
  {
    std::lock_guard lock(ioMutex_);
    ioStop_ = true;
  }
  ioCv_.notify_all();
  if (ioThread_.joinable()) {
    ioThread_.join();
  }
}

Result<Void> UuseChunkStore::runIo(std::function<Result<Void>()> fn) {
  if (!ioThread_.joinable()) {
    return fn();
  }
  IoTask task;
  task.fn = std::move(fn);
  {
    std::lock_guard lock(ioMutex_);
    ioQueue_.push_back(&task);
  }
  ioCv_.notify_one();
  {
    std::unique_lock lock(ioMutex_);
    ioDoneCv_.wait(lock, [&task] { return task.done; });
  }
  return task.result;
}

Result<Void> UuseChunkStore::ioWriteAt(uuse::File &file, const uint8_t *data, uint32_t size, uint64_t offset) {
  return runIo([&]() { return writeAt(file, data, size, offset); });
}

Result<Void> UuseChunkStore::ioReadAt(uuse::File &file, uint8_t *data, uint32_t size, uint64_t offset) {
  return runIo([&]() { return readAt(file, data, size, offset); });
}

Result<Void> UuseChunkStore::ioSyncFile(uuse::File &file, const char *what) {
  return runIo([&]() -> Result<Void> {
    auto synced = file.sync().wait().value();
    if (!synced.ok()) {
      return makeError(StatusCode::kIOError, std::string(what) + synced.message());
    }
    return Void{};
  });
}

std::size_t UuseChunkStore::classOfFile(uint32_t fileId) {
  uint32_t base = 0;
  for (std::size_t c = 0; c < kClassCount; ++c) {
    base += kClassFiles[c];
    if (fileId < base) {
      return c;
    }
  }
  return kClassCount;
}

UuseChunkStore::SlotRef UuseChunkStore::slotRefOf(const ChunkMetadata &meta) {
  const auto cls = classOfFile(meta.innerFileId.chunkIdx);
  return SlotRef{meta.innerFileId.chunkIdx, static_cast<uint32_t>(meta.innerOffset / kClassSize[cls])};
}

std::string UuseChunkStore::slotPath(uint32_t fileId) const {
  return options_.targetDir + "/f" + std::to_string(fileId) + ".dat";
}

std::string UuseChunkStore::metaPath() const { return options_.targetDir + "/meta.log"; }

Result<Void> UuseChunkStore::createLayout() {
  auto dirs = client_->createDirectories(options_.targetDir).wait().value();
  if (!dirs.ok()) {
    return makeError(StatusCode::kIOError, "uuse mkdir: " + dirs.message());
  }
  for (uint32_t fileId = 0; fileId < kFileCount; ++fileId) {
    auto opened = client_->open(slotPath(fileId), uuse::OpenFlags::Create).wait().value();
    if (!opened.ok()) {
      return makeError(StatusCode::kIOError, "uuse open: " + opened.message());
    }
    const uint64_t fileSize = kClassSize[classOfFile(fileId)] * kSlotsPerFile;
    auto allocated = opened.value()->allocate(0, fileSize).wait().value();
    if (!allocated.ok()) {
      return makeError(StatusCode::kIOError, "uuse allocate: " + allocated.message());
    }
  }
  return Void{};
}

Result<Void> UuseChunkStore::openFiles() {
  for (uint32_t fileId = 0; fileId < kFileCount; ++fileId) {
    auto opened = client_->open(slotPath(fileId), uuse::OpenFlags::Create).wait().value();
    if (!opened.ok()) {
      return makeError(StatusCode::kIOError, "uuse open: " + opened.message());
    }
    slotFiles_[fileId] = opened.value();
  }
  auto meta = client_->open(metaPath(), uuse::OpenFlags::Create).wait().value();
  if (!meta.ok()) {
    return makeError(StatusCode::kIOError, "uuse open meta: " + meta.message());
  }
  metaFile_ = meta.value();
  return Void{};
}

Result<Void> UuseChunkStore::replay() {
  auto stat = metaFile_->stat();
  if (!stat.ok()) {
    return makeError(StatusCode::kIOError, "uuse meta stat: " + stat.message());
  }
  const uint64_t size = stat.value().logicalSize;
  std::vector<std::byte> raw;
  raw.reserve(static_cast<std::size_t>(size));
  std::vector<std::byte> buffer(64 * 1024);
  uint64_t offset = 0;
  while (offset < size) {
    const std::size_t want = static_cast<std::size_t>(std::min<uint64_t>(buffer.size(), size - offset));
    uuse::IoVec vec{buffer.data(), want};
    auto read = metaFile_->read(std::span<uuse::IoVec>(&vec, 1), offset, std::stop_token{}).wait().value();
    if (!read.ok()) {
      return makeError(StatusCode::kIOError, "uuse meta read: " + read.message());
    }
    if (read.value().bytes == 0) {
      break;
    }
    raw.insert(raw.end(), buffer.begin(), buffer.begin() + static_cast<std::ptrdiff_t>(read.value().bytes));
    offset += read.value().bytes;
  }
  if (raw.size() % kRecordSize != 0) {
    return makeError(StatusCode::kDataCorruption, "uuse journal is not record aligned");
  }
  for (std::size_t pos = 0; pos < raw.size(); pos += kRecordSize) {
    UuseJournalRecord record{};
    std::memcpy(&record, raw.data() + pos, sizeof(record));
    if (record.magic != kRecordMagic) {
      return makeError(StatusCode::kDataCorruption, "uuse journal magic mismatch");
    }
    if (uuse::crc32c(&record, kRecordSize - 4) != record.recordCrc) {
      return makeError(StatusCode::kDataCorruption, "uuse journal crc mismatch");
    }
    applyRecord(record);
  }
  return Void{};
}

void UuseChunkStore::applyRecord(const UuseJournalRecord &record) {
  if (record.chunkIdLen != 16) {
    return;
  }
  ChunkId chunkId(std::string(reinterpret_cast<const char *>(record.chunkId), 16));
  if (record.op == kOpPut) {
    auto existing = chunks_.find(chunkId.data());
    if (existing != chunks_.end()) {
      releaseSlot(existing->second.slot);
    }
    ChunkInfo info;
    info.meta.commitVer = ChunkVer{record.commitVer};
    info.meta.updateVer = ChunkVer{record.updateVer};
    info.meta.chainVer = ChainVer{record.chainVer};
    info.meta.size = record.size;
    info.meta.chunkState = static_cast<ChunkState>(record.chunkState);
    info.meta.checksumType = static_cast<ChecksumType>(record.checksumType);
    info.meta.checksumValue = record.checksumValue;
    info.meta.recycleState = static_cast<RecycleState>(record.reserved2);
    info.meta.innerFileId.chunkSize = static_cast<uint32_t>(kClassSize[classOfFile(record.fileId)]);
    info.meta.innerFileId.chunkIdx = record.fileId;
    info.meta.innerOffset = record.innerOffset;
    info.slot = slotRefOf(info.meta);
    chunks_[chunkId.data()] = info;
  } else if (record.op == kOpDelete) {
    auto existing = chunks_.find(chunkId.data());
    if (existing != chunks_.end()) {
      releaseSlot(existing->second.slot);
      chunks_.erase(existing);
    }
  }
}

void UuseChunkStore::rebuildFreeSlots() {
  for (auto &slots : freeSlots_) {
    slots.clear();
  }
  std::set<std::pair<uint32_t, uint32_t>> used;
  for (const auto &[key, info] : chunks_) {
    used.emplace(info.slot.fileId, info.slot.slot);
  }
  for (uint32_t fileId = 0; fileId < kFileCount; ++fileId) {
    for (uint32_t slot = 0; slot < kSlotsPerFile; ++slot) {
      if (used.find({fileId, slot}) == used.end()) {
        freeSlots_[classOfFile(fileId)].push_back(SlotRef{fileId, slot});
      }
    }
  }
}

Result<Void> UuseChunkStore::appendRecord(const UuseJournalRecord &record) {
  uuse::IoVec vec{const_cast<std::byte *>(reinterpret_cast<const std::byte *>(&record)), kRecordSize};
  auto result = metaFile_->append(std::span<const uuse::IoVec>(&vec, 1), std::stop_token{}).wait().value();
  if (!result.ok()) {
    return makeError(StatusCode::kIOError, "uuse meta append: " + result.message());
  }
  if (result.value().bytes != kRecordSize) {
    return makeError(StatusCode::kIOError, "uuse short meta append");
  }
  return Void{};
}

Result<UuseChunkStore::SlotRef> UuseChunkStore::allocateSlot(std::size_t cls) {
  if (freeSlots_[cls].empty()) {
    return makeError(StatusCode::kInvalidArg, "uuse chunk class has no free slot");
  }
  const SlotRef ref = freeSlots_[cls].back();
  freeSlots_[cls].pop_back();
  return ref;
}

void UuseChunkStore::releaseSlot(const SlotRef &slot) {
  if (slot.fileId < kFileCount && slot.slot < kSlotsPerFile) {
    freeSlots_[classOfFile(slot.fileId)].push_back(slot);
  }
}

Result<UuseChunkStore::ChunkInfo> UuseChunkStore::get(const ChunkId &chunkId) {
  std::lock_guard lock(mutex_);
  auto it = chunks_.find(chunkId.data());
  if (it == chunks_.end()) {
    return makeError(StorageCode::kChunkMetadataNotFound, "uuse chunk not found");
  }
  return it->second;
}

Result<UuseChunkStore::ChunkInfo> UuseChunkStore::createChunk(const ChunkId &chunkId, uint32_t chunkSize) {
  if (chunkId.data().size() != 16) {
    return makeError(StatusCode::kInvalidArg, "uuse chunk id must be 16 bytes");
  }
  const std::size_t cls = classForSize(chunkSize);
  if (cls >= kClassCount) {
    return makeError(StatusCode::kInvalidArg, "uuse chunk size above max class");
  }
  std::lock_guard lock(mutex_);
  auto slotResult = allocateSlot(cls);
  if (!slotResult) {
    /* Reclaim removed chunks first (the meta may have removed them since the
     * last recycle cycle), then retry once. */
    auto recycled = recycleRemovedLocked();
    if (!recycled) {
      return folly::makeUnexpected(recycled.error());
    }
    slotResult = allocateSlot(cls);
    if (!slotResult) {
      return folly::makeUnexpected(slotResult.error());
    }
  }
  const SlotRef slot = slotResult.value();
  ChunkInfo info;
  info.slot = slot;
  info.meta.chunkState = ChunkState::DIRTY;
  info.meta.checksumType = ChecksumType::NONE;
  info.meta.innerFileId.chunkSize = chunkSize;
  info.meta.innerFileId.chunkIdx = slot.fileId;
  info.meta.innerOffset = static_cast<uint64_t>(slot.slot) * kClassSize[cls];
  return info;
}

Result<Void> UuseChunkStore::writeChunk(ChunkInfo &chunkInfo, const uint8_t *data, uint32_t size, uint32_t offset) {
  std::lock_guard lock(mutex_);
  auto fileIt = slotFiles_.find(chunkInfo.slot.fileId);
  if (fileIt == slotFiles_.end()) {
    return makeError(StatusCode::kInvalidArg, "uuse slot file missing");
  }
  auto written = ioWriteAt(*fileIt->second, data, size, chunkInfo.meta.innerOffset + offset);
  if (!written) {
    return written;
  }
  chunkInfo.meta.size = std::max<uint32_t>(chunkInfo.meta.size, offset + size);
  return Void{};
}

Result<Void> UuseChunkStore::readChunk(const ChunkInfo &chunkInfo, uint8_t *data, uint32_t size, uint32_t offset) {
  std::lock_guard lock(mutex_);
  auto fileIt = slotFiles_.find(chunkInfo.slot.fileId);
  if (fileIt == slotFiles_.end()) {
    return makeError(StatusCode::kInvalidArg, "uuse slot file missing");
  }
  return ioReadAt(*fileIt->second, data, size, chunkInfo.meta.innerOffset + offset);
}

Result<Void> UuseChunkStore::set(const ChunkId &chunkId, const ChunkInfo &chunkInfo) {
  if (chunkId.data().size() != 16) {
    return makeError(StatusCode::kInvalidArg, "uuse chunk id must be 16 bytes");
  }
  std::lock_guard lock(mutex_);
  const auto record = makeRecord(kOpPut, chunkId, chunkInfo);
  auto appended = appendRecord(record);
  if (!appended) {
    return appended;
  }
  auto existing = chunks_.find(chunkId.data());
  if (existing != chunks_.end() && (existing->second.slot.fileId != chunkInfo.slot.fileId ||
                                    existing->second.slot.slot != chunkInfo.slot.slot)) {
    releaseSlot(existing->second.slot);
  }
  chunks_[chunkId.data()] = chunkInfo;
  return Void{};
}

Result<Void> UuseChunkStore::remove(const ChunkId &chunkId, ChunkInfo &chunkInfo) {
  std::lock_guard lock(mutex_);
  auto it = chunks_.find(chunkId.data());
  if (it == chunks_.end()) {
    return makeError(StorageCode::kChunkMetadataNotFound, "uuse chunk not found");
  }
  chunkInfo = it->second;
  const auto record = makeRecord(kOpDelete, chunkId, chunkInfo);
  auto appended = appendRecord(record);
  if (!appended) {
    return appended;
  }
  releaseSlot(it->second.slot);
  chunks_.erase(it);
  return Void{};
}

Result<bool> UuseChunkStore::punchHole() {
  std::lock_guard lock(mutex_);
  return recycleRemovedLocked();
}

Result<bool> UuseChunkStore::recycleRemovedLocked() {
  std::vector<std::pair<ChunkId, ChunkInfo>> pending;
  for (const auto &[key, info] : chunks_) {
    if (info.meta.recycleState != RecycleState::NORMAL) {
      pending.emplace_back(ChunkId(key), info);
    }
  }
  if (pending.empty()) {
    return true;
  }
  for (auto &[chunkId, info] : pending) {
    const auto record = makeRecord(kOpDelete, chunkId, info);
    auto appended = appendRecord(record);
    if (!appended) {
      return folly::makeUnexpected(appended.error());
    }
    releaseSlot(info.slot);
    chunks_.erase(chunkId.data());
  }
  return false;
}

Result<UuseChunkStore::UpdateOutput> UuseChunkStore::update(const UpdateInput &input) {
  if (input.chunkId.data().size() != 16) {
    return makeError(StatusCode::kInvalidArg, "uuse chunk id must be 16 bytes");
  }
  if (!input.isRemove && !input.isTruncate && !input.isExtend &&
      (input.offset >= input.chunkSize || input.offset + input.length > input.chunkSize)) {
    return makeError(StatusCode::kInvalidArg, "uuse write offset exceeds chunk size");
  }

  std::lock_guard lock(mutex_);
  UpdateOutput output;
  auto existing = chunks_.find(input.chunkId.data());
  if (existing == chunks_.end() && input.isRemove) {
    output.commitVer = ChunkVer{static_cast<uint32_t>(input.updateVer)};
    output.updateVer = ChunkVer{static_cast<uint32_t>(input.updateVer)};
    output.chainVer = ChainVer{input.chainVer};
    return output;
  }

  ChunkInfo info;
  bool needCreate = false;
  if (existing != chunks_.end()) {
    info = existing->second;
    if (input.chunkSize != 0 && info.meta.innerFileId.chunkSize != input.chunkSize) {
      return makeError(StorageCode::kChunkSizeMismatch, "uuse chunk size mismatch");
    }
  } else {
    needCreate = true;
    info.meta.chainVer = ChainVer{input.chainVer};
    info.meta.chunkState = ChunkState::CLEAN;
  }
  auto &meta = info.meta;

  output.commitVer = meta.commitVer;
  output.updateVer = meta.updateVer;
  output.chainVer = meta.chainVer;
  output.checksum = meta.checksum();
  output.length = meta.size;

  if (input.isRemove) {
    const uint32_t next = input.updateVer != 0 ? static_cast<uint32_t>(input.updateVer)
                                               : meta.updateVer.toUnderType() + 1;
    meta.updateVer = ChunkVer{next};
    meta.chainVer = ChainVer{input.chainVer};
    meta.recycleState = RecycleState::REMOVAL_IN_PROGRESS;
    auto record = makeRecord(kOpPut, input.chunkId, info);
    auto appended = appendRecord(record);
    if (!appended) {
      return folly::makeUnexpected(appended.error());
    }
    chunks_[input.chunkId.data()] = info;
    output.updateVer = meta.updateVer;
    output.chainVer = meta.chainVer;
    return output;
  }

  uint32_t nextUpdate = meta.updateVer.toUnderType();
  if (input.isSyncing) {
    nextUpdate = static_cast<uint32_t>(input.updateVer);
    meta.commitVer = ChunkVer{nextUpdate};
  } else if (input.updateVer > 0) {
    const uint32_t commitVer = meta.commitVer.toUnderType();
    if (input.updateVer <= commitVer) {
      return makeError(StorageCode::kChunkCommittedUpdate, "uuse committed update");
    }
    if (input.updateVer <= nextUpdate) {
      return makeError(StorageCode::kChunkStaleUpdate, "uuse stale update");
    }
    if (input.updateVer > nextUpdate + 1) {
      return makeError(StorageCode::kChunkMissingUpdate, "uuse missing update");
    }
    nextUpdate = static_cast<uint32_t>(input.updateVer);
  } else {
    nextUpdate += 1;
    if (nextUpdate > meta.commitVer.toUnderType() + 1) {
      return makeError(StorageCode::kChunkAdvanceUpdate, "uuse advance update");
    }
  }

  if (needCreate) {
    const std::size_t cls = classForSize(input.chunkSize);
    if (cls >= kClassCount) {
      return makeError(StatusCode::kInvalidArg, "uuse chunk size above max class");
    }
    auto slotResult = allocateSlot(cls);
    if (!slotResult) {
      /* Reclaim removed chunks first, then retry once. */
      auto recycled = recycleRemovedLocked();
      if (!recycled) {
        return folly::makeUnexpected(recycled.error());
      }
      slotResult = allocateSlot(cls);
      if (!slotResult) {
        return folly::makeUnexpected(slotResult.error());
      }
    }
    info.slot = slotResult.value();
    /* Metadata keeps the logical chunk size; the slot class only drives the
     * physical slot size (1 MiB chunks live in the 4 MiB class). */
    info.meta.innerFileId.chunkSize = input.chunkSize;
    info.meta.innerFileId.chunkIdx = info.slot.fileId;
    info.meta.innerOffset = static_cast<uint64_t>(info.slot.slot) * kClassSize[cls];
  }

  auto &updated = info.meta;
  updated.updateVer = ChunkVer{nextUpdate};
  updated.chainVer = ChainVer{input.chainVer};
  updated.chunkState = ChunkState::DIRTY;
  updated.recycleState = RecycleState::NORMAL;

  if (input.checksum.type != ChecksumType::NONE && input.length != 0) {
    ChecksumInfo::MemoryDataIterator iter(input.data, input.length);
    const auto computed = ChecksumInfo::create(input.checksum.type, &iter, input.length);
    if (computed != input.checksum) {
      return makeError(StorageCode::kChecksumMismatch, "uuse client checksum mismatch");
    }
  }

  auto fileIt = slotFiles_.find(info.slot.fileId);
  if (fileIt == slotFiles_.end()) {
    return makeError(StatusCode::kInvalidArg, "uuse slot file missing");
  }
  static const std::vector<uint8_t> zeros(kClassSize[kClassCount - 1], 0);
  if (input.isTruncate || input.isExtend) {
    /* Truncate shrinks (or keeps) the chunk; extend zero-fills the new range.
     * The storage service uses TRUNCATE to materialize the last chunk. */
    if (input.length > updated.size) {
      auto filled = ioWriteAt(*fileIt->second, zeros.data(), input.length - updated.size,
                              updated.innerOffset + updated.size);
      if (!filled) {
        return folly::makeUnexpected(filled.error());
      }
    }
    updated.size = input.length;
  } else if (input.length != 0) {
    if (updated.size < input.offset) {
      auto filled = ioWriteAt(*fileIt->second, zeros.data(), input.offset - updated.size,
                              updated.innerOffset + updated.size);
      if (!filled) {
        return folly::makeUnexpected(filled.error());
      }
    }
    auto written = ioWriteAt(*fileIt->second, input.data, input.length, updated.innerOffset + input.offset);
    if (!written) {
      return folly::makeUnexpected(written.error());
    }
    updated.size = std::max<uint32_t>(updated.size, input.offset + input.length);
  }

  /* Whole-chunk write: the checksum can be computed from the written buffer
   * (no read-back). Partial/gap/truncate writes fall back to a recompute. */
  if (!input.isTruncate && !input.isExtend && input.length != 0 && input.offset == 0 &&
      input.length == updated.size) {
    ChecksumInfo::MemoryDataIterator iter(input.data, input.length);
    const auto fullChecksum = ChecksumInfo::create(ChecksumType::CRC32C, &iter, input.length);
    updated.checksumType = fullChecksum.type;
    updated.checksumValue = fullChecksum.value;
  } else {
    std::vector<uint8_t> chunkData(updated.size);
    auto readBack = ioReadAt(*fileIt->second, chunkData.data(), chunkData.size(), updated.innerOffset);
    if (!readBack) {
      return folly::makeUnexpected(readBack.error());
    }
    ChecksumInfo::MemoryDataIterator fullIter(chunkData.data(), chunkData.size());
    const auto fullChecksum = ChecksumInfo::create(ChecksumType::CRC32C, &fullIter, chunkData.size());
    updated.checksumType = fullChecksum.type;
    updated.checksumValue = fullChecksum.value;
  }
  updated.chunkState = ChunkState::CLEAN;

  auto record = makeRecord(kOpPut, input.chunkId, info);
  auto appended = appendRecord(record);
  if (!appended) {
    return folly::makeUnexpected(appended.error());
  }
  chunks_[input.chunkId.data()] = info;

  output.commitVer = updated.commitVer;
  output.updateVer = updated.updateVer;
  output.chainVer = updated.chainVer;
  output.checksum = updated.checksum();
  output.length = updated.size;
  return output;
}

Result<Void> UuseChunkStore::commit(const ChunkId &chunkId, uint64_t commitVer, uint32_t chainVer) {
  std::lock_guard lock(mutex_);
  auto it = chunks_.find(chunkId.data());
  if (it == chunks_.end()) {
    return makeError(StorageCode::kChunkMetadataNotFound, "uuse chunk not found");
  }
  it->second.meta.commitVer = ChunkVer{static_cast<uint32_t>(commitVer)};
  it->second.meta.chainVer = ChainVer{chainVer};
  it->second.meta.chunkState = ChunkState::COMMIT;
  const auto record = makeRecord(kOpPut, chunkId, it->second);
  return appendRecord(record);
}

Result<std::vector<std::pair<ChunkId, ChunkMetadata>>> UuseChunkStore::queryChunks(const ChunkIdRange &range) const {
  std::lock_guard lock(mutex_);
  std::vector<std::pair<ChunkId, ChunkMetadata>> result;
  for (const auto &[key, info] : chunks_) {
    ChunkId id(key);
    if (id < range.begin || range.end < id) {
      continue;
    }
    result.emplace_back(id, info.meta);
    if (range.maxNumChunkIdsToProcess != 0 && result.size() >= range.maxNumChunkIdsToProcess) {
      break;
    }
  }
  std::sort(result.begin(), result.end(), [](const auto &left, const auto &right) {
    return right.first < left.first;
  });
  return result;
}

std::vector<std::pair<ChunkId, ChunkMetadata>> UuseChunkStore::allChunks() const {
  std::lock_guard lock(mutex_);
  std::vector<std::pair<ChunkId, ChunkMetadata>> result;
  result.reserve(chunks_.size());
  for (const auto &[key, info] : chunks_) {
    result.emplace_back(ChunkId(key), info.meta);
  }
  return result;
}

uint64_t UuseChunkStore::usedSize() const {
  std::lock_guard lock(mutex_);
  uint64_t total = 0;
  for (const auto &[key, info] : chunks_) {
    total += info.meta.size;
  }
  return total;
}

std::vector<ChunkId> UuseChunkStore::uncommitted() const {
  std::lock_guard lock(mutex_);
  std::vector<ChunkId> result;
  for (const auto &[key, info] : chunks_) {
    if (info.meta.commitVer != info.meta.updateVer) {
      result.emplace_back(key);
    }
  }
  return result;
}

Result<Void> UuseChunkStore::resetUncommitted(ChainVer chainVer) {
  std::lock_guard lock(mutex_);
  for (auto &[key, info] : chunks_) {
    if (info.meta.commitVer != info.meta.updateVer) {
      info.meta.commitVer = info.meta.updateVer;
      info.meta.chainVer = chainVer;
      info.meta.chunkState = ChunkState::COMMIT;
      const auto record = makeRecord(kOpPut, ChunkId(key), info);
      auto appended = appendRecord(record);
      if (!appended) {
        return appended;
      }
    }
  }
  return Void{};
}

Result<Void> UuseChunkStore::sync() {
  std::lock_guard lock(mutex_);
  for (auto &[fileId, file] : slotFiles_) {
    auto synced = ioSyncFile(*file, "uuse slot sync: ");
    if (!synced) {
      return synced;
    }
  }
  return ioSyncFile(*metaFile_, "uuse meta sync: ");
}

std::size_t UuseChunkStore::liveChunks() const {
  std::lock_guard lock(mutex_);
  return chunks_.size();
}

std::size_t UuseChunkStore::freeSlotCount() const {
  std::lock_guard lock(mutex_);
  std::size_t count = 0;
  for (const auto &slots : freeSlots_) {
    count += slots.size();
  }
  return count;
}

}  // namespace hf3fs::storage
