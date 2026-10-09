#!/usr/bin/env python3
"""Shard-117: skip the host-path raft data set validation when a UUSE conf is
configured (engine data lives on the UUSE volume)."""

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
    rel = "src/config/mod.rs"
    dst = BACKUP / ("val_" + rel.replace("/", "_"))
    if not dst.exists():
        shutil.copy2(TIKV / rel, dst)
        print(f"backup {dst}")

    patch(
        rel,
        "        RaftDataStateMachine::new(\n"
        "            &self.storage.data_dir,\n"
        "            &self.raft_store.raftdb_path,\n"
        "            &self.raft_engine.config.dir,\n"
        "        )\n"
        "        .validate(kv_data_exists)?;",
        "        // UUSE local engine (Phase 8): engine data lives on the UUSE volume,\n"
        "        // so the host-path raft data set validation does not apply.\n"
        "        if self.storage.uuse_conf.is_none() && self.raft_engine.uuse_conf.is_none() {\n"
        "            RaftDataStateMachine::new(\n"
        "                &self.storage.data_dir,\n"
        "                &self.raft_store.raftdb_path,\n"
        "                &self.raft_engine.config.dir,\n"
        "            )\n"
        "            .validate(kv_data_exists)?;\n"
        "        }",
    )
    print("VALIDATION_SKIP_PATCH_DONE")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
