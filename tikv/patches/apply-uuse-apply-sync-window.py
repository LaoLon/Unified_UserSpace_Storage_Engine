#!/usr/bin/env python3
"""Shard-120 follow-up: bounded-window apply sync.

Replaces the always-sync block (Shard-118) with a 1 ms window: the kvdb apply
still syncs (bounded durability), but at most once per window, amortizing the
UUSE checkpoint barrier. Explicit sync requests (sync_log_hint) are honored and
the shutdown path syncs unconditionally."""

from __future__ import annotations

import pathlib

TIKV = pathlib.Path("/build/tikv-fork")
REL = "components/raftstore/src/store/fsm/apply.rs"

OLD = (
    "        // UUSE local engine (Phase 8): without a kernel page cache, unsynced\n"
    "        // kv writes do not survive a process exit; always sync apply batches\n"
    "        // so the durable applied index keeps up with raft log compaction\n"
    "        // (Shard-118).\n"
    "        let need_sync = !self.disable_wal;\n"
)

NEW = (
    "        // UUSE local engine (Phase 8): bounded-window apply sync. UUSE has no\n"
    "        // kernel page cache, so unsynced kv writes do not survive a process\n"
    "        // exit; syncing every apply batch is safe but pays one checkpoint\n"
    "        // barrier per batch (Shard-120 trace: 55k barriers/60 s, p50 182 us).\n"
    "        // Sync at most once per window (1 ms) and still honor explicit sync\n"
    "        // requests; the shutdown path syncs unconditionally and raft log GC\n"
    "        // keeps a >=50 entry margin over the applied index, so the window\n"
    "        // stays inside the compaction safety margin.\n"
    "        const UUSE_APPLY_SYNC_WINDOW_NS: u64 = 1_000_000;\n"
    "        static UUSE_APPLY_LAST_SYNC_NS: AtomicU64 = AtomicU64::new(0);\n"
    "        let need_sync = if self.disable_wal {\n"
    "            false\n"
    "        } else if self.sync_log_hint {\n"
    "            true\n"
    "        } else {\n"
    "            let now = timespec_to_ns(monotonic_raw_now());\n"
    "            let last = UUSE_APPLY_LAST_SYNC_NS.load(Ordering::Relaxed);\n"
    "            if now.saturating_sub(last) >= UUSE_APPLY_SYNC_WINDOW_NS {\n"
    "                UUSE_APPLY_LAST_SYNC_NS.store(now, Ordering::Relaxed);\n"
    "                true\n"
    "            } else {\n"
    "                false\n"
    "            }\n"
    "        };\n"
)


def main() -> int:
    path = TIKV / REL
    text = path.read_text(encoding="utf-8")
    count = text.count(OLD)
    if count != 1:
        raise SystemExit(f"anchor count {count} != 1 in {REL}")
    path.write_text(text.replace(OLD, NEW), encoding="utf-8")
    print(f"patched {REL}")
    print("APPLY_SYNC_WINDOW_PATCH_DONE")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
