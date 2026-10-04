/* UuseChunkStore smoke test.
 * phase 1: create the store, put three chunks, verify, COW overwrite, remove,
 *          query, sync (process exits afterwards).
 * phase 2: separate process reloads the journal and verifies recovery. */
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "common/utils/Result.h"
#include "fbs/storage/Common.h"
#include "storage/store/UuseChunkStore.h"

using namespace hf3fs;
using namespace hf3fs::storage;

namespace {

std::string chunkIdFor(uint64_t index) {
  std::string id(16, '\0');
  for (int i = 0; i < 8; ++i) {
    id[15 - i] = static_cast<char>((index >> (8 * i)) & 0xff);
  }
  return id;
}

void fillPattern(std::vector<uint8_t> &data, uint32_t seed) {
  uint32_t state = seed * 2654435761u + 1u;
  for (auto &byte : data) {
    state = state * 1664525u + 1013904223u;
    byte = static_cast<uint8_t>(state >> 24);
  }
}

uint32_t checksumOf(const std::vector<uint8_t> &data) {
  ChecksumInfo::MemoryDataIterator iter(data.data(), data.size());
  return ChecksumInfo::create(ChecksumType::CRC32C, &iter, data.size()).value;
}

int fail(const char *what, const Status &status) {
  const auto msg = status.message();
  std::fprintf(stderr, "SMOKE_FAIL: %s: %.*s\n", what, static_cast<int>(msg.size()), msg.data());
  return 1;
}

int failMsg(const char *what) {
  std::fprintf(stderr, "SMOKE_FAIL: %s\n", what);
  return 1;
}

Result<ChunkId> putChunk(UuseChunkStore &store, uint64_t index, uint32_t size, uint32_t seed, uint32_t ver) {
  ChunkId id(chunkIdFor(index));
  auto created = store.createChunk(id, size);
  if (!created.hasValue()) {
    return folly::makeUnexpected(created.error());
  }
  auto info = created.value();
  std::vector<uint8_t> data(size);
  fillPattern(data, seed);
  auto written = store.writeChunk(info, data.data(), size, 0);
  if (!written.hasValue()) {
    return folly::makeUnexpected(written.error());
  }
  info.meta.commitVer = ChunkVer{ver};
  info.meta.updateVer = ChunkVer{ver};
  info.meta.chainVer = ChainVer{1};
  info.meta.chunkState = ChunkState::COMMIT;
  info.meta.checksumType = ChecksumType::CRC32C;
  info.meta.checksumValue = checksumOf(data);
  auto setResult = store.set(id, info);
  if (!setResult.hasValue()) {
    return folly::makeUnexpected(setResult.error());
  }
  return id;
}

int verifyChunk(UuseChunkStore &store, uint64_t index, uint32_t expectedSize, uint32_t expectedVer) {
  ChunkId id(chunkIdFor(index));
  auto infoResult = store.get(id);
  if (!infoResult.hasValue()) {
    return fail("get", infoResult.error());
  }
  auto info = infoResult.value();
  if (info.meta.size != expectedSize || info.meta.updateVer.toUnderType() != expectedVer) {
    std::fprintf(stderr, "SMOKE_FAIL: chunk %llu meta size=%u ver=%u\n",
                 static_cast<unsigned long long>(index), info.meta.size,
                 info.meta.updateVer.toUnderType());
    return 1;
  }
  std::vector<uint8_t> data(info.meta.size);
  auto read = store.readChunk(info, data.data(), data.size(), 0);
  if (!read.hasValue()) {
    return fail("read", read.error());
  }
  if (checksumOf(data) != info.meta.checksumValue) {
    return failMsg("checksum mismatch");
  }
  return 0;
}

}  // namespace

