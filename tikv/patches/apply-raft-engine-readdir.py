#!/usr/bin/env python3
"""Shard-116: route raft-engine directory listing/creation through the
FileSystem trait instead of std::fs (host bypass)."""

from __future__ import annotations

import pathlib
import shutil

RE = pathlib.Path("/build/cargo/git/checkouts/raft-engine-aebf03c5dccc7789/3d84405")
BACKUP = pathlib.Path("/build/spike-backup")

TRAIT_METHODS = """
    /// Creates a directory (and parents when supported). The default fails
    /// closed so filesystems without mkdir surface the gap instead of falling
    /// back to the host filesystem.
    fn create_dir<P: AsRef<Path>>(&self, _path: P) -> Result<()> {
        Err(std::io::Error::new(
            std::io::ErrorKind::Unsupported,
            "create_dir is not supported by this filesystem",
        ))
    }

    /// Lists regular file names directly under `path` (non-recursive). The
    /// default fails closed so filesystems without directory listing surface
    /// the gap instead of falling back to the host filesystem.
    fn read_dir<P: AsRef<Path>>(&self, _path: P) -> Result<Vec<String>> {
        Err(std::io::Error::new(
            std::io::ErrorKind::Unsupported,
            "read_dir is not supported by this filesystem",
        ))
    }
"""

DEFAULT_METHODS = """
    fn create_dir<P: AsRef<Path>>(&self, path: P) -> IoResult<()> {
        std::fs::create_dir_all(path)
    }

    fn read_dir<P: AsRef<Path>>(&self, path: P) -> IoResult<Vec<String>> {
        let mut names = Vec::new();
        for entry in std::fs::read_dir(path)? {
            let entry = entry?;
            if entry.file_type()?.is_file() {
                if let Some(name) = entry.file_name().to_str() {
                    names.push(name.to_owned());
                }
            }
        }
        Ok(names)
    }
"""


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
    for rel in (
        "src/env/mod.rs",
        "src/env/default.rs",
        "src/file_pipe_log/mod.rs",
        "src/file_pipe_log/pipe_builder.rs",
    ):
        dst = BACKUP / ("raft_" + rel.replace("/", "_"))
        if not dst.exists():
            shutil.copy2(RE / rel, dst)
            print(f"backup {dst}")

    patch(
        "src/env/mod.rs",
        "    fn new_reader(&self, handle: Arc<Self::Handle>) -> Result<Self::Reader>;\n"
        "\n"
        "    fn new_writer(&self, handle: Arc<Self::Handle>) -> Result<Self::Writer>;\n"
        "}",
        "    fn new_reader(&self, handle: Arc<Self::Handle>) -> Result<Self::Reader>;\n"
        "\n"
        "    fn new_writer(&self, handle: Arc<Self::Handle>) -> Result<Self::Writer>;\n"
        + TRAIT_METHODS
        + "}",
    )

    patch(
        "src/env/default.rs",
        "    fn new_writer(&self, handle: Arc<Self::Handle>) -> IoResult<Self::Writer> {\n"
        "        Ok(LogFile::new(handle))\n"
        "    }\n"
        "}",
        "    fn new_writer(&self, handle: Arc<Self::Handle>) -> IoResult<Self::Writer> {\n"
        "        Ok(LogFile::new(handle))\n"
        "    }\n"
        + DEFAULT_METHODS
        + "}",
    )

    patch(
        "src/file_pipe_log/mod.rs",
        """        pub fn new_directory_reader(system: Arc<F>, dir: &Path) -> Result<Self> {
            if !dir.is_dir() {
                return Err(Error::InvalidArgument(format!(
                    "Not a directory: {}",
                    dir.display()
                )));
            }
            let mut files: Vec<_> = std::fs::read_dir(dir)?
                .filter_map(|e| {
                    if let Ok(e) = e {
                        let p = e.path();
                        if p.is_file() {
                            if let Some(file_id) =
                                FileId::parse_file_name(p.file_name().unwrap().to_str().unwrap())
                            {
                                return Some((file_id, p));
                            }
                        }
                    }
                    None
                })
                .collect();
            files.sort_by_key(|pair| pair.0);
""",
        """        pub fn new_directory_reader(system: Arc<F>, dir: &Path) -> Result<Self> {
            let mut files: Vec<_> = system
                .read_dir(dir)
                .map_err(|e| {
                    Error::InvalidArgument(format!("Not a directory: {}: {}", dir.display(), e))
                })?
                .into_iter()
                .filter_map(|name| {
                    FileId::parse_file_name(&name).map(|file_id| (file_id, dir.join(name)))
                })
                .collect();
            files.sort_by_key(|pair| pair.0);
""",
    )

    patch(
        "src/file_pipe_log/pipe_builder.rs",
        "use std::fs::{self, File as StdFile};",
        "use std::fs::File as StdFile;",
    )

    patch(
        "src/file_pipe_log/pipe_builder.rs",
        """    fn scan_dir(&mut self, dir: &str, lock: bool) -> Result<()> {
        let dir = Path::new(dir);
        if !dir.exists() {
            if lock {
                info!("Create raft log directory: {}", dir.display());
                fs::create_dir(dir)?;
                self.dir_locks.push(lock_dir(dir)?);
            }
            self.dirs.push(dir.to_path_buf());
            return Ok(());
        }
        if !dir.is_dir() {
            return Err(box_err!("Not directory: {}", dir.display()));
        }
        if lock {
            self.dir_locks.push(lock_dir(dir)?);
        }
        self.dirs.push(dir.to_path_buf());
        let path_id = self.dirs.len() - 1;

        fs::read_dir(dir)?.try_for_each(|e| -> Result<()> {
            let dir_entry = e?;
            let p = dir_entry.path();
            if !p.is_file() {
                return Ok(());
            }
            let file_name = p.file_name().unwrap().to_str().unwrap();
""",
        """    fn scan_dir(&mut self, dir: &str, lock: bool) -> Result<()> {
        let dir = Path::new(dir);
        let names = match self.file_system.read_dir(dir) {
            Ok(names) => names,
            Err(e) if e.kind() == std::io::ErrorKind::NotFound => {
                if lock {
                    info!("Create raft log directory: {}", dir.display());
                    self.file_system.create_dir(dir)?;
                    self.dir_locks.push(lock_dir(dir)?);
                }
                self.dirs.push(dir.to_path_buf());
                return Ok(());
            }
            Err(e) => return Err(e.into()),
        };
        if lock {
            self.dir_locks.push(lock_dir(dir)?);
        }
        self.dirs.push(dir.to_path_buf());
        let path_id = self.dirs.len() - 1;

        names.into_iter().try_for_each(|name| -> Result<()> {
            let p = dir.join(name);
            let file_name = p.file_name().unwrap().to_str().unwrap();
""",
    )

    print("RAFT_ENGINE_READDIR_PATCH_DONE")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
