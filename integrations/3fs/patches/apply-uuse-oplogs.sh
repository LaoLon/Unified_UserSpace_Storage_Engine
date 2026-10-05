#!/bin/bash
# Add per-op success logs to the UUSE backend (update/commit/read) so the
# storage log can be used for op-level tracing against the client matrix.
# Idempotent; applies on top of apply-uuse-chunkstore.sh.
#
# Usage: bash apply-uuse-oplogs.sh <3fs-root>
set -eu
ROOT="${1:?usage: apply-uuse-oplogs.sh <3fs-root>}"

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

p = root + '/src/storage/store/StorageTarget.cc'

# update success log
rep(p, '''  auto &result = job.result();
  result.commitVer = updated.value().commitVer;''',
    '''  XLOGF(INFO, "uuse update ok: chunk {} chunkSize {} offset {} length {} ver {} syncing {} remove {} truncate {} extend {}", io.key.chunkId.toString(), io.chunkSize, io.offset, io.length, io.updateVer.toUnderType(), job.options().isSyncing, io.isRemove(), io.isTruncate(), io.isExtend());
  auto &result = job.result();
  result.commitVer = updated.value().commitVer;''')

# commit success log
rep(p, '''  auto &result = job.result();
  result.commitVer = io.commitVer;''',
    '''  XLOGF(INFO, "uuse commit ok: chunk {} ver {} chainVer {}", io.key.chunkId.toString(), io.commitVer.toUnderType(), job.commitChainVer().toUnderType());
  auto &result = job.result();
  result.commitVer = io.commitVer;''')

# read success log
rep(p, '''  if (!read) {
    return makeError(std::move(read.error()));
  }
  return toRead;
}''',
    '''  if (!read) {
    return makeError(std::move(read.error()));
  }
  XLOGF(INFO, "uuse read ok: chunk {} offset {} len {}", chunkId.toString(), alignedOffset, toRead);
  return toRead;
}''')

print('uuse op logs installed')
PY
