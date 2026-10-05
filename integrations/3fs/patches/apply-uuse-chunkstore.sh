#!/bin/bash
# Install the UUSE chunk store (M2.1 core + M2.2 StorageTarget wiring) into a
# 3FS checkout:
#   - UuseChunkStore.{h,cc}      -> src/storage/store/  (compiled by the storage lib)
#   - UuseChunkStoreSmoke.cc     -> src/uuse_store/     (smoke target)
#   - CMake block: UUSE include/lib on the storage target + smoke executable
#   - PhysicalConfig: use_uuse_engine / uuse_conf
#   - StorageTarget: third backend branch (create/load/update/commit/query)
set -eu
ROOT="${1:-/build/3fs}"
HERE="$(cd "$(dirname "$0")" && pwd)"
SRC="$HERE/../uuse"

mkdir -p "$ROOT/src/uuse_store"
cp "$SRC/UuseChunkStore.h" "$ROOT/src/storage/store/UuseChunkStore.h"
cp "$SRC/UuseChunkStore.cc" "$ROOT/src/storage/store/UuseChunkStore.cc"
cp "$SRC/UuseChunkStoreSmoke.cc" "$ROOT/src/uuse_store/UuseChunkStoreSmoke.cc"
rm -f "$ROOT/src/storage/store/UuseChunkStoreSmoke.cc"

python3 - "$ROOT" <<'PY'
import sys
root = sys.argv[1]

def read(p):
    return open(p, encoding='utf-8', errors='replace').read()

def write(p, t):
    open(p, 'w', encoding='utf-8', errors='replace').write(t)

def rep(path, old, new, required=True):
    t = read(path)
    if new in t:
        return
    if old not in t:
        if required:
            raise SystemExit('anchor missing in %s: %s' % (path, old[:60]))
        return
    write(path, t.replace(old, new, 1))

# ---- CMake block ----
p = root + '/CMakeLists.txt'
t = read(p)
marker = '# ---- UUSE chunk store'
idx = t.find(marker)
if idx != -1:
    t = t[:idx]
block = '''# ---- UUSE chunk store (M2.1/M2.2 lab target) ----
set(UUSE_CORE_ROOT "/root/code-uuse" CACHE PATH "UUSE dev artifacts root (headers + libuuse_core.so)")
if(EXISTS "${UUSE_CORE_ROOT}/include/uuse/core.hpp" AND EXISTS "${UUSE_CORE_ROOT}/build-verify/libuuse_core.so")
  cmake_policy(SET CMP0079 NEW)
  target_include_directories(storage PRIVATE "${UUSE_CORE_ROOT}/include")
  target_link_libraries(storage "${UUSE_CORE_ROOT}/build-verify/libuuse_core.so")

  add_executable(uuse_chunkstore_smoke src/uuse_store/UuseChunkStoreSmoke.cc)
  target_include_directories(uuse_chunkstore_smoke PRIVATE "${UUSE_CORE_ROOT}/include")
  target_link_libraries(uuse_chunkstore_smoke PRIVATE storage)
  target_link_options(uuse_chunkstore_smoke PRIVATE
      "-Wl,--allow-shlib-undefined" "-Wl,-rpath,${UUSE_CORE_ROOT}/build-verify")
  set_target_properties(uuse_chunkstore_smoke PROPERTIES
      RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
  message(STATUS "UUSE chunk store enabled (${UUSE_CORE_ROOT})")
else()
  message(STATUS "UUSE chunk store disabled (no artifacts at ${UUSE_CORE_ROOT})")
endif()
'''
write(p, t + block)

# ---- PhysicalConfig ----
p = root + '/src/storage/store/PhysicalConfig.h'
t = read(p)
if 'use_uuse_engine' not in t:
    anchor = '  SERDE_STRUCT_FIELD(only_chunk_engine, false);\n'
    add = '  SERDE_STRUCT_FIELD(use_uuse_engine, false);\n  SERDE_STRUCT_FIELD(uuse_conf, std::string{});\n'
    assert anchor in t
    write(p, t.replace(anchor, anchor + add, 1))

