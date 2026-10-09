// Copyright 2026 TiKV Project Authors. Licensed under Apache-2.0.

//! UUSE-backed `FileSystem` for raft-engine (UUSE Phase 8, Shard-116).
//!
//! Every file operation goes through the C ABI shim in `tikv/shim/`
//! (`uuse_tikv_shim.h`), which drives the UUSE core on the configured NVMe.
//! Encryption metadata hooks keep the default no-op behavior; encryption is
//! disabled in Phase 8.

use std::ffi::{c_char, c_int, c_void, CString};
use std::io::{self, Read, Seek, SeekFrom, Write};
use std::path::Path;
use std::sync::Arc;

use raft_engine::env::{FileSystem, Handle, Permission, WriteExt};

/* Opaque C ABI handles (see tikv/shim/uuse_tikv_shim.h). */
#[repr(C)]
struct UuseEngineOpaque {
    _private: [u8; 0],
}

#[repr(C)]
struct UuseFileOpaque {
    _private: [u8; 0],
}

const OPEN_CREATE: u32 = 1 << 0;
const OPEN_TRUNCATE: u32 = 1 << 1;

extern "C" {
    fn uuse_tikv_engine_open(conf: *const c_char, out: *mut *mut UuseEngineOpaque) -> c_int;
    fn uuse_tikv_engine_close(engine: *mut UuseEngineOpaque);
    fn uuse_tikv_open(
        engine: *mut UuseEngineOpaque,
        path: *const c_char,
        flags: u32,
        out: *mut *mut UuseFileOpaque,
    ) -> c_int;
    fn uuse_tikv_close(file: *mut UuseFileOpaque);
    fn uuse_tikv_remove(engine: *mut UuseEngineOpaque, path: *const c_char) -> c_int;
    fn uuse_tikv_rename(
        engine: *mut UuseEngineOpaque,
        from: *const c_char,
        to: *const c_char,
    ) -> c_int;
    fn uuse_tikv_create_dirs(engine: *mut UuseEngineOpaque, path: *const c_char) -> c_int;
    fn uuse_tikv_read_dir(
        engine: *mut UuseEngineOpaque,
        path: *const c_char,
        buf: *mut c_char,
        buf_len: usize,
    ) -> i64;
    fn uuse_tikv_pread(
        file: *mut UuseFileOpaque,
        buf: *mut c_void,
        len: usize,
        offset: u64,
    ) -> i64;
    fn uuse_tikv_pwrite(
        file: *mut UuseFileOpaque,
        buf: *const c_void,
        len: usize,
        offset: u64,
    ) -> i64;
    fn uuse_tikv_truncate(file: *mut UuseFileOpaque, size: u64) -> c_int;
    fn uuse_tikv_allocate(file: *mut UuseFileOpaque, offset: u64, len: u64) -> c_int;
    fn uuse_tikv_sync(file: *mut UuseFileOpaque) -> c_int;
    fn uuse_tikv_file_size(file: *mut UuseFileOpaque) -> i64;
}

fn io_error(code: i64) -> io::Error {
    io::Error::from_raw_os_error((-code) as i32)
}

fn c_path(path: &Path) -> io::Result<CString> {
    CString::new(path.to_string_lossy().into_owned())
        .map_err(|_| io::Error::new(io::ErrorKind::InvalidInput, "path contains NUL"))
}

/// UUSE-backed raft-engine filesystem; owns one mounted UUSE engine.
pub struct UuseFileSystem {
    engine: *mut UuseEngineOpaque,
}

unsafe impl Send for UuseFileSystem {}
unsafe impl Sync for UuseFileSystem {}

impl UuseFileSystem {
    /// Mounts the volume described by the UUSE `.conf`.
    pub fn new(conf_path: &str) -> io::Result<Self> {
        let conf = CString::new(conf_path)
            .map_err(|_| io::Error::new(io::ErrorKind::InvalidInput, "conf path contains NUL"))?;
        let mut engine = std::ptr::null_mut();
        let rc = unsafe { uuse_tikv_engine_open(conf.as_ptr(), &mut engine) };
        if rc != 0 {
            return Err(io_error(rc as i64));
        }
        Ok(Self { engine })
    }