int main(int argc, char **argv) {
  if (argc < 4) {
    std::fprintf(stderr, "usage: %s <uuse.conf> <targetDir> <phase:1|2>\n", argv[0]);
    return 2;
  }
  UuseChunkStore::Options options;
  options.uuseConfPath = argv[1];
  options.targetDir = argv[2];
  options.targetId = 10001;
  options.create = (std::atoi(argv[3]) == 1);

  auto storeResult = UuseChunkStore::open(options);
  if (!storeResult.hasValue()) {
    return fail("open", storeResult.error());
  }
  auto store = std::move(storeResult).value();

  if (options.create) {
    auto a = putChunk(*store, 1, 64u << 10, 11, 1);
    if (!a.hasValue()) return fail("put A", a.error());
    auto b = putChunk(*store, 2, 512u << 10, 22, 1);
    if (!b.hasValue()) return fail("put B", b.error());
    auto c = putChunk(*store, 3, 4u << 20, 33, 1);
    if (!c.hasValue()) return fail("put C", c.error());
    if (verifyChunk(*store, 1, 64u << 10, 1) || verifyChunk(*store, 2, 512u << 10, 1) ||
        verifyChunk(*store, 3, 4u << 20, 1)) {
      return 1;
    }
    std::printf("phase1: 3 chunks verified\n");

    auto overwritten = putChunk(*store, 1, 512u << 10, 111, 2);
    if (!overwritten.hasValue()) return fail("overwrite A", overwritten.error());
    if (verifyChunk(*store, 1, 512u << 10, 2)) return 1;
    std::printf("phase1: COW overwrite A ok\n");

    ChunkId bId(chunkIdFor(2));
    UuseChunkStore::ChunkInfo removed;
    auto removeResult = store->remove(bId, removed);
    if (!removeResult.hasValue()) return fail("remove B", removeResult.error());
    if (store->get(bId).hasValue()) return failMsg("removed chunk still present");
    std::printf("phase1: remove B ok\n");

    ChunkIdRange range;
    range.begin = ChunkId(std::string(16, '\0'));
    range.end = ChunkId(std::string(16, static_cast<char>(0xff)));
    auto queried = store->queryChunks(range);
    if (!queried.hasValue()) return fail("query", queried.error());
    std::printf("phase1: query returned %zu chunks\n", queried.value().size());

    /* update()/commit() semantics on a fresh chunk. */
    {
      std::vector<uint8_t> data(64u << 10);
      fillPattern(data, 77);
      UuseChunkStore::UpdateInput input;
      input.chunkId = ChunkId(chunkIdFor(4));
      input.chunkSize = 64u << 10;
      input.offset = 0;
      input.length = static_cast<uint32_t>(data.size());
      input.updateVer = 0;
      input.chainVer = 1;
      input.data = data.data();
      ChecksumInfo::MemoryDataIterator iter(data.data(), data.size());
      input.checksum = ChecksumInfo::create(ChecksumType::CRC32C, &iter, data.size());

      auto updated = store->update(input);
      if (!updated.hasValue()) return fail("update create", updated.error());
      if (updated.value().updateVer.toUnderType() != 1 || updated.value().commitVer.toUnderType() != 0) {
        return failMsg("update version wrong");
      }
      auto stale = store->update(input);
      if (stale.hasValue()) return failMsg("stale update accepted");
      auto committed = store->commit(input.chunkId, 1, 1);
      if (!committed.hasValue()) return fail("commit", committed.error());
      if (verifyChunk(*store, 4, 64u << 10, 1)) return 1;
      std::printf("phase1: update/commit semantics ok\n");
    }

    auto syncResult = store->sync();
    if (!syncResult.hasValue()) return fail("sync", syncResult.error());
    std::printf("SMOKE1_PASS live=%zu free=%zu used=%llu\n", store->liveChunks(), store->freeSlotCount(),
                static_cast<unsigned long long>(store->usedSize()));
    return 0;
  }

  if (store->liveChunks() != 3) {
    return failMsg("replayed live count != 3");
  }
  if (verifyChunk(*store, 1, 512u << 10, 2) || verifyChunk(*store, 3, 4u << 20, 1) ||
      verifyChunk(*store, 4, 64u << 10, 1)) {
    return 1;
  }
  ChunkId bId(chunkIdFor(2));
  if (store->get(bId).hasValue()) {
    return failMsg("deleted chunk resurrected");
  }
  std::printf("SMOKE2_PASS live=%zu free=%zu used=%llu\n", store->liveChunks(), store->freeSlotCount(),
              static_cast<unsigned long long>(store->usedSize()));
  return 0;
}
