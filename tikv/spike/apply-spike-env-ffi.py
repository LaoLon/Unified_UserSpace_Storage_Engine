#!/usr/bin/env python3
"""Shard-116 spike: patch the rust-rocksdb checkout with a path-redirecting
crocksdb Env constructor and add a TiKV example that proves the chain."""

from __future__ import annotations

import pathlib
import shutil
import sys

CO = pathlib.Path("/build/cargo/git/checkouts/rust-rocksdb-882d3d9019f8b391/64a092f")
BACKUP = pathlib.Path("/build/spike-backup")
TIKV = pathlib.Path("/build/tikv-fork")

CXX_BLOCK = r"""
namespace {

/* Spike FileSystem for the TiKV/UUSE integration (Shard-116): prefixes every
 * path with `base_` and delegates to the default POSIX FileSystem. Proves the
 * crocksdb Env(FileSystem) injection chain before the real UUSE-backed
 * FileSystem lands. */
class UuseSpikeFileSystem : public rocksdb::FileSystemWrapper {
 public:
  explicit UuseSpikeFileSystem(std::string base)
      : rocksdb::FileSystemWrapper(rocksdb::FileSystem::Default()),
        base_(std::move(base)) {}

  rocksdb::IOStatus NewSequentialFile(
      const std::string& f, const rocksdb::FileOptions& o,
      std::unique_ptr<rocksdb::FSSequentialFile>* r,
      rocksdb::IODebugContext* dbg) override {
    return target()->NewSequentialFile(Map(f), o, r, dbg);
  }
  rocksdb::IOStatus NewRandomAccessFile(
      const std::string& f, const rocksdb::FileOptions& o,
      std::unique_ptr<rocksdb::FSRandomAccessFile>* r,
      rocksdb::IODebugContext* dbg) override {
    return target()->NewRandomAccessFile(Map(f), o, r, dbg);
  }
  rocksdb::IOStatus NewWritableFile(
      const std::string& f, const rocksdb::FileOptions& o,
      std::unique_ptr<rocksdb::FSWritableFile>* r,
      rocksdb::IODebugContext* dbg) override {
    return target()->NewWritableFile(Map(f), o, r, dbg);
  }
  rocksdb::IOStatus ReopenWritableFile(
      const std::string& f, const rocksdb::FileOptions& o,
      std::unique_ptr<rocksdb::FSWritableFile>* r,
      rocksdb::IODebugContext* dbg) override {
    return target()->ReopenWritableFile(Map(f), o, r, dbg);
  }
  rocksdb::IOStatus NewDirectory(
      const std::string& name, const rocksdb::IOOptions& o,
      std::unique_ptr<rocksdb::FSDirectory>* r,
      rocksdb::IODebugContext* dbg) override {
    return target()->NewDirectory(Map(name), o, r, dbg);
  }
  rocksdb::IOStatus FileExists(const std::string& f,
                               const rocksdb::IOOptions& o,
                               rocksdb::IODebugContext* dbg) override {
    return target()->FileExists(Map(f), o, dbg);
  }
  rocksdb::IOStatus GetChildren(const std::string& d,
                                const rocksdb::IOOptions& o,
                                std::vector<std::string>* r,
                                rocksdb::IODebugContext* dbg) override {
    return target()->GetChildren(Map(d), o, r, dbg);
  }
  rocksdb::IOStatus GetChildrenFileAttributes(
      const std::string& d, const rocksdb::IOOptions& o,
      std::vector<rocksdb::FileAttributes>* r,
      rocksdb::IODebugContext* dbg) override {
    return target()->GetChildrenFileAttributes(Map(d), o, r, dbg);
  }
  rocksdb::IOStatus DeleteFile(const std::string& f,
                               const rocksdb::IOOptions& o,
                               rocksdb::IODebugContext* dbg) override {
    return target()->DeleteFile(Map(f), o, dbg);
  }
  rocksdb::IOStatus Truncate(const std::string& f, size_t size,
                             const rocksdb::IOOptions& o,
                             rocksdb::IODebugContext* dbg) override {
    return target()->Truncate(Map(f), size, o, dbg);
  }
  rocksdb::IOStatus CreateDir(const std::string& d,
                              const rocksdb::IOOptions& o,
                              rocksdb::IODebugContext* dbg) override {
    return target()->CreateDir(Map(d), o, dbg);
  }
  rocksdb::IOStatus CreateDirIfMissing(const std::string& d,
                                       const rocksdb::IOOptions& o,
                                       rocksdb::IODebugContext* dbg) override {
    return target()->CreateDirIfMissing(Map(d), o, dbg);
  }
  rocksdb::IOStatus DeleteDir(const std::string& d,
                              const rocksdb::IOOptions& o,
                              rocksdb::IODebugContext* dbg) override {
    return target()->DeleteDir(Map(d), o, dbg);
  }
  rocksdb::IOStatus GetFileSize(const std::string& f,
                                const rocksdb::IOOptions& o, uint64_t* size,
                                rocksdb::IODebugContext* dbg) override {
    return target()->GetFileSize(Map(f), o, size, dbg);
  }
  rocksdb::IOStatus GetFileModificationTime(
      const std::string& f, const rocksdb::IOOptions& o, uint64_t* time,
      rocksdb::IODebugContext* dbg) override {
    return target()->GetFileModificationTime(Map(f), o, time, dbg);
  }
  rocksdb::IOStatus RenameFile(const std::string& s, const std::string& t,
                               const rocksdb::IOOptions& o,
                               rocksdb::IODebugContext* dbg) override {
    return target()->RenameFile(Map(s), Map(t), o, dbg);
  }
  rocksdb::IOStatus LinkFile(const std::string& s, const std::string& t,
                             const rocksdb::IOOptions& o,
                             rocksdb::IODebugContext* dbg) override {
    return target()->LinkFile(Map(s), Map(t), o, dbg);
  }
  rocksdb::IOStatus LockFile(const std::string& f,
                             const rocksdb::IOOptions& o,
                             rocksdb::FileLock** l,
                             rocksdb::IODebugContext* dbg) override {
    return target()->LockFile(Map(f), o, l, dbg);
  }
  rocksdb::IOStatus UnlockFile(rocksdb::FileLock* l,
                               const rocksdb::IOOptions& o,
                               rocksdb::IODebugContext* dbg) override {
    return target()->UnlockFile(l, o, dbg);
  }

 private:
  std::string Map(const std::string& f) const { return base_ + f; }
  std::string base_;
};

}  // namespace

crocksdb_env_t* crocksdb_uuse_spike_env_create(const char* base_path) {
  auto fs = std::make_shared<UuseSpikeFileSystem>(
      std::string(base_path == nullptr ? "" : base_path));
  crocksdb_env_t* result = new crocksdb_env_t;
  result->rep = rocksdb::NewCompositeEnv(fs).release();
  result->is_default = false;
  result->block_cipher = nullptr;
  result->encryption_provider = nullptr;
  return result;
}
"""