# ---- StorageTarget.h ----
p = root + '/src/storage/store/StorageTarget.h'
t = read(p)
if '#include "storage/store/UuseChunkStore.h"' not in t:
    anchor = '#include "storage/store/PhysicalConfig.h"\n'
    assert anchor in t
    t = t.replace(anchor, anchor + '#include "storage/store/UuseChunkStore.h"\n', 1)
    t = t.replace('\nclass UuseChunkStore;\n', '\n', 1)
    write(p, t)
rep(p, '  // finish aio read.\n  Result<Void> aioFinishRead(AioReadJob &job);\n',
    '  // finish aio read.\n  Result<Void> aioFinishRead(AioReadJob &job);\n\n'
    '  // UUSE backend helpers.\n  Result<Void> openUuse(bool create);\n'
    '  Result<uint32_t> updateUuseChunk(UpdateJob &job);\n'
    '  Result<uint32_t> commitUuseChunk(UpdateJob &job);\n')
rep(p, '''  Result<bool> punchHole() {
    if (useChunkEngine()) {
      return true;
    } else {
      return chunkStore_.punchHole();
    }
  }''',
    '''  Result<bool> punchHole() {
    if (useChunkEngine() || useUuseEngine()) {
      return true;
    } else {
      return chunkStore_.punchHole();
    }
  }''')
rep(p, '''  Result<Void> sync() {
    if (useChunkEngine()) {
      return Void{};
    } else {
      return chunkStore_.sync();
    }
  }''',
    '''  Result<Void> sync() {
    if (useChunkEngine()) {
      return Void{};
    } else if (useUuseEngine()) {
      return uuseStore_ ? uuseStore_->sync() : Void{};
    } else {
      return chunkStore_.sync();
    }
  }''')
rep(p, '''    if (useChunkEngine()) {
      return ChunkEngine::chainUsedSize(*engine_, ChainId{targetConfig_.chain_id});
    } else {
      return chunkStore_.usedSize();
    }''',
    '''    if (useChunkEngine()) {
      return ChunkEngine::chainUsedSize(*engine_, ChainId{targetConfig_.chain_id});
    } else if (useUuseEngine()) {
      return uuseStore_ ? uuseStore_->usedSize() : 0;
    } else {
      return chunkStore_.usedSize();
    }''')
rep(p, '''    if (useChunkEngine()) {
      return ChunkEngine::queryUncommittedChunks(*engine_, chainId());
    } else {
      return chunkStore_.uncommitted();
    }''',
    '''    if (useChunkEngine()) {
      return ChunkEngine::queryUncommittedChunks(*engine_, chainId());
    } else if (useUuseEngine()) {
      return uuseStore_ ? uuseStore_->uncommitted() : std::vector<ChunkId>{};
    } else {
      return chunkStore_.uncommitted();
    }''')
rep(p, '''    if (useChunkEngine()) {
      return ChunkEngine::resetUncommittedChunks(*engine_, chainId(), chainVer);
    } else {
      return chunkStore_.resetUncommitted(chainVer);
    }''',
    '''    if (useChunkEngine()) {
      return ChunkEngine::resetUncommittedChunks(*engine_, chainId(), chainVer);
    } else if (useUuseEngine()) {
      return uuseStore_ ? uuseStore_->resetUncommitted(chainVer) : Void{};
    } else {
      return chunkStore_.resetUncommitted(chainVer);
    }''')
rep(p, '''  void setEmergencyRecycling(bool enable) {
    if (useChunkEngine()) {
      return;
    } else {
      return chunkStore_.setEmergencyRecycling(enable);
    }
  }''',
    '''  void setEmergencyRecycling(bool enable) {
    if (useChunkEngine() || useUuseEngine()) {
      return;
    } else {
      return chunkStore_.setEmergencyRecycling(enable);
    }
  }''')
rep(p, '  inline bool useChunkEngine() const { return targetConfig_.only_chunk_engine; }\n',
    '  inline bool useChunkEngine() const { return targetConfig_.only_chunk_engine; }\n\n'
    '  // check if the UUSE chunk store is used.\n  inline bool useUuseEngine() const { return targetConfig_.use_uuse_engine; }\n')
rep(p, '  ChunkStore chunkStore_;\n',
    '  ChunkStore chunkStore_;\n  std::unique_ptr<UuseChunkStore> uuseStore_;\n')

