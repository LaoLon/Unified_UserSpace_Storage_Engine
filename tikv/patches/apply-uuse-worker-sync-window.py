#!/usr/bin/env python3
"""Shard-120 follow-up: windowed sync for the async write worker.

The worker persists the apply state (kvdb) and the raft log with hardcoded
sync=true per raft append; on UUSE each sync is a checkpoint barrier. Apply the
same 5 ms bounded window here (shutdown still syncs unconditionally)."""

from __future__ import annotations

import pathlib
import shutil

TIKV = pathlib.Path("/build/tikv-fork")
BACKUP = pathlib.Path("/build/spike-backup")
REL = "components/raftstore/src/store/async_io/write.rs"

HELPER = (
    "/* UUSE local engine (Phase 8): bounded-window write sync for the async\n"
    " * write worker. UUSE has no kernel page cache; syncing every raft append\n"
    " * (apply state + raft log) pays one checkpoint barrier per append. Sync at\n"
    " * most once per window (5 ms); the shutdown path syncs unconditionally and\n"
    " * raft log GC keeps a >=50 entry margin over the applied index. */\n"
    "fn uuse_write_sync_window() -> bool {\n"
    "    const UUSE_WRITE_SYNC_WINDOW_NS: u64 = 5_000_000;\n"
    "    static UUSE_WRITE_LAST_SYNC_NS: AtomicU64 = AtomicU64::new(0);\n"
    "    let now = timespec_to_ns(monotonic_raw_now());\n"
    "    let last = UUSE_WRITE_LAST_SYNC_NS.load(Ordering::Relaxed);\n"
    "    if now.saturating_sub(last) >= UUSE_WRITE_SYNC_WINDOW_NS {\n"
    "        UUSE_WRITE_LAST_SYNC_NS.store(now, Ordering::Relaxed);\n"
    "        true\n"
    "    } else {\n"
    "        false\n"
    "    }\n"
    "}\n"
    "\n"
)


def patch(old: str, new: str, count: int = 1) -> None:
    path = TIKV / REL
    text = path.read_text(encoding="utf-8")
    found = text.count(old)
    if found != count:
        raise SystemExit(f"anchor count {found} != {count} for {old[:60]!r}")
    path.write_text(text.replace(old, new), encoding="utf-8")
    print(f"patched: {old[:60]!r}")


def main() -> int:
    BACKUP.mkdir(parents=True, exist_ok=True)
    dst = BACKUP / ("worker_sync_" + REL.replace("/", "_"))
    if not dst.exists():
        shutil.copy2(TIKV / REL, dst)
        print(f"backup {dst}")

    # 1) Insert the window helper before the worker's write_to_db.
    patch(
        "    pub fn write_to_db(&mut self, notify: bool) {\n",
        HELPER + "    pub fn write_to_db(&mut self, notify: bool) {\n",
    )

    # 2) kvdb (apply state) write: sync per window instead of always.
    patch(
        "                let mut write_opts = WriteOptions::new();\n"
        "                write_opts.set_sync(true);\n"
        "                // TODO: Add perf context\n",
        "                let mut write_opts = WriteOptions::new();\n"
        "                write_opts.set_sync(Self::uuse_write_sync_window());\n"
        "                // TODO: Add perf context\n",
    )

    # 3) raft log write: sync per window instead of always.
    patch(
        "                    .consume_and_shrink(\n"
        "                        &mut self.batch.raft_wbs[i],\n"
        "                        true,\n"
        "                        RAFT_WB_SHRINK_SIZE,\n",
        "                    .consume_and_shrink(\n"
        "                        &mut self.batch.raft_wbs[i],\n"
        "                        Self::uuse_write_sync_window(),\n"
        "                        RAFT_WB_SHRINK_SIZE,\n",
    )

    print("WORKER_SYNC_WINDOW_PATCH_DONE")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
