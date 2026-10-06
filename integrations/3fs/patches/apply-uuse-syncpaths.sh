#!/bin/bash
# M3.1 sync/recycle paths for the UUSE backend: getAllMetadata /
# getAllMetadataMap / reportUnrecycledSize / addChunkSize / migrate guard /
# punchHole wiring. Standalone so it can be applied to a tree that was
# installed from an earlier revision of apply-uuse-chunkstore.sh. Idempotent.
#
# Usage: bash apply-uuse-syncpaths.sh <3fs-root>
set -eu
ROOT="${1:?usage: apply-uuse-syncpaths.sh <3fs-root>}"

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
        print('already:', new.splitlines()[0][:60])
        return
    assert old in t, ('anchor missing in %s: %s' % (path, old[:70]))
    write(path, t.replace(old, new, 1))

h = root + '/src/storage/store/StorageTarget.h'

rep(h, '''  // recycle a batch of chunks. return true if all holes are punched.
  Result<bool> punchHole() {
    if (useChunkEngine() || useUuseEngine()) {
      return true;
    } else {
      return chunkStore_.punchHole();
    }
  }''',
'''  // recycle a batch of chunks. return true if all holes are punched.
  Result<bool> punchHole() {
    if (useChunkEngine()) {
      return true;
    } else if (useUuseEngine()) {
      if (!uuseStore_) {
        return makeError(StorageCode::kStorageInitFailed, "uuse store not loaded");
      }
      return uuseStore_->punchHole();
    } else {
      return chunkStore_.punchHole();
    }
  }''')

p = root + '/src/storage/store/StorageTarget.cc'

rep(p, '''Result<Void> StorageTarget::getAllMetadata(ChunkMetaVector &metadataVec) {
  if (useChunkEngine()) {
    return ChunkEngine::getAllMetadata(*engine_, chainId(), metadataVec);
  } else {
    return chunkStore_.getAllMetadata(metadataVec);
  }
}''',
'''Result<Void> StorageTarget::getAllMetadata(ChunkMetaVector &metadataVec) {
  if (useChunkEngine()) {
    return ChunkEngine::getAllMetadata(*engine_, chainId(), metadataVec);
  } else if (useUuseEngine()) {
    if (!uuseStore_) {
      return makeError(StorageCode::kStorageInitFailed, "uuse store not loaded");
    }
    for (auto &entry : uuseStore_->allChunks()) {
      auto &meta = entry.second;
      metadataVec.emplace_back();
      auto &out = metadataVec.back();
      out.chunkId = entry.first;
      out.updateVer = meta.updateVer;
      out.commitVer = meta.commitVer;
      out.chainVer = meta.chainVer;
      out.chunkState = meta.chunkState;
      out.checksum = meta.checksum();
      out.length = meta.size;
    }
    return Void{};
  } else {
    return chunkStore_.getAllMetadata(metadataVec);
  }
}''')

rep(p, '''Result<Void> StorageTarget::getAllMetadataMap(std::unordered_map<ChunkId, ChunkMetadata> &metas) {
  if (useChunkEngine()) {
    return ChunkEngine::getAllMetadataMap(*engine_, metas, chainId());
  } else {''',
'''Result<Void> StorageTarget::getAllMetadataMap(std::unordered_map<ChunkId, ChunkMetadata> &metas) {
  if (useChunkEngine()) {
    return ChunkEngine::getAllMetadataMap(*engine_, metas, chainId());
  } else if (useUuseEngine()) {
    if (!uuseStore_) {
      return makeError(StorageCode::kStorageInitFailed, "uuse store not loaded");
    }
    for (auto &entry : uuseStore_->allChunks()) {
      metas[entry.first] = entry.second;
    }
    return Void{};
  } else {''')

rep(p, '''Result<Void> StorageTarget::reportUnrecycledSize() {
  targetUsedSize_->set(usedSize());

  if (useChunkEngine()) {
    return Void{};
  }''',
'''Result<Void> StorageTarget::reportUnrecycledSize() {
  targetUsedSize_->set(usedSize());

  if (useChunkEngine() || useUuseEngine()) {
    return Void{};
  }''')

rep(p, '''Result<Void> StorageTarget::addChunkSize(const std::vector<Size> &sizeList) {
  if (useChunkEngine()) {
    return Void{};
  }''',
'''Result<Void> StorageTarget::addChunkSize(const std::vector<Size> &sizeList) {
  if (useChunkEngine() || useUuseEngine()) {
    return Void{};
  }''')

rep(p, '''  if (!targetConfig_.only_chunk_engine && config_.migrate_kv_store() &&
      config_.kv_store().type() != targetConfig_.kv_store_type) {''',
'''  if (!targetConfig_.only_chunk_engine && !targetConfig_.use_uuse_engine && config_.migrate_kv_store() &&
      config_.kv_store().type() != targetConfig_.kv_store_type) {''')
print('uuse sync paths installed')
PY