# ---- StorageTarget.cc ----
p = root + '/src/storage/store/StorageTarget.cc'
rep(p, '#include "storage/store/StorageTarget.h"\n',
    '#include "storage/store/StorageTarget.h"\n\n#include "storage/store/UuseChunkStore.h"\n')
rep(p, '''  } else {
    RETURN_AND_LOG_ON_ERROR(chunkStore_.create(targetConfig_));
  }''',
    '''  } else if (useUuseEngine()) {
    RETURN_AND_LOG_ON_ERROR(openUuse(true));
  } else {
    RETURN_AND_LOG_ON_ERROR(chunkStore_.create(targetConfig_));
  }''')
rep(p, '''  if (!targetConfig_.only_chunk_engine) {
    RETURN_AND_LOG_ON_ERROR(chunkStore_.load(targetConfig_));
  }''',
    '''  if (targetConfig_.use_uuse_engine) {
    RETURN_AND_LOG_ON_ERROR(openUuse(false));
  } else if (!targetConfig_.only_chunk_engine) {
    RETURN_AND_LOG_ON_ERROR(chunkStore_.load(targetConfig_));
  }''')
rep(p, '''    if (useChunkEngine()) {
      job.setResult(ChunkEngine::commit(*engine_, job, config_.kv_store().sync_when_write()));
    } else {
      job.setResult(ChunkReplica::commit(chunkStore_, job));
    }
  } else {
    auto result =
        useChunkEngine() ? ChunkEngine::update(*engine_, job) : ChunkReplica::update(chunkStore_, job, executor);''',
    '''    if (useChunkEngine()) {
      job.setResult(ChunkEngine::commit(*engine_, job, config_.kv_store().sync_when_write()));
    } else if (useUuseEngine()) {
      job.setResult(commitUuseChunk(job));
    } else {
      job.setResult(ChunkReplica::commit(chunkStore_, job));
    }
  } else {
    Result<uint32_t> result = [&]() -> Result<uint32_t> {
      if (useChunkEngine()) {
        return ChunkEngine::update(*engine_, job);
      }
      if (useUuseEngine()) {
        return updateUuseChunk(job);
      }
      return ChunkReplica::update(chunkStore_, job, executor);
    }();''')
rep(p, '''  if (useChunkEngine()) {
    return ChunkEngine::aioPrepareRead(*engine_, job);
  } else {
    return ChunkReplica::aioPrepareRead(chunkStore_, job);
  }''',
    '''  if (useChunkEngine()) {
    return ChunkEngine::aioPrepareRead(*engine_, job);
  } else if (useUuseEngine()) {
    return makeError(StorageCode::kChunkReadFailed, "uuse read path is not wired yet");
  } else {
    return ChunkReplica::aioPrepareRead(chunkStore_, job);
  }''')
rep(p, '''  if (useChunkEngine()) {
    return ChunkEngine::queryChunks(*engine_, chunkIdRange, chainId());
  }
  return chunkStore_.queryChunks(chunkIdRange);''',
    '''  if (useChunkEngine()) {
    return ChunkEngine::queryChunks(*engine_, chunkIdRange, chainId());
  }
  if (useUuseEngine()) {
    if (!uuseStore_) {
      return makeError(StorageCode::kChunkMetadataNotFound, "uuse store not loaded");
    }
    return uuseStore_->queryChunks(chunkIdRange);
  }
  return chunkStore_.queryChunks(chunkIdRange);''')
rep(p, '''  if (useChunkEngine()) {
    return ChunkEngine::queryChunk(*engine_, chunkId, chainId());
  }
  auto getResult = chunkStore_.get(chunkId);''',
    '''  if (useChunkEngine()) {
    return ChunkEngine::queryChunk(*engine_, chunkId, chainId());
  }
  if (useUuseEngine()) {
    if (!uuseStore_) {
      return makeError(StorageCode::kChunkMetadataNotFound, "uuse store not loaded");
    }
    auto info = uuseStore_->get(chunkId);
    if (!info) {
      return makeError(std::move(info.error()));
    }
    return info.value().meta;
  }
  auto getResult = chunkStore_.get(chunkId);''')

