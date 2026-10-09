#!/usr/bin/env python3
"""Shard-117: share one UUSE engine between kvdb (RocksDB env) and raft-engine.
- librocksdb_sys build: include the shim header dir + link libuuse_tikv_shim.
- crocksdb_uuse_env_create: use the shim singleton engine + adapter external FS."""

from __future__ import annotations

import pathlib
import shutil

CO = pathlib.Path("/build/cargo/git/checkouts/rust-rocksdb-882d3d9019f8b391/64a092f")
BACKUP = pathlib.Path("/build/spike-backup")


def patch(rel: str, old: str, new: str, count: int = 1) -> None:
    path = CO / rel
    text = path.read_text(encoding="utf-8")
    found = text.count(old)
    if found != count:
        raise SystemExit(f"anchor count {found} != {count} in {rel}")
    path.write_text(text.replace(old, new), encoding="utf-8")
    print(f"patched {rel} ({count})")


def main() -> int:
    BACKUP.mkdir(parents=True, exist_ok=True)
    for rel in ("librocksdb_sys/build.rs", "librocksdb_sys/crocksdb/c.cc"):
        dst = BACKUP / ("shared_" + rel.replace("/", "_"))
        if not dst.exists():
            shutil.copy2(CO / rel, dst)
            print(f"backup {dst}")

    patch(
        "librocksdb_sys/build.rs",
        '    build.include(&uuse_root);\n',
        '    build.include(&uuse_root);\n'
        '    build.include(format!("{}/tikv/shim", uuse_root));\n',
    )

    patch(
        "librocksdb_sys/build.rs",
        '    println!("cargo:rustc-link-lib=dylib=uuse_core");\n',
        '    println!("cargo:rustc-link-lib=dylib=uuse_core");\n'
        '    println!("cargo:rustc-link-lib=dylib=uuse_tikv_shim");\n',
    )

    patch(
        "librocksdb_sys/crocksdb/c.cc",
        '#include "rocksdb/uuse_filesystem.hpp"\n',
        '#include "rocksdb/uuse_filesystem.hpp"\n'
        '#include "uuse_tikv_shim.hpp"\n',
    )

    patch(
        "librocksdb_sys/crocksdb/c.cc",
        "#include <limits>\n",
        "#include <limits>\n#include <mutex>\n#include <vector>\n",
    )

    old_fn = (
        "crocksdb_env_t* crocksdb_uuse_env_create(const char* conf_path) {\n"
        "  auto fs = uuse::rocksdb_adapter::createFileSystem(\n"
        '      std::string(conf_path == nullptr ? "" : conf_path));\n'
        "  crocksdb_env_t* result = new crocksdb_env_t;\n"
        "  result->rep = rocksdb::NewCompositeEnv(fs).release();\n"
        "  result->is_default = false;\n"
        "  result->block_cipher = nullptr;\n"
        "  result->encryption_provider = nullptr;\n"
        "  return result;\n"
        "}\n"
    )
    new_fn = (
        "crocksdb_env_t* crocksdb_uuse_env_create(const char* conf_path) {\n"
        "  if (conf_path == nullptr) return nullptr;\n"
        "  uuse_tikv_engine* engine_handle = nullptr;\n"
        "  const int rc = uuse_tikv_engine_open(conf_path, &engine_handle);\n"
        "  if (rc != 0 || engine_handle == nullptr) return nullptr;\n"
        "  /* Process-lifetime keepalive: kvdb and raft-engine share this engine. */\n"
        "  static std::mutex uuse_engine_mutex;\n"
        "  static std::vector<uuse_tikv_engine*> uuse_engine_handles;\n"
        "  {\n"
        "    std::lock_guard<std::mutex> lock(uuse_engine_mutex);\n"
        "    uuse_engine_handles.push_back(engine_handle);\n"
        "  }\n"
        "  auto engine = uuse_tikv_engine_shared_engine(engine_handle);\n"
        "  auto fs = uuse::rocksdb_adapter::createFileSystemWithEngine(engine);\n"
        "  if (fs == nullptr) return nullptr;\n"
        "  crocksdb_env_t* result = new crocksdb_env_t;\n"
        "  result->rep = rocksdb::NewCompositeEnv(fs).release();\n"
        "  result->is_default = false;\n"
        "  result->block_cipher = nullptr;\n"
        "  result->encryption_provider = nullptr;\n"
        "  return result;\n"
        "}\n"
    )
    patch("librocksdb_sys/crocksdb/c.cc", old_fn, new_fn)

    print("SHARED_ENGINE_PATCH_DONE")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
