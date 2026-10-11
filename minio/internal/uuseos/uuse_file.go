//go:build cgo

package uuseos

/*
#cgo CFLAGS: -I${SRCDIR}/../../minio/shim
#cgo LDFLAGS: -L${SRCDIR}/../../minio/shim -luuse_minio_shim -lstdc++ -lpthread

#include <stdlib.h>
#include "uuse_minio_shim.h"
*/
import "C"

import (
	"io"
	"io/fs"
	"syscall"
	"time"
	"unsafe"
)

// file wraps one open UUSE file handle.
type file struct {
	handle      *C.uuse_minio_file
	offset      int64 // sequential cursor used by Read/Write (positional API)
	syncOnClose bool  // O_DSYNC/O_SYNC: barrier at Close (writeAllInternal pattern)
	appendMode  bool  // O_APPEND: write at EOF each call (POSIX append semantics)
}

// cOpenFile opens path through the shim; the returned File is backed by UUSE.
func (u *UuseFs) cOpenFile(path string, flag int, perm fs.FileMode) (*file, error) {
	cPath := C.CString(path)
	defer C.free(unsafe.Pointer(cPath))
	var cFlags C.uint
	if flag&syscall.O_CREAT != 0 {
		cFlags |= C.UUSE_MINIO_OPEN_CREATE
	}
	if flag&syscall.O_EXCL != 0 {
		cFlags |= C.UUSE_MINIO_OPEN_EXCLUSIVE
	}
	if flag&syscall.O_TRUNC != 0 {
		cFlags |= C.UUSE_MINIO_OPEN_TRUNCATE
	}
	errbuf := make([]byte, int(C.UUSE_MINIO_ERRBUF))
	var handle *C.uuse_minio_file
	rc := C.uuse_minio_open(u.eng.handle, cPath, cFlags, &handle,
		(*C.char)(unsafe.Pointer(&errbuf[0])), C.size_t(len(errbuf)))
	if rc != 0 {
		return nil, errnoError(int(rc), "uuse open "+path, errbuf)
	}
	f := &file{handle: handle}
	if flag&(syscall.O_DSYNC|syscall.O_SYNC) != 0 {
		f.syncOnClose = true
	}
	if flag&syscall.O_APPEND != 0 {
		f.appendMode = true
	}
	return f, nil
}

func (f *file) Read(b []byte) (int, error) {
	if len(b) == 0 {
		return 0, nil
	}
	rc := C.uuse_minio_pread(f.handle, unsafe.Pointer(&b[0]), C.size_t(len(b)), C.uint64_t(f.offset))
	if rc < 0 {
		return 0, errnoError(int(rc), "uuse read", nil)
	}
	if rc == 0 {
		return 0, io.EOF
	}
	f.offset += int64(rc)
	return int(rc), nil
}

func (f *file) ReadAt(b []byte, offset int64) (int, error) {
	if len(b) == 0 {
		return 0, nil
	}
	rc := C.uuse_minio_pread(f.handle, unsafe.Pointer(&b[0]), C.size_t(len(b)), C.uint64_t(offset))
	if rc < 0 {
		return 0, errnoError(int(rc), "uuse pread", nil)
	}
	return int(rc), nil
}

func (f *file) Write(b []byte) (int, error) {
	if f.appendMode {
		st, err := f.Stat()
		if err != nil {
			return 0, err
		}
		f.offset = st.Size()
	}
	total := 0
	for total < len(b) {
		rc := C.uuse_minio_pwrite(f.handle, unsafe.Pointer(&b[total]), C.size_t(len(b)-total), C.uint64_t(f.offset))
		if rc < 0 {
			return total, errnoError(int(rc), "uuse write", nil)
		}
		if rc == 0 {
			return total, errnoError(-int(syscall.EIO), "uuse write", nil)
		}
		total += int(rc)
		f.offset += int64(rc)
	}
	return total, nil
}

func (f *file) WriteAt(b []byte, offset int64) (int, error) {
	if len(b) == 0 {
		return 0, nil
	}
	rc := C.uuse_minio_pwrite(f.handle, unsafe.Pointer(&b[0]), C.size_t(len(b)), C.uint64_t(offset))
	if rc < 0 {
		return 0, errnoError(int(rc), "uuse pwrite", nil)
	}
	return int(rc), nil
}