helpers = '''
Result<Void> StorageTarget::openUuse(bool create) {
  boost::system::error_code ec{};
  boost::filesystem::create_directories(targetConfig_.path, ec);
  if (UNLIKELY(ec.failed())) {
    auto msg = fmt::format("target create directory {} failed: {}", targetConfig_.path.string(), ec.message());
    XLOG(ERR, msg);
    return makeError(StorageCode::kChunkOpenFailed, std::move(msg));
  }
  UuseChunkStore::Options options;
  options.uuseConfPath = targetConfig_.uuse_conf;
  options.targetDir = fmt::format("/uuse-store/t{}", targetConfig_.target_id);
  options.targetId = targetConfig_.target_id;
  options.create = create;
  auto store = UuseChunkStore::open(options);
  if (!store) {
    auto msg = fmt::format("uuse store open failed: {}", store.error().message());
    XLOG(ERR, msg);
    return makeError(StorageCode::kStorageInitFailed, std::move(msg));
  }
  uuseStore_ = std::move(store).value();
  XLOGF(INFO, "uuse store {} target {} at {}", create ? "created" : "loaded", targetConfig_.target_id, options.targetDir);
  return Void{};
}

Result<uint32_t> StorageTarget::updateUuseChunk(UpdateJob &job) {
  if (!uuseStore_) {
    return makeError(StorageCode::kStorageInitFailed, "uuse store not loaded");
  }
  const auto &io = job.updateIO();
  UuseChunkStore::UpdateInput input;
  input.chunkId = io.key.chunkId;
  input.chunkSize = io.chunkSize;
  input.offset = io.offset;
  input.length = io.length;
  input.updateVer = io.updateVer.toUnderType();
  input.chainVer = job.commitChainVer().toUnderType();
  input.data = job.state().data;
  input.checksum = io.checksum;
  input.isSyncing = job.options().isSyncing;
  input.isRemove = io.isRemove();
  input.isTruncate = io.isTruncate();
  auto updated = uuseStore_->update(input);
  if (!updated) {
    return makeError(std::move(updated.error()));
  }
  auto &result = job.result();
  result.commitVer = updated.value().commitVer;
  result.updateVer = updated.value().updateVer;
  result.commitChainVer = updated.value().chainVer;
  result.checksum = updated.value().checksum;
  result.lengthInfo = updated.value().length;
  return updated.value().length;
}

Result<uint32_t> StorageTarget::commitUuseChunk(UpdateJob &job) {
  if (!uuseStore_) {
    return makeError(StorageCode::kStorageInitFailed, "uuse store not loaded");
  }
  const auto &io = job.commitIO();
  auto committed = uuseStore_->commit(io.key.chunkId, io.commitVer.toUnderType(), job.commitChainVer().toUnderType());
  if (!committed) {
    return makeError(std::move(committed.error()));
  }
  auto &result = job.result();
  result.commitVer = io.commitVer;
  result.updateVer = io.commitVer;
  result.commitChainVer = job.commitChainVer();
  return 0;
}

}  // namespace hf3fs::storage
'''
close = '}  // namespace hf3fs::storage\n'
t = read(p)
if 'StorageTarget::openUuse' not in t:
    assert t.endswith(close)
    write(p, t[: -len(close)] + helpers)

print('uuse chunkstore wiring installed')
PY

# ---- M2.2 read path ----
python3 - "$ROOT" <<'PY'
import sys
root = sys.argv[1]

def read(p):
    return open(p, encoding='utf-8', errors='replace').read()

def write(p, t):
    open(p, 'w', encoding='utf-8', errors='replace').write(t)

def rep(path, old, new):
    t = read(path)
    if new in t:
        return
    assert old in t, ('anchor missing in %s: %s' % (path, old[:70]))
    write(path, t.replace(old, new, 1))

p = root + '/src/storage/store/StorageTarget.h'
rep(p, '  Result<uint32_t> updateUuseChunk(UpdateJob &job);\n',
    '  Result<uint32_t> updateUuseChunk(UpdateJob &job);\n  Result<uint32_t> readUuseChunk(AioReadJob &job);\n')

