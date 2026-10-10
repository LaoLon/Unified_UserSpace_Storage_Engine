#!/usr/bin/env python3
"""Shard-118: make raft-engine `Engine::sync()` actually sync.

`Engine::write` early-returns on empty batches, so the documented
"sync through an empty write" path was a no-op; TiKV's shutdown sync (UUSE
integration) therefore never reached the log file. Sync the append queue
directly, matching the writer's own sync path."""

from __future__ import annotations

import pathlib
import shutil

RE = pathlib.Path("/build/cargo/git/checkouts/raft-engine-aebf03c5dccc7789/3d84405")
BACKUP = pathlib.Path("/build/spike-backup")


def patch(rel: str, old: str, new: str) -> None:
    path = RE / rel
    text = path.read_text(encoding="utf-8")
    count = text.count(old)
    if count != 1:
        raise SystemExit(f"anchor count {count} != 1 in {rel}")
    path.write_text(text.replace(old, new, 1), encoding="utf-8")
    print(f"patched {rel}")


def main() -> int:
    BACKUP.mkdir(parents=True, exist_ok=True)
    rel = "src/engine.rs"
    dst = BACKUP / ("raft_sync_" + rel.replace("/", "_"))
    if not dst.exists():
        shutil.copy2(RE / rel, dst)
        print(f"backup {dst}")

    patch(
        rel,
        "    /// Synchronizes the Raft engine.\n"
        "    pub fn sync(&self) -> Result<()> {\n"
        "        self.write(&mut LogBatch::default(), true)?;\n"
        "        Ok(())\n"
        "    }",
        "    /// Synchronizes the Raft engine.\n"
        "    pub fn sync(&self) -> Result<()> {\n"
        "        // `write` early-returns on empty batches, so syncing through an\n"
        "        // empty batch was a no-op; sync the append queue directly\n"
        "        // (Shard-118 UUSE integration).\n"
        "        self.pipe_log.sync(LogQueue::Append)?;\n"
        "        Ok(())\n"
        "    }",
    )
    print("RAFT_ENGINE_SYNC_PATCH_DONE")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
