#!/usr/bin/env python3
"""Shard-116 main: integrate the real UUSE FileSystem adapter into the
rust-rocksdb build and expose crocksdb_uuse_env_create / Env::uuse."""

from __future__ import annotations

import pathlib
import shutil

CO = pathlib.Path("/build/cargo/git/checkouts/rust-rocksdb-882d3d9019f8b391/64a092f")
BACKUP = pathlib.Path("/build/spike-backup")
TIKV = pathlib.Path("/build/tikv-fork")

EXAMPLE = r"""//! Shard-116: open a DB whose FileSystem is the real UUSE adapter
//! (`uuse::rocksdb_adapter::createFileSystem`). Requires the device bound to
//! uio_pci_generic and a formatted UUSE volume (uusectl format --yes).
use std::sync::Arc;

use rocksdb::{DBOptions, Env, FlushOptions, Writable, DB};

fn main() -> Result<(), Box<dyn std::error::Error>> {
    let args: Vec<String> = std::env::args().collect();
    let conf = args
        .get(1)
        .cloned()
        .unwrap_or_else(|| "/root/code/config/uuse_nvme.conf".to_string());
    let db_path = args
        .get(2)
        .cloned()
        .unwrap_or_else(|| "/uuse-spike-db".to_string());

    let env = Arc::new(Env::uuse(&conf)?);
    let mut opts = DBOptions::new();
    opts.create_if_missing(true);
    opts.set_env(env.clone());

    let db = DB::open(opts, &db_path)?;
    db.put(b"k1", b"v1")?;
    match db.get(b"k1")? {
        Some(value) => assert_eq!(value.to_vec(), b"v1".to_vec()),
        None => panic!("missing key after put"),
    }
    db.flush(&FlushOptions::default())?;
    drop(db);
    println!("uuse env ok: db={}", db_path);
    Ok(())
}
"""


def patch(path: pathlib.Path, anchor: str, insert: str, before: bool = False) -> None:
    text = path.read_text(encoding="utf-8")
    count = text.count(anchor)
    if count != 1:
        raise SystemExit(f"anchor count {count} != 1 in {path}")
    replacement = insert + anchor if before else anchor + insert
    path.write_text(text.replace(anchor, replacement, 1), encoding="utf-8")
    print(f"patched {path}")


