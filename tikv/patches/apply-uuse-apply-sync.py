#!/usr/bin/env python3
"""Shard-118: always sync apply batches (kvdb WAL) under the UUSE integration.

Without a kernel page cache, TiKV's async kv writes do not survive a process
exit, so the durable applied index falls behind the (admin-synced) raft log
compaction and the next start fails with "entry at apply index ... doesn't
exist". Syncing every apply batch keeps the durable applied index in step with
the log GC, at the cost of one kvdb fsync per apply batch."""

from __future__ import annotations

import pathlib
import shutil

TIKV = pathlib.Path("/build/tikv-fork")
BACKUP = pathlib.Path("/build/spike-backup")


def patch(rel: str, old: str, new: str) -> None:
    path = TIKV / rel
    text = path.read_text(encoding="utf-8")
    count = text.count(old)
    if count != 1:
        raise SystemExit(f"anchor count {count} != 1 in {rel}")
    path.write_text(text.replace(old, new, 1), encoding="utf-8")
    print(f"patched {rel}")


def main() -> int:
    BACKUP.mkdir(parents=True, exist_ok=True)
    rel = "components/raftstore/src/store/fsm/apply.rs"
    dst = BACKUP / ("apply_sync_" + rel.replace("/", "_"))
    if not dst.exists():
        shutil.copy2(TIKV / rel, dst)
        print(f"backup {dst}")

    patch(
        rel,
        "        let need_sync = self.sync_log_hint && !self.disable_wal;\n",
        "        // UUSE local engine (Phase 8): bounded-window apply sync. UUSE has no\n"
        "        // kernel page cache, so unsynced kv writes do not survive a process\n"
        "        // exit; syncing every apply batch is safe but pays one checkpoint\n"
        "        // barrier per batch (Shard-120 trace: 55k barriers/60 s, p50 182 us).\n"
        "        // Sync at most once per window (5 ms) and still honor explicit sync\n"
        "        // requests; the shutdown path syncs unconditionally and raft log GC\n"
        "        // keeps a >=50 entry margin over the applied index, so the window\n"
        "        // stays inside the compaction safety margin.\n"
        "        const UUSE_APPLY_SYNC_WINDOW_NS: u64 = 5_000_000;\n"
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
        "        };\n",
    )
    print("UUSE_APPLY_SYNC_PATCH_DONE")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
