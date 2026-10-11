//go:build !cgo

// Package uuseos provides the MinIO disk-seam abstraction over the UUSE shim.
// This file is the non-cgo fallback: without cgo the package compiles to pure
// stock behavior (no UUSE drives can be mounted).
package uuseos

import (
	"io/fs"
	"os"
)

// File is the disk-seam file handle abstraction (stock-only build).
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

// DirEntry is the directory handle abstraction (stock-only build).
type DirEntry interface {
	Readdirnames(n int) ([]string, error)
	Close() error
}

var _ File = (*os.File)(nil)
var _ DirEntry = (*os.File)(nil)

// RegisteredParent is always false without cgo.
func RegisteredParent(path string) bool { return false }

// Registered is always false without cgo.
func Registered(root string) bool { return false }

// OpenDir routes to os.
func OpenDir(path string, flag int, perm os.FileMode) (DirEntry, error) {
	return os.OpenFile(path, flag, perm)
}

// Mounted is always false without cgo.
func Mounted() bool { return false }

// OpenFile routes to os.
func OpenFile(path string, flag int, perm os.FileMode) (*os.File, File, error) {
	f, err := os.OpenFile(path, flag, perm)
	return f, f, err
}

// ReadFile routes to os.
func ReadFile(path string) ([]byte, error) { return os.ReadFile(path) }

// Stat routes to os.
func Stat(path string) (fs.FileInfo, error) { return os.Stat(path) }

// Lstat routes to os.
func Lstat(path string) (fs.FileInfo, error) { return os.Lstat(path) }

// MkdirAll routes to os.
func MkdirAll(path string, perm os.FileMode) error { return os.MkdirAll(path, perm) }

// Remove routes to os.
func Remove(path string) error { return os.Remove(path) }

// RemoveAll routes to os.
func RemoveAll(path string) error { return os.RemoveAll(path) }

// Rename routes to os.
func Rename(from, to string) error { return os.Rename(from, to) }

// ReadDirFn routes to os.ReadDir.
func ReadDirFn(path string, fn func(name string, mode os.FileMode) error) error {
	entries, err := os.ReadDir(path)
	if err != nil {
		if os.IsNotExist(err) {
			return nil
		}
		return err
	}
	for _, entry := range entries {
		mode := os.FileMode(0)
		if entry.IsDir() {
			mode = os.ModeDir
		}
		if err := fn(entry.Name(), mode); err != nil {
			return err
		}
	}
	return nil
}

// ReadDirN routes to os.ReadDir.
func ReadDirN(path string, count int) ([]string, error) {
	entries, err := os.ReadDir(path)
	if err != nil {
		return nil, err
	}
	var names []string
	for _, entry := range entries {
		name := entry.Name()
		if entry.IsDir() {
			name += "/"
		}
		names = append(names, name)
		if count > 0 && len(names) >= count {
			break
		}
	}
	return names, nil
}
