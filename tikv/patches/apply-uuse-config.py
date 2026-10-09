#!/usr/bin/env python3
"""Shard-116: add `raft_engine.uuse_conf` config gating and route the two
production raft-engine creation paths through RaftLogEngine::new_with_uuse."""

from __future__ import annotations

import pathlib
import shutil

TIKV = pathlib.Path("/build/tikv-fork")
BACKUP = pathlib.Path("/build/spike-backup")


def patch(rel: str, old: str, new: str, count: int = 1) -> None:
    path = TIKV / rel
    text = path.read_text(encoding="utf-8")
    found = text.count(old)
    if found != count:
        raise SystemExit(f"anchor count {found} != {count} in {rel}")
    path.write_text(text.replace(old, new), encoding="utf-8")
    print(f"patched {rel} ({count})")


def main() -> int:
    BACKUP.mkdir(parents=True, exist_ok=True)
    for rel in ("src/config/mod.rs", "components/server/src/common.rs"):
        dst = BACKUP / ("cfg_" + rel.replace("/", "_"))
        if not dst.exists():
            shutil.copy2(TIKV / rel, dst)
            print(f"backup {dst}")

    patch(
        "src/config/mod.rs",
        "pub struct RaftEngineConfig {\n"
        "    pub enable: bool,\n"
        "    #[serde(flatten)]\n"
        "    config: RawRaftEngineConfig,\n"
        "}",
        "pub struct RaftEngineConfig {\n"
        "    pub enable: bool,\n"
        "    #[serde(flatten)]\n"
        "    config: RawRaftEngineConfig,\n"
        "    /// UUSE local engine (Phase 8): when set, raft logs live on the UUSE\n"
        "    /// volume described by this `.conf` (encryption must stay disabled).\n"
        "    #[serde(default)]\n"
        "    pub uuse_conf: Option<String>,\n"
        "}",
    )

    patch(
        "src/config/mod.rs",
        "        Self {\n"
        "            enable: true,\n"
        "            config: RawRaftEngineConfig::default(),\n"
        "        }",
        "        Self {\n"
        "            enable: true,\n"
        "            config: RawRaftEngineConfig::default(),\n"
        "            uuse_conf: None,\n"
        "        }",
    )

    patch(
        "components/server/src/common.rs",
        "            let raft_engine =\n"
        "                RaftLogEngine::new(config.raft_engine.config(), key_manager.clone(), None)\n"
        '                    .expect("failed to open raft engine for migration");',
        "            let raft_engine = match config.raft_engine.uuse_conf.as_deref() {\n"
        "                Some(conf) => RaftLogEngine::new_with_uuse(\n"
        "                    config.raft_engine.config(),\n"
        "                    key_manager.clone(),\n"
        "                    None,\n"
        "                    conf,\n"
        "                ),\n"
        "                None => {\n"
        "                    RaftLogEngine::new(config.raft_engine.config(), key_manager.clone(), None)\n"
        "                }\n"
        "            }\n"
        '            .expect("failed to open raft engine for migration");',
    )

    patch(
        "components/server/src/common.rs",
        "        let raft_config = config.raft_engine.config();\n"
        "        let raft_engine =\n"
        "            RaftLogEngine::new(raft_config, key_manager.clone(), get_io_rate_limiter())\n"
        '                .expect("failed to open raft engine");',
        "        let raft_config = config.raft_engine.config();\n"
        "        let raft_engine = match config.raft_engine.uuse_conf.as_deref() {\n"
        "            Some(conf) => RaftLogEngine::new_with_uuse(\n"
        "                raft_config,\n"
        "                key_manager.clone(),\n"
        "                get_io_rate_limiter(),\n"
        "                conf,\n"
        "            ),\n"
        "            None => RaftLogEngine::new(raft_config, key_manager.clone(), get_io_rate_limiter()),\n"
        "        }\n"
        '        .expect("failed to open raft engine");',
    )

    print("UUSE_CONFIG_PATCH_DONE")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