def main() -> int:
    BACKUP.mkdir(parents=True, exist_ok=True)
    for rel in ("librocksdb_sys/build.rs", "librocksdb_sys/crocksdb/c.cc",
                "librocksdb_sys/crocksdb/crocksdb/c.h", "librocksdb_sys/src/lib.rs",
                "src/rocksdb.rs"):
        src = CO / rel
        dst = BACKUP / ("main_" + rel.replace("/", "_"))
        if not dst.exists():
            shutil.copy2(src, dst)
            print(f"backup {dst}")

    patch(
        CO / "librocksdb_sys/build.rs",
        '    build.include(cur_dir.join("libtitan_sys").join("titan"));\n',
        "    // UUSE 2.0 integration (Shard-116): adapter sources live in the UUSE tree.\n"
        '    let uuse_root = env::var("UUSE_CORE_ROOT").unwrap_or_else(|_| "/root/code".to_string());\n'
        '    build.include(format!("{}/include", uuse_root));\n'
        "    build.include(&uuse_root);\n",
    )

    adapter_cc = (
        "    // UUSE 2.0 adapter (Shard-116): compiled against the vendored RocksDB\n"
        "    // headers and linked against libuuse_core.so from the UUSE build tree.\n"
        '    let cur_dir = env::current_dir().unwrap();\n'
        '    let uuse_root = env::var("UUSE_CORE_ROOT").unwrap_or_else(|_| "/root/code".to_string());\n'
        '    let uuse_build = env::var("UUSE_CORE_BUILD")\n'
        '        .unwrap_or_else(|_| "/root/code/build-uuse".to_string());\n'
        "    let mut adapter = Build::new();\n"
        "    adapter\n"
        "        .cpp(true)\n"
        '        .file(format!("{}/rocksdb/uuse_filesystem.cpp", uuse_root))\n'
        '        .flag("-std=c++20")\n'
        '        .flag("-fexceptions")\n'
        '        .flag("-fno-rtti")\n'
        '        .include(cur_dir.join("rocksdb").join("include"))\n'
        '        .include(cur_dir.join("rocksdb"))\n'
        '        .include(format!("{}/include", uuse_root))\n'
        "        .include(&uuse_root);\n"
        '    adapter.warnings(false).compile("libuuse_filesystem_adapter.a");\n'
        '    println!("cargo:rustc-link-search=native={}", uuse_build);\n'
        '    println!("cargo:rustc-link-lib=dylib=uuse_core");\n'
        '    println!("cargo:rerun-if-changed={}/rocksdb/uuse_filesystem.cpp", uuse_root);\n'
        '    println!("cargo:rerun-if-changed={}/rocksdb/uuse_filesystem.hpp", uuse_root);\n'
    )
    patch(
        CO / "librocksdb_sys/build.rs",
        '    link_cpp(&mut build);\n    build.warnings(false).compile("libcrocksdb.a");\n',
        adapter_cc,
    )

    patch(
        CO / "librocksdb_sys/crocksdb/c.cc",
        '#include "rocksdb/file_system.h"\n',
        '#include "rocksdb/uuse_filesystem.hpp"\n',
    )

    spike_fn = (
        "crocksdb_env_t* crocksdb_uuse_spike_env_create(const char* base_path) {\n"
        "  auto fs = std::make_shared<UuseSpikeFileSystem>(\n"
        '      std::string(base_path == nullptr ? "" : base_path));\n'
        "  crocksdb_env_t* result = new crocksdb_env_t;\n"
        "  result->rep = rocksdb::NewCompositeEnv(fs).release();\n"
        "  result->is_default = false;\n"
        "  result->block_cipher = nullptr;\n"
        "  result->encryption_provider = nullptr;\n"
        "  return result;\n"
        "}\n"
    )
    real_fn = (
        "\n"
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
    patch(CO / "librocksdb_sys/crocksdb/c.cc", spike_fn, real_fn)

    patch(
        CO / "librocksdb_sys/crocksdb/crocksdb/c.h",
        "extern C_ROCKSDB_LIBRARY_API crocksdb_env_t* crocksdb_uuse_spike_env_create(\n"
        "    const char* base_path);\n",
        "extern C_ROCKSDB_LIBRARY_API crocksdb_env_t* crocksdb_uuse_env_create(\n"
        "    const char* conf_path);\n",
    )

    patch(
        CO / "librocksdb_sys/src/lib.rs",
        "    pub fn crocksdb_uuse_spike_env_create(base_path: *const c_char) -> *mut DBEnv;\n",
        "    pub fn crocksdb_uuse_env_create(conf_path: *const c_char) -> *mut DBEnv;\n",
    )

    rust_method = (
        "    // Real UUSE-backed Env (Shard-116): the FileSystem stores every file\n"
        "    // on the UUSE volume described by `conf_path`.\n"
        "    pub fn uuse(conf_path: &str) -> Result<Env, String> {\n"
        "        let path = std::ffi::CString::new(conf_path).map_err(|e| e.to_string())?;\n"
        "        let env = unsafe { crocksdb_ffi::crocksdb_uuse_env_create(path.as_ptr()) };\n"
        "        if env.is_null() {\n"
        '            return Err("uuse env create failed".to_owned());\n'
        "        }\n"
        "        Ok(Env {\n"
        "            inner: env,\n"
        "            base: None,\n"
        "        })\n"
        "    }\n"
        "\n"
    )
    patch(
        CO / "src/rocksdb.rs",
        "    // Create a ctr encrypted env with a given base env and a given ciper text.\n",
        rust_method,
        before=True,
    )

    # The librocksdb_sys FFI declaration is renamed above; update the spike
    # method's call site so the crate still compiles (the spike example is not
    # part of the product path).
    rs = CO / "src/rocksdb.rs"
    rs_text = rs.read_text(encoding="utf-8")
    rs_old = "crocksdb_ffi::crocksdb_uuse_spike_env_create(path.as_ptr())"
    rs_new = "crocksdb_ffi::crocksdb_uuse_env_create(path.as_ptr())"
    if rs_text.count(rs_old) == 1:
        rs.write_text(rs_text.replace(rs_old, rs_new), encoding="utf-8")
        print("renamed spike FFI call site")

    example_dir = TIKV / "components/engine_rocks/examples"
    example_dir.mkdir(parents=True, exist_ok=True)
    (example_dir / "uuse_env_real.rs").write_text(EXAMPLE, encoding="utf-8")
    print(f"wrote {example_dir / 'uuse_env_real.rs'}")
    print("UUSE_FS_PATCH_DONE")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