    /// Creates every missing directory component of `path`.
    pub fn create_dirs(&self, path: &str) -> io::Result<()> {
        let path = CString::new(path)
            .map_err(|_| io::Error::new(io::ErrorKind::InvalidInput, "path contains NUL"))?;
        let rc = unsafe { uuse_tikv_create_dirs(self.engine, path.as_ptr()) };
        if rc != 0 {
            return Err(io_error(rc as i64));
        }
        Ok(())
    }

    fn open_path(&self, path: &Path, flags: u32) -> io::Result<UuseHandle> {
        let path = c_path(path)?;
        let mut file = std::ptr::null_mut();
        let rc = unsafe { uuse_tikv_open(self.engine, path.as_ptr(), flags, &mut file) };
        if rc != 0 {
            return Err(io_error(rc as i64));
        }
        Ok(UuseHandle { file })
    }
}

impl Drop for UuseFileSystem {
    fn drop(&mut self) {
        unsafe { uuse_tikv_engine_close(self.engine) }
    }
}

/// One open raft-engine file.
pub struct UuseHandle {
    file: *mut UuseFileOpaque,
}

unsafe impl Send for UuseHandle {}
unsafe impl Sync for UuseHandle {}

impl Handle for UuseHandle {
    fn truncate(&self, offset: usize) -> io::Result<()> {
        let rc = unsafe { uuse_tikv_truncate(self.file, offset as u64) };
        if rc != 0 {
            return Err(io_error(rc as i64));
        }
        Ok(())
    }

    fn file_size(&self) -> io::Result<usize> {
        let size = unsafe { uuse_tikv_file_size(self.file) };
        if size < 0 {
            return Err(io_error(size));
        }
        Ok(size as usize)
    }

    fn sync(&self) -> io::Result<()> {
        let rc = unsafe { uuse_tikv_sync(self.file) };
        if rc != 0 {
            return Err(io_error(rc as i64));
        }
        Ok(())
    }
}

impl Drop for UuseHandle {
    fn drop(&mut self) {
        unsafe { uuse_tikv_close(self.file) }
    }
}

/// Positioned reader over a UUSE file.
pub struct UuseReader {
    handle: Arc<UuseHandle>,
    offset: u64,
}

impl Read for UuseReader {
    fn read(&mut self, buf: &mut [u8]) -> io::Result<usize> {
        if buf.is_empty() {
            return Ok(0);
        }
        let n = unsafe {
            uuse_tikv_pread(
                self.handle.file,
                buf.as_mut_ptr() as *mut c_void,
                buf.len(),
                self.offset,
            )
        };
        if n < 0 {
            return Err(io_error(n));
        }
        self.offset += n as u64;
        Ok(n as usize)
    }
}

impl Seek for UuseReader {
    fn seek(&mut self, pos: SeekFrom) -> io::Result<u64> {
        let next = match pos {
            SeekFrom::Start(n) => n as i64,
            SeekFrom::Current(n) => self.offset as i64 + n,
            SeekFrom::End(n) => self.handle.file_size()? as i64 + n,
        };
        if next < 0 {
            return Err(io::Error::new(
                io::ErrorKind::InvalidInput,
                "negative seek offset",
            ));
        }
        self.offset = next as u64;
        Ok(self.offset)
    }
}

/// Positioned writer over a UUSE file.
pub struct UuseWriter {
    handle: Arc<UuseHandle>,
    offset: u64,
}

impl Write for UuseWriter {
    fn write(&mut self, buf: &[u8]) -> io::Result<usize> {
        if buf.is_empty() {
            return Ok(0);
        }
        let n = unsafe {
            uuse_tikv_pwrite(
                self.handle.file,
                buf.as_ptr() as *const c_void,
                buf.len(),
                self.offset,
            )
        };
        if n < 0 {
            return Err(io_error(n));
        }
        self.offset += n as u64;
        Ok(n as usize)
    }

    fn flush(&mut self) -> io::Result<()> {
        Ok(())
    }
}

