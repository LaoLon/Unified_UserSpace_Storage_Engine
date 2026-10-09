#!/usr/bin/env python3
"""Shard-117 prep: add `storage.uuse_conf` and select the UUSE RocksDB env in
the shared-env factory."""

from __future__ import annotations

import pathlib
import shutil

TIKV = pathlib.Path("/build/tikv-fork")
BACKUP = pathlib.Path("/build/spike-backup")

UUSE_ENV_FN = """
/// Creates the shared RocksDB env backed by the UUSE volume described by
/// `conf_path` (Phase 8). Encryption is not supported with the UUSE base; a
/// key manager fails closed.
pub fn get_env_with_uuse(
    key_manager: Option<std::sync::Arc<DataKeyManager>>,
    limiter: Option<std::sync::Arc<::file_system::IoRateLimiter>>,
    conf_path: &str,
) -> engine_traits::Result<std::sync::Arc<raw::Env>> {
    if key_manager.is_some() {
        return Err(engine_traits::Error::Engine(
            engine_traits::Status::with_error(
                engine_traits::Code::NotSupported,
                "UUSE RocksDB env does not support encryption",
            ),
        ));
    }
    let base = std::sync::Arc::new(rocksdb::Env::uuse(conf_path).map_err(|e| {
        engine_traits::Error::Engine(engine_traits::Status::with_error(
            engine_traits::Code::IoError,
            e,
        ))
    })?);
    file_system::get_env(Some(base), limiter)
}
"""


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
    for rel in ("src/storage/config.rs", "components/engine_rocks/src/lib.rs",
                "src/config/mod.rs"):
        dst = BACKUP / ("rocks_" + rel.replace("/", "_"))
        if not dst.exists():
            shutil.copy2(TIKV / rel, dst)
            print(f"backup {dst}")

    patch(
        "src/storage/config.rs",
        "pub struct Config {\n"
        "    #[online_config(skip)]\n"
        "    pub data_dir: String,\n",
        "pub struct Config {\n"
        "    #[online_config(skip)]\n"
        "    pub data_dir: String,\n"
        "    /// UUSE local engine (Phase 8): when set, the shared RocksDB env\n"
        "    /// (kvdb and raftdb) stores every file on the UUSE volume described\n"
        "    /// by this `.conf`. Encryption must stay disabled.\n"
        "    #[online_config(skip)]\n"
        "    pub uuse_conf: Option<String>,\n",
    )

    patch(
        "src/storage/config.rs",
        "            data_dir: DEFAULT_DATA_DIR.to_owned(),\n"
        "            engine: EngineType::RaftKv,\n",
        "            data_dir: DEFAULT_DATA_DIR.to_owned(),\n"
        "            uuse_conf: None,\n"
        "            engine: EngineType::RaftKv,\n",
    )

    patch(
        "components/engine_rocks/src/lib.rs",
        "pub fn get_env(\n"
        "    key_manager: Option<std::sync::Arc<DataKeyManager>>,\n"
        "    limiter: Option<std::sync::Arc<::file_system::IoRateLimiter>>,\n"
        ") -> engine_traits::Result<std::sync::Arc<raw::Env>> {\n"
        "    let env = encryption::get_env(None /* base_env */, key_manager)?;\n"
        "    file_system::get_env(env, limiter)\n"
        "}",
        "pub fn get_env(\n"
        "    key_manager: Option<std::sync::Arc<DataKeyManager>>,\n"
        "    limiter: Option<std::sync::Arc<::file_system::IoRateLimiter>>,\n"
        ") -> engine_traits::Result<std::sync::Arc<raw::Env>> {\n"
        "    let env = encryption::get_env(None /* base_env */, key_manager)?;\n"
        "    file_system::get_env(env, limiter)\n"
        "}\n"
        + UUSE_ENV_FN,
    )

    patch(
        "src/config/mod.rs",
        "    get_env,\n",
        "    get_env,\n    get_env_with_uuse,\n",
    )

    patch(
        "src/config/mod.rs",
        "        let env = get_env(key_manager, limiter)?;\n",
        "        let env = match self.storage.uuse_conf.as_deref() {\n"
        "            Some(conf) => get_env_with_uuse(key_manager, limiter, conf)?,\n"
        "            None => get_env(key_manager, limiter)?,\n"
        "        };\n",
    )

    print("ROCKSDB_CONFIG_PATCH_DONE")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
