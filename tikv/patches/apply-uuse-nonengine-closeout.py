#!/usr/bin/env python3
"""Shard-117: close out non-engine host bypasses under the UUSE config.
- skip host data_dir/LOCK (UUSE engine process lock is the exclusion mechanism)
- skip host reserve-space placeholder (host reserve does not protect the UUSE
  volume) and warn instead."""

from __future__ import annotations

import pathlib
import shutil

TIKV = pathlib.Path("/build/tikv-fork")
BACKUP = pathlib.Path("/build/spike-backup")
REL = "components/server/src/common.rs"


def patch(old: str, new: str) -> None:
    path = TIKV / REL
    text = path.read_text(encoding="utf-8")
    found = text.count(old)
    if found != 1:
        raise SystemExit(f"anchor count {found} != 1")
    path.write_text(text.replace(old, new, 1), encoding="utf-8")
    print("patched common.rs")


OLD_LOCK = (
    '        let lock_path = self.store_path.join(Path::new("LOCK"));\n'
    "\n"
    "        let f = File::create(lock_path.as_path())\n"
    '            .unwrap_or_else(|e| fatal!("failed to create lock at {}: {}", lock_path.display(), e));\n'
    "        if f.try_lock_exclusive().is_err() {\n"
    "            fatal!(\n"
    '                "lock {} failed, maybe another instance is using this directory.",\n'
    "                self.store_path.display()\n"
    "            );\n"
    "        }\n"
    "        self.lock_files.push(f);\n"
)

NEW_LOCK = (
    "        // UUSE local engine (Phase 8, Shard-117): the UUSE engine process\n"
    "        // lock is the exclusion mechanism; skip the host data_dir/LOCK.\n"
    "        let uuse_enabled = self.config.storage.uuse_conf.is_some()\n"
    "            || self.config.raft_engine.uuse_conf.is_some();\n"
    "        if !uuse_enabled {\n"
    '            let lock_path = self.store_path.join(Path::new("LOCK"));\n'
    "\n"
    "            let f = File::create(lock_path.as_path())\n"
    '                .unwrap_or_else(|e| fatal!("failed to create lock at {}: {}", lock_path.display(), e));\n'
    "            if f.try_lock_exclusive().is_err() {\n"
    "                fatal!(\n"
    '                    "lock {} failed, maybe another instance is using this directory.",\n'
    "                    self.store_path.display()\n"
    "                );\n"
    "            }\n"
    "            self.lock_files.push(f);\n"
    "        }\n"
)

OLD_RESERVE = (
    "        let (disk_cap, disk_avail) =\n"
    "            disk::get_disk_space_stats(&self.config.storage.data_dir).unwrap();\n"
    "        let mut capacity = disk_cap;\n"
    "        if self.config.raft_store.capacity.0 > 0 {\n"
    "            capacity = cmp::min(capacity, self.config.raft_store.capacity.0);\n"
    "        }\n"
    "        // reserve space for kv engine\n"
    "        let kv_reserved_size =\n"
    "            calculate_reserved_space(capacity, self.config.storage.reserve_space.0);\n"
    "        disk::set_disk_reserved_space(kv_reserved_size);\n"
    "        reserve_physical_space(&self.config.storage.data_dir, disk_avail, kv_reserved_size);\n"
    "\n"
    "        let raft_data_dir = if self.config.raft_engine.enable {\n"
    "            self.config.raft_engine.config().dir\n"
    "        } else {\n"
    "            self.config.raft_store.raftdb_path.clone()\n"
    "        };\n"
    "\n"
    "        let separated_raft_mount_path =\n"
    "            path_in_diff_mount_point(&self.config.storage.data_dir, &raft_data_dir);\n"
    "        if separated_raft_mount_path {\n"
    "            let (raft_disk_cap, raft_disk_avail) =\n"
    "                disk::get_disk_space_stats(&raft_data_dir).unwrap();\n"
    "            // reserve space for raft engine if raft engine is deployed separately\n"
    "            let raft_reserved_size =\n"
    "                calculate_reserved_space(raft_disk_cap, self.config.storage.reserve_raft_space.0);\n"
    "            disk::set_raft_disk_reserved_space(raft_reserved_size);\n"
    "            reserve_physical_space(&raft_data_dir, raft_disk_avail, raft_reserved_size);\n"
    "        }\n"
)

NEW_RESERVE = (
    "        if uuse_enabled {\n"
    '            warn!("UUSE local engine enabled; host space placeholder reservation is skipped");\n'
    "        } else {\n"
    "            let (disk_cap, disk_avail) =\n"
    "                disk::get_disk_space_stats(&self.config.storage.data_dir).unwrap();\n"
    "            let mut capacity = disk_cap;\n"
    "            if self.config.raft_store.capacity.0 > 0 {\n"
    "                capacity = cmp::min(capacity, self.config.raft_store.capacity.0);\n"
    "            }\n"
    "            // reserve space for kv engine\n"
    "            let kv_reserved_size =\n"
    "                calculate_reserved_space(capacity, self.config.storage.reserve_space.0);\n"
    "            disk::set_disk_reserved_space(kv_reserved_size);\n"
    "            reserve_physical_space(&self.config.storage.data_dir, disk_avail, kv_reserved_size);\n"
    "\n"
    "            let raft_data_dir = if self.config.raft_engine.enable {\n"
    "                self.config.raft_engine.config().dir\n"
    "            } else {\n"
    "                self.config.raft_store.raftdb_path.clone()\n"
    "            };\n"
    "\n"
    "            let separated_raft_mount_path =\n"
    "                path_in_diff_mount_point(&self.config.storage.data_dir, &raft_data_dir);\n"
    "            if separated_raft_mount_path {\n"
    "                let (raft_disk_cap, raft_disk_avail) =\n"
    "                    disk::get_disk_space_stats(&raft_data_dir).unwrap();\n"
    "                // reserve space for raft engine if raft engine is deployed separately\n"
    "                let raft_reserved_size =\n"
    "                    calculate_reserved_space(raft_disk_cap, self.config.storage.reserve_raft_space.0);\n"
    "                disk::set_raft_disk_reserved_space(raft_reserved_size);\n"
    "                reserve_physical_space(&raft_data_dir, raft_disk_avail, raft_reserved_size);\n"
    "            }\n"
    "        }\n"
)


def main() -> int:
    BACKUP.mkdir(parents=True, exist_ok=True)
    dst = BACKUP / "initfs_common.rs"
    if not dst.exists():
        shutil.copy2(TIKV / REL, dst)
        print(f"backup {dst}")

    patch(OLD_LOCK, NEW_LOCK)
    patch(OLD_RESERVE, NEW_RESERVE)
    print("NONENGINE_CLOSEOUT_PATCH_DONE")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
