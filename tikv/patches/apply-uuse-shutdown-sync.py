#!/usr/bin/env python3
"""Shard-118: sync both engines during TiKV shutdown so their tails are durable
before process exit.

The UUSE shim's engine destructor does not run on process exit, and TiKV's
normal writes are async (raft log `sync=false`, kvdb apply `need_sync=false`);
host ext4 keeps unsynced writes in the page cache, UUSE does not. Without an
explicit sync the next start fails with "entry at apply index ... doesn't
exist" (the raft log is compacted past the rolled-back kvdb applied index).
Also keeps the engines in `tikv` (the signal thread gets a clone) so this
shutdown path can still reach them."""

from __future__ import annotations

import pathlib
import shutil

TIKV = pathlib.Path("/build/tikv-fork")
BACKUP = pathlib.Path("/build/spike-backup")


def patch(rel: str, old: str, new: str) -> None:
    path = TIKV / rel
    text = path.read_text(encoding="utf-8")
    found = text.count(old)
    if found != 1:
        raise SystemExit(f"anchor count {found} != 1 in {rel}")
    path.write_text(text.replace(old, new, 1), encoding="utf-8")
    print(f"patched {rel}")


def main() -> int:
    BACKUP.mkdir(parents=True, exist_ok=True)
    rel = "components/server/src/server.rs"
    dst = BACKUP / ("shutdown_sync_" + rel.replace("/", "_"))
    if not dst.exists():
        shutil.copy2(TIKV / rel, dst)
        print(f"backup {dst}")

    patch(
        rel,
        "        let engines = tikv.engines.take().unwrap().engines;\n"
        "        let kv_statistics = tikv.kv_statistics.clone();",
        "        // UUSE local engine (Phase 8): keep the engines in `tikv` so the\n"
        "        // shutdown path can sync the raft log before they are dropped;\n"
        "        // the signal thread only needs a clone for stats dumps.\n"
        "        let engines = tikv.engines.as_ref().unwrap().engines.clone();\n"
        "        let kv_statistics = tikv.kv_statistics.clone();",
    )

    patch(
        rel,
        "        servers.raft_server.stop();\n"
        "        self.region_info_accessor.as_ref().unwrap().stop();",
        "        servers.raft_server.stop();\n"
        "        // UUSE local engine (Phase 8): process exit does not run the shim's\n"
        "        // engine destructor, so the raft log tail must be synced here;\n"
        "        // entries written with sync=false are otherwise lost (UUSE has no\n"
        "        // kernel page cache) and the next start fails with \"entry at apply\n"
        "        // index ... doesn't exist\" (Shard-118).\n"
        "        if let Some(engines) = self.engines.as_ref() {\n"
        "            info!(\"UUSE shutdown: syncing kv engine before exit\");\n"
        "            match KvEngine::sync(&engines.engines.kv) {\n"
        "                Ok(()) => info!(\"UUSE shutdown: kv engine synced\"),\n"
        "                Err(e) => {\n"
        "                    error!(\"failed to sync kv engine on shutdown\"; \"err\" => ?e);\n"
        "                }\n"
        "            }\n"
        "            info!(\"UUSE shutdown: syncing raft engine before exit\");\n"
        "            match engines.engines.raft.sync() {\n"
        "                Ok(()) => info!(\"UUSE shutdown: raft engine synced\"),\n"
        "                Err(e) => {\n"
        "                    error!(\"failed to sync raft engine on shutdown\"; \"err\" => ?e);\n"
        "                }\n"
        "            }\n"
        "        } else {\n"
        "            info!(\"UUSE shutdown: engines already released, skipping engine sync\");\n"
        "        }\n"
        "        self.region_info_accessor.as_ref().unwrap().stop();",
    )
    print("SHUTDOWN_SYNC_PATCH_DONE")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
