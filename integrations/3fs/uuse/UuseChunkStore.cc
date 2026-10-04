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

std::size_t UuseChunkStore::classOfFile(uint32_t fileId) { return fileId / kFilesPerClass; }

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
  for (uint32_t fileId = 0; fileId < kClassCount * kFilesPerClass; ++fileId) {
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
  for (uint32_t fileId = 0; fileId < kClassCount * kFilesPerClass; ++fileId) {
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
  for (uint32_t fileId = 0; fileId < kClassCount * kFilesPerClass; ++fileId) {
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
  if (slot.fileId < kClassCount * kFilesPerClass && slot.slot < kSlotsPerFile) {
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
    return folly::makeUnexpected(slotResult.error());
  }
  const SlotRef slot = slotResult.value();
  ChunkInfo info;
  info.slot = slot;
  info.meta.chunkState = ChunkState::DIRTY;
  info.meta.checksumType = ChecksumType::NONE;
  info.meta.innerFileId.chunkSize = static_cast<uint32_t>(kClassSize[cls]);
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
  auto written = writeAt(*fileIt->second, data, size, chunkInfo.meta.innerOffset + offset);
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
  return readAt(*fileIt->second, data, size, chunkInfo.meta.innerOffset + offset);
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
    auto synced = file->sync().wait().value();
    if (!synced.ok()) {
      return makeError(StatusCode::kIOError, "uuse slot sync: " + synced.message());
    }
  }
  auto metaSynced = metaFile_->sync().wait().value();
  if (!metaSynced.ok()) {
    return makeError(StatusCode::kIOError, "uuse meta sync: " + metaSynced.message());
  }
  return Void{};
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