p = root + '/src/storage/store/StorageTarget.cc'
rep(p, '''  } else if (useUuseEngine()) {
    return makeError(StorageCode::kChunkReadFailed, "uuse read path is not wired yet");
  } else {''',
    '''  } else if (useUuseEngine()) {
    if (!uuseStore_) {
      return makeError(StorageCode::kStorageInitFailed, "uuse store not loaded");
    }
    auto &result = job.result();
    auto &state = job.state();
    const auto &chunkId = job.readIO().key.chunkId;
    auto info = uuseStore_->get(chunkId);
    if (!info) {
      return makeError(std::move(info.error()));
    }
    const ChunkMetadata &meta = info.value().meta;
    result.commitVer = meta.commitVer;
    result.updateVer = meta.updateVer;
    result.commitChainVer = meta.chainVer;
    state.chunkLen = meta.size;
    state.chunkChecksum = meta.checksum();
    if (UNLIKELY(result.commitVer != result.updateVer && !state.readUncommitted)) {
      return makeError(StorageCode::kChunkNotCommit, "uuse chunk not committed");
    }
    state.readLength = job.alignedLength();
    state.readOffset = meta.innerOffset + job.alignedOffset();
    state.readFd = -1;
    return Void{};
  } else {''')

helpers = '''
Result<uint32_t> StorageTarget::readUuseChunk(AioReadJob &job) {
  if (!uuseStore_) {
    return makeError(StorageCode::kStorageInitFailed, "uuse store not loaded");
  }
  const auto &chunkId = job.readIO().key.chunkId;
  auto info = uuseStore_->get(chunkId);
  if (!info) {
    return makeError(std::move(info.error()));
  }
  const auto &meta = info.value().meta;
  auto &state = job.state();
  const uint64_t alignedOffset = job.alignedOffset();
  if (alignedOffset >= meta.size) {
    return 0;
  }
  const uint32_t available = static_cast<uint32_t>(meta.size - alignedOffset);
  const uint32_t toRead = std::min(state.readLength, available);
  if (toRead == 0) {
    return 0;
  }
  auto read = uuseStore_->readChunk(info.value(), reinterpret_cast<uint8_t *>(state.localbuf.ptr()), toRead,
                                    static_cast<uint32_t>(alignedOffset));
  if (!read) {
    return makeError(std::move(read.error()));
  }
  return toRead;
}

}  // namespace hf3fs::storage
'''
close = '}  // namespace hf3fs::storage\n'
t = read(p)
if 'StorageTarget::readUuseChunk' not in t:
    assert t.endswith(close)
    write(p, t[: -len(close)] + helpers)

p = root + '/src/storage/aio/AioStatus.cc'
rep(p, '''    auto result = job.state().storageTarget->aioPrepareRead(job);
    if (UNLIKELY(!result)) {
      job.setResult(makeError(std::move(result.error())));
      continue;
    }

    ++readyToSubmit_;''',
    '''    auto result = job.state().storageTarget->aioPrepareRead(job);
    if (UNLIKELY(!result)) {
      job.setResult(makeError(std::move(result.error())));
      continue;
    }
    if (UNLIKELY(job.state().storageTarget->useUuseEngine())) {
      auto readResult = job.state().storageTarget->readUuseChunk(job);
      if (UNLIKELY(!readResult)) {
        job.setResult(makeError(std::move(readResult.error())));
        continue;
      }
      setReadJobResult(&job, static_cast<int64_t>(readResult.value()));
      continue;
    }

    ++readyToSubmit_;''')

p = root + '/src/storage/store/StorageTarget.cc'
rep(p, '''Result<Void> StorageTarget::aioFinishRead(AioReadJob &job) {
  if (job.state().chunkEngineJob.has_chunk()) {
    return Void{};
  }
  return ChunkReplica::aioFinishRead(chunkStore_, job);
}''',
    '''Result<Void> StorageTarget::aioFinishRead(AioReadJob &job) {
  if (job.state().chunkEngineJob.has_chunk()) {
    return Void{};
  }
  if (useUuseEngine()) {
    if (!uuseStore_) {
      return makeError(StorageCode::kStorageInitFailed, "uuse store not loaded");
    }
    auto info = uuseStore_->get(job.readIO().key.chunkId);
    if (!info) {
      return makeError(std::move(info.error()));
    }
    if (job.result().updateVer != info.value().meta.updateVer) {
      return makeError(StorageCode::kChunkNotCommit, "uuse chunk version outdated");
    }
    return Void{};
  }
  return ChunkReplica::aioFinishRead(chunkStore_, job);
}''')
print('uuse read path installed')
PY
echo "apply-uuse-chunkstore done"