func (f *file) Seek(offset int64, whence int) (int64, error) {
	var size int64
	if st, err := f.Stat(); err == nil {
		size = st.Size()
	} else {
		return 0, err
	}
	var next int64
	switch whence {
	case io.SeekStart:
		next = offset
	case io.SeekCurrent:
		next = offset // the shim is positional: relative seeks are refused below
		return 0, errnoError(-int(syscall.ENOTSUP), "uuse seek", nil)
	case io.SeekEnd:
		next = size + offset
	default:
		return 0, errnoError(-int(syscall.EINVAL), "uuse seek", nil)
	}
	if next < 0 {
		return 0, errnoError(-int(syscall.EINVAL), "uuse seek", nil)
	}
	f.offset = next
	return next, nil
}

func (f *file) Truncate(size int64) error {
	rc := C.uuse_minio_ftruncate(f.handle, C.uint64_t(size))
	if rc != 0 {
		return errnoError(int(rc), "uuse truncate", nil)
	}
	// A truncate resets the sequential cursor to the new size; UUSE create
	// with O_TRUNC rewrites from offset 0, matching os.File semantics.
	f.offset = size
	return nil
}

func (f *file) Sync() error {
	rc := C.uuse_minio_fdatasync(f.handle)
	if rc != 0 {
		return errnoError(int(rc), "uuse sync", nil)
	}
	return nil
}

func (f *file) Stat() (fs.FileInfo, error) {
	var cStat C.uuse_minio_stat
	rc := C.uuse_minio_fstat(f.handle, &cStat)
	if rc != 0 {
		return nil, errnoError(int(rc), "uuse fstat", nil)
	}
	return statFromC(&cStat, ""), nil
}

func (f *file) Close() error {
	if f.handle == nil {
		return nil
	}
	var syncErr error
	if f.syncOnClose {
		// O_DSYNC/O_SYNC: make the write durable before reporting success.
		syncErr = f.Sync()
	}
	C.uuse_minio_close(f.handle)
	f.handle = nil
	return syncErr
}

// Name returns an empty name for UUSE handles.
func (f *file) Name() string { return "" }

// Fd returns an invalid descriptor: UUSE handles have no host fd. Callers
// that need one are out of contract on UUSE drives (probe paths are skipped).
func (f *file) Fd() uintptr { return ^uintptr(0) }

// statFromC converts the C stat snapshot into an os.FileInfo-like value.
func statFromC(cStat *C.uuse_minio_stat, name string) fs.FileInfo {
	mode := fs.FileMode(0o666)
	if cStat.directory != 0 {
		mode |= fs.ModeDir
	}
	return fileStat{
		name:    name,
		size:    int64(cStat.logical_size),
		mode:    mode,
		modTime: time.Unix(0, int64(cStat.mtime_ns)),
		sys:     nil,
	}
}

// fileStat is the minimal fs.FileInfo implementation for UUSE entries.
type fileStat struct {
	name    string
	size    int64
	mode    fs.FileMode
	modTime time.Time
	sys     any
}

func (s fileStat) Name() string       { return s.name }
func (s fileStat) Size() int64        { return s.size }
func (s fileStat) Mode() fs.FileMode  { return s.mode }
func (s fileStat) ModTime() time.Time { return s.modTime }
func (s fileStat) IsDir() bool        { return s.mode.IsDir() }
func (s fileStat) Sys() any           { return s.sys }

// cPathStat runs the path-level shim stat and returns the raw C snapshot.
func (u *UuseFs) cPathStat(path string) (*C.uuse_minio_stat, error) {
	cPath := C.CString(path)
	defer C.free(unsafe.Pointer(cPath))
	errbuf := make([]byte, int(C.UUSE_MINIO_ERRBUF))
	var cStat C.uuse_minio_stat
	rc := C.uuse_minio_path_stat(u.eng.handle, cPath, &cStat,
		(*C.char)(unsafe.Pointer(&errbuf[0])), C.size_t(len(errbuf)))
	if rc != 0 {
		return nil, errnoError(int(rc), "uuse stat "+path, errbuf)
	}
	return &cStat, nil
}