EXAMPLE = r"""//! Shard-116 spike: open a DB through a crocksdb Env whose FileSystem
//! redirects every path under a base directory (proves the Env(fs) chain).
use std::sync::Arc;

use rocksdb::{DB, Env, Options};

fn main() -> Result<(), Box<dyn std::error::Error>> {
    let args: Vec<String> = std::env::args().collect();
    let base = args
        .get(1)
        .cloned()
        .unwrap_or_else(|| "/build/uuse-spike-root".to_string());
    let db_path = args
        .get(2)
        .cloned()
        .unwrap_or_else(|| "/build/uuse-spike-db".to_string());

    let env = Arc::new(Env::uuse_spike(&base)?);
    let mut opts = Options::default();
    opts.create_if_missing(true);
    opts.set_env(env.clone());

    let db = DB::open(&opts, &db_path)?;
    db.put(b"k1", b"v1")?;
    assert_eq!(db.get(b"k1")?, Some(b"v1".to_vec()));
    db.flush()?;
    drop(db);

    let mapped = format!("{}{}", base, db_path);
    println!("spike ok: mapped dir {}", mapped);
    let mut names: Vec<String> = std::fs::read_dir(&mapped)?
        .map(|e| e.unwrap().file_name().to_string_lossy().into_owned())
        .collect();
    names.sort();
    println!("files: {:?}", names);
    Ok(())
}
"""


def patch(path: pathlib.Path, anchor: str, insert: str, before: bool) -> None:
    text = path.read_text(encoding="utf-8")
    count = text.count(anchor)
    if count != 1:
        raise SystemExit(f"anchor count {count} != 1 in {path}")
    replacement = insert + anchor if before else anchor + insert
    path.write_text(text.replace(anchor, replacement, 1), encoding="utf-8")
    print(f"patched {path}")


def main() -> int:
    BACKUP.mkdir(parents=True, exist_ok=True)
    for rel in ("librocksdb_sys/crocksdb/crocksdb/c.h", "librocksdb_sys/crocksdb/c.cc",
                "librocksdb_sys/src/lib.rs", "src/rocksdb.rs"):
        src = CO / rel
        dst = BACKUP / rel.replace("/", "_")
        if not dst.exists():
            shutil.copy2(src, dst)
            print(f"backup {dst}")

    patch(
        CO / "librocksdb_sys/crocksdb/crocksdb/c.h",
        "extern C_ROCKSDB_LIBRARY_API crocksdb_env_t* crocksdb_default_env_create();\n",
        "extern C_ROCKSDB_LIBRARY_API crocksdb_env_t* crocksdb_uuse_spike_env_create(\n"
        "    const char* base_path);\n",
        before=False,
    )

    cxx_anchor = (
        "crocksdb_env_t* crocksdb_default_env_create() {\n"
        "  crocksdb_env_t* result = new crocksdb_env_t;\n"
        "  result->rep = Env::Default();\n"
        "  result->block_cipher = nullptr;\n"
        "  result->encryption_provider = nullptr;\n"
        "  result->is_default = true;\n"
        "  return result;\n"
        "}\n"
    )
    patch(CO / "librocksdb_sys/crocksdb/c.cc", cxx_anchor, CXX_BLOCK, before=False)

    patch(
        CO / "librocksdb_sys/src/lib.rs",
        "    pub fn crocksdb_default_env_create() -> *mut DBEnv;\n",
        "    pub fn crocksdb_uuse_spike_env_create(base_path: *const c_char) -> *mut DBEnv;\n",
        before=False,
    )

    rust_method = (
        "    // Spike (Shard-116): Env whose FileSystem redirects every path under\n"
        "    // `base_path`. Proves the crocksdb Env(FileSystem) injection chain.\n"
        "    pub fn uuse_spike(base_path: &str) -> Result<Env, String> {\n"
        "        let path = std::ffi::CString::new(base_path).map_err(|e| e.to_string())?;\n"
        "        let env = unsafe { crocksdb_ffi::crocksdb_uuse_spike_env_create(path.as_ptr()) };\n"
        "        if env.is_null() {\n"
        '            return Err("uuse spike env create failed".to_owned());\n'
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

    example_dir = TIKV / "components/engine_rocks/examples"
    example_dir.mkdir(parents=True, exist_ok=True)
    (example_dir / "uuse_env_spike.rs").write_text(EXAMPLE, encoding="utf-8")
    print(f"wrote {example_dir / 'uuse_env_spike.rs'}")
    print("SPIKE_PATCH_DONE")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
