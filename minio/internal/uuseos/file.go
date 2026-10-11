//go:build cgo

package uuseos

import (
	"io/fs"
	"os"
)

// File is the disk-seam file handle abstraction: it is implemented by the
// UUSE-backed handle and by *os.File (native paths), so cmd call sites can
// carry either without changing their method set. The method set mirrors the
// *os.File methods MinIO uses on storage file handles. Fd() returns an
// invalid descriptor for UUSE handles: callers that really need a host fd
// must not receive one (the O_DIRECT probe is skipped for UUSE drives).
type File interface {
	Read(p []byte) (int, error)
	ReadAt(p []byte, offset int64) (int, error)
	Write(p []byte) (int, error)
	Seek(offset int64, whence int) (int64, error)
	Truncate(size int64) error
	Stat() (fs.FileInfo, error)
	Sync() error
	Name() string
	Fd() uintptr
	Close() error
}

// compile-time checks.
var _ File = (*file)(nil)
var _ File = (*os.File)(nil)

// DirEntry is the disk-seam directory handle abstraction: *os.File for native
// paths, UuseDir for UUSE drives. Only the methods the readDir seam needs are
// declared; minio's stock readDir paths are kept for native files.
type DirEntry interface {
	Readdirnames(n int) ([]string, error)
	Close() error
}