impl Seek for UuseWriter {
    fn seek(&mut self, pos: SeekFrom) -> io::Result<u64> {
        let next = match pos {
            SeekFrom::Start(n) => n as i64,
            SeekFrom::Current(n) => self.offset as i64 + n,
            SeekFrom::End(n) => self.handle.file_size()? as i64 + n,
        };
        if next < 0 {
            return Err(io::Error::new(
                io::ErrorKind::InvalidInput,
                "negative seek offset",
            ));
        }
        self.offset = next as u64;
        Ok(self.offset)
    }
}

impl WriteExt for UuseWriter {
    fn truncate(&mut self, offset: usize) -> io::Result<()> {
        let rc = unsafe { uuse_tikv_truncate(self.handle.file, offset as u64) };
        if rc != 0 {
            return Err(io_error(rc as i64));
        }
        if self.offset > offset as u64 {
            self.offset = offset as u64;
        }
        Ok(())
    }

    fn allocate(&mut self, offset: usize, size: usize) -> io::Result<()> {
        let rc = unsafe { uuse_tikv_allocate(self.handle.file, offset as u64, size as u64) };
        if rc != 0 {
            return Err(io_error(rc as i64));
        }
        Ok(())
    }
}

impl FileSystem for UuseFileSystem {
    type Handle = UuseHandle;
    type Reader = UuseReader;
    type Writer = UuseWriter;

    fn create<P: AsRef<Path>>(&self, path: P) -> io::Result<UuseHandle> {
        self.open_path(path.as_ref(), OPEN_CREATE | OPEN_TRUNCATE)
    }

    fn open<P: AsRef<Path>>(&self, path: P, _perm: Permission) -> io::Result<UuseHandle> {
        self.open_path(path.as_ref(), 0)
    }

    fn delete<P: AsRef<Path>>(&self, path: P) -> io::Result<()> {
        let path = c_path(path.as_ref())?;
        let rc = unsafe { uuse_tikv_remove(self.engine, path.as_ptr()) };
        if rc != 0 {
            return Err(io_error(rc as i64));
        }
        Ok(())
    }

    fn rename<P: AsRef<Path>>(&self, src_path: P, dst_path: P) -> io::Result<()> {
        let from = c_path(src_path.as_ref())?;
        let to = c_path(dst_path.as_ref())?;
        let rc = unsafe { uuse_tikv_rename(self.engine, from.as_ptr(), to.as_ptr()) };
        if rc != 0 {
            return Err(io_error(rc as i64));
        }
        Ok(())
    }

    fn new_reader(&self, handle: Arc<UuseHandle>) -> io::Result<UuseReader> {
        Ok(UuseReader { handle, offset: 0 })
    }

    fn new_writer(&self, handle: Arc<UuseHandle>) -> io::Result<UuseWriter> {
        Ok(UuseWriter { handle, offset: 0 })
    }

    fn create_dir<P: AsRef<Path>>(&self, path: P) -> io::Result<()> {
        let path = c_path(path.as_ref())?;
        let rc = unsafe { uuse_tikv_create_dirs(self.engine, path.as_ptr()) };
        if rc != 0 {
            return Err(io_error(rc as i64));
        }
        Ok(())
    }

    fn read_dir<P: AsRef<Path>>(&self, path: P) -> io::Result<Vec<String>> {
        let path = c_path(path.as_ref())?;
        let needed =
            unsafe { uuse_tikv_read_dir(self.engine, path.as_ptr(), std::ptr::null_mut(), 0) };
        if needed < 0 {
            return Err(io_error(needed));
        }
        if needed == 0 {
            return Ok(Vec::new());
        }
        let mut buf = vec![0u8; needed as usize];
        let rc = unsafe {
            uuse_tikv_read_dir(
                self.engine,
                path.as_ptr(),
                buf.as_mut_ptr() as *mut c_char,
                buf.len(),
            )
        };
        if rc != 0 {
            return Err(io_error(rc));
        }
        let mut names = Vec::new();
        for part in buf.split(|b| *b == 0) {
            if !part.is_empty() {
                names.push(String::from_utf8_lossy(part).into_owned());
            }
        }
        Ok(names)
    }
}
