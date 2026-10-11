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
	"errors"
	"io"
	"io/fs"
	"os"
	"strings"
	"sync"
	"syscall"
	"unsafe"
)

// readFileUuse reads a whole file through the shim (registered drives only).
func ReadFileUuse(path string) (File, error) {
	fsys := lookup(path)
	if fsys == nil {
		return nil, errors.New("uuseos: not a registered drive path: " + path)
	}
	return fsys.cOpenFile(fsys.namespacePath(path), os.O_RDONLY, 0)
}

// UuseFs is one registered UUSE drive: an engine handle plus the host path
// prefix MinIO uses for that drive. All operations translate the MinIO path
// into the UUSE namespace and never fall back to the host filesystem.
type UuseFs struct {
	eng  *engine
	root string // cleaned absolute host path of the drive root
}

var (
	registryMu sync.RWMutex
	registry   = make(map[string]*UuseFs)
)

// NewUuseFs mounts the .conf engine and registers the drive root prefix.
// Returns nil when root is not an absolute path.
func NewUuseFs(confPath, root string) (*UuseFs, error) {
	cleanRoot := strings.TrimSuffix(root, "/")
	if cleanRoot == "" || cleanRoot[0] != '/' {
		return nil, errors.New("uuseos: drive root must be an absolute path: " + root)
	}
	eng, err := openEngine(confPath)
	if err != nil {
		return nil, err
	}
	fs := &UuseFs{eng: eng, root: cleanRoot}
	registryMu.Lock()
	registry[cleanRoot] = fs
	registryMu.Unlock()
	return fs, nil
}

// Close unregisters the drive and releases the engine handle.
func (u *UuseFs) Close() {
	if u == nil {
		return
	}
	registryMu.Lock()
	if registry[u.root] == u {
		delete(registry, u.root)
	}
	registryMu.Unlock()
	if u.eng != nil {
		u.eng.Close()
		u.eng = nil
	}
}

// lookup returns the UuseFs registered for the longest prefix of path.
func lookup(path string) *UuseFs {
	registryMu.RLock()
	if len(registry) != 0 {
		var best *UuseFs
		for root, fs := range registry {
			if path == root || strings.HasPrefix(path, root+"/") {
				if best == nil || len(root) > len(best.root) {
					best = fs
				}
			}
		}
		if best != nil {
			registryMu.RUnlock()
			return best
		}
	}
	registryMu.RUnlock()

	// Not registered yet: mount on first use when the path is declared in
	// MINIO_UUSE_DRIVES (idempotent; MinIO may touch the drive before the
	// explicit mount hook runs).
	root := declaredParent(path)
	if root == nil {
		return nil
	}
	if err := MountDrives(*root); err != nil {
		return nil
	}
	registryMu.RLock()
	fs := registry[*root]
	registryMu.RUnlock()
	return fs
}

// Mounted reports whether any drive is registered (fast path guard).
func Mounted() bool {
	registryMu.RLock()
	defer registryMu.RUnlock()
	return len(registry) > 0
}

// namespacePath converts a MinIO path into the UUSE namespace path. Trailing
// slashes (MinIO composes directory paths with them) are stripped: the UUSE
// resolver treats a trailing empty name as an invalid path component.
func (u *UuseFs) namespacePath(path string) string {
	if path == u.root {
		return "/"
	}
	rel := path[len(u.root):]
	for len(rel) > 1 && rel[len(rel)-1] == '/' {
		rel = rel[:len(rel)-1]
	}
	if rel == "" {
		return "/"
	}
	return rel
}

// Registered checks whether a specific drive root is registered.
func Registered(root string) bool {
	registryMu.RLock()
	defer registryMu.RUnlock()
	_, ok := registry[strings.TrimSuffix(root, "/")]
	return ok
}

// RegisteredParent reports whether path lives on a registered UUSE drive.
func RegisteredParent(path string) bool { return lookup(path) != nil }

// LookupParent reports whether path is under any declared UUSE drive,
// registered or not.
func LookupParent(path string) bool { return declaredParent(path) != nil }

// declaredParent matches path against the MINIO_UUSE_DRIVES table (exact root
// or a path under it); unlike lookup it does not require prior registration.
func declaredParent(path string) *string {
	table := uuseEnvTable()
	clean := strings.TrimSuffix(path, "/")
	var best *string
	for root := range table {
		if clean == root || strings.HasPrefix(clean, root+"/") {
			if best == nil || len(root) > len(*best) {
				rootCopy := root
				best = &rootCopy
			}
		}
	}
	return best
}

// uuseEnvTable parses MINIO_UUSE_DRIVES on demand (the env may be set after
// package init in tests and helper tools, so no sync.Once caching).
func uuseEnvTable() map[string]string {
	table := make(map[string]string)
	value := os.Getenv("MINIO_UUSE_DRIVES")
	for _, part := range strings.Split(value, ",") {
		part = strings.TrimSpace(part)
		if part == "" {
			continue
		}
		eq := strings.LastIndex(part, "=")
		if eq <= 0 {
			continue
		}
		root := strings.TrimSuffix(strings.TrimSpace(part[:eq]), "/")
		conf := strings.TrimSpace(part[eq+1:])
		if root != "" && conf != "" {
			table[root] = conf
		}
	}
	return table
}

// MountDrives mounts every drive declared in MINIO_UUSE_DRIVES that is not
// registered yet. The variable format is "root=conf[,root=conf...]" with
// absolute roots (the drive paths passed to `minio server`).
func MountDrives(driveRoot string) error {
	conf, ok := uuseConfFor(driveRoot)
	if !ok {
		return errors.New("uuseos: no MINIO_UUSE_DRIVES entry for drive " + driveRoot)
	}
	if _, err := NewUuseFs(conf, driveRoot); err != nil {
		return err
	}
	return nil
}

// uuseConfFor returns the .conf path declared for a drive root.
func uuseConfFor(driveRoot string) (string, bool) {
	table := uuseEnvTable()
	clean := strings.TrimSuffix(driveRoot, "/")
	conf, ok := table[clean]
	return conf, ok
}

// DriveStats reports the engine volume capacity for a drive root.
func DriveStats(driveRoot string) (total, free, used uint64, err error) {
	fsys := lookup(driveRoot)
	if fsys == nil {
		return 0, 0, 0, errors.New("uuseos: drive not registered: " + driveRoot)
	}
	return fsys.eng.diskStats()
}

// ReadFileIfUuse reads a whole file through the shim for registered drives and
// falls back to os.ReadFile otherwise (mirrors xioutil.ReadFile semantics).
func ReadFileIfUuse(path string) ([]byte, error) {
	if fsys := lookup(path); fsys != nil {
		return fsys.cReadFile(fsys.namespacePath(path))
	}
	return os.ReadFile(path)
}

// Flush commits pending namespace metadata durably (design §20.3: File::sync
// is a namespace-level barrier covering created/renamed/removed entries).
func (u *UuseFs) Flush() error {
	f, err := u.cOpenFile("/.minio-namespace-flush", os.O_CREATE|os.O_WRONLY, 0o666)
	if err != nil {
		return err
	}
	syncErr := f.Sync()
	closeErr := f.Close()
	if syncErr != nil {
		return syncErr
	}
	return closeErr
}

// OpenDir opens a directory handle for the stock readDir seam: *os.File for
// native paths, UuseDir for registered drives.
func OpenDir(path string, flag int, perm os.FileMode) (DirEntry, error) {
	if fsys := lookup(path); fsys != nil {
		return fsys.cOpenDir(fsys.namespacePath(path))
	}
	return os.OpenFile(path, flag, perm)
}

// UuseDir is a shim-backed directory handle served as Readdirnames batches,
// mirroring the os.File directory semantics used by minio's readDir seam.
type UuseDir struct {
	dir    *C.uuse_minio_dir
	buferr error
}

// cOpenDir opens a shim directory iterator.
func (u *UuseFs) cOpenDir(path string) (*UuseDir, error) {
	cPath := C.CString(path)
	defer C.free(unsafe.Pointer(cPath))
	errbuf := make([]byte, int(C.UUSE_MINIO_ERRBUF))
	var dir *C.uuse_minio_dir
	rc := C.uuse_minio_dir_open(u.eng.handle, cPath, &dir,
		(*C.char)(unsafe.Pointer(&errbuf[0])), C.size_t(len(errbuf)))
	if rc != 0 {
		return nil, errnoError(int(rc), "uuse dir_open "+path, errbuf)
	}
	return &UuseDir{dir: dir}, nil
}

// Readdirnames returns up to n names; a missing iterator reports io.EOF.
func (d *UuseDir) Readdirnames(n int) ([]string, error) {
	if d.dir == nil {
		return nil, d.buferr
	}
	limit := n
	if n < 0 {
		limit = 4096 // bounded batch for n < 0 semantics
	}
	var out []string
	for len(out) < limit {
		var entries [64]C.uuse_minio_dirent
		want := limit - len(out)
		if want > 64 {
			want = 64
		}
		var count C.size_t
		var eof C.int
		rc := C.uuse_minio_dir_read(d.dir, &entries[0], C.size_t(want), &count, &eof)
		if rc != 0 {
			return out, errnoError(int(rc), "uuse dir_read", nil)
		}
		for i := 0; i < int(count); i++ {
			out = append(out, cStringN(entries[i].name[:]))
		}
		if eof != 0 {
			d.buferr = io.EOF
			break
		}
	}
	if len(out) == 0 && d.buferr != nil {
		return nil, d.buferr
	}
	return out, nil
}

// Close releases the shim directory handle.
func (d *UuseDir) Close() error {
	if d.dir == nil {
		return nil
	}
	C.uuse_minio_dir_close(d.dir)
	d.dir = nil
	return nil
}

// --- routing entry points ---------------------------------------------------

// OpenFile routes to UUSE for registered drives and to os otherwise.
func OpenFile(path string, flag int, perm os.FileMode) (*os.File, File, error) {
	if fs := lookup(path); fs != nil {
		f, err := fs.cOpenFile(fs.namespacePath(path), flag, perm)
		return nil, f, err
	}
	f, err := os.OpenFile(path, flag, perm)
	return f, f, err
}

// ReadFile reads a whole file; routed.
func ReadFile(path string) ([]byte, error) {
	if fs := lookup(path); fs != nil {
		return fs.cReadFile(fs.namespacePath(path))
	}
	return os.ReadFile(path)
}

// Stat routes to UUSE for registered drives.
func Stat(path string) (fs.FileInfo, error) {
	if fsys := lookup(path); fsys != nil {
		cStat, err := fsys.cPathStat(fsys.namespacePath(path))
		if err != nil {
			return nil, err
		}
		return statFromC(cStat, baseName(path)), nil
	}
	return os.Stat(path)
}

// Lstat is Stat for UUSE drives (no symlinks in the namespace).
func Lstat(path string) (fs.FileInfo, error) {
	if fs := lookup(path); fs != nil {
		return Stat(path)
	}
	return os.Lstat(path)
}

// MkdirAll creates every missing directory component; routed.
func MkdirAll(path string, perm os.FileMode) error {
	if fs := lookup(path); fs != nil {
		return fs.cMkdirAll(fs.namespacePath(path))
	}
	return os.MkdirAll(path, perm)
}

// Remove deletes one file or empty directory; routed.
func Remove(path string) error {
	if fs := lookup(path); fs != nil {
		return fs.cRemove(fs.namespacePath(path), false)
	}
	return os.Remove(path)
}

// RemoveAll deletes a tree recursively; routed.
func RemoveAll(path string) error {
	if fs := lookup(path); fs != nil {
		return fs.cRemove(fs.namespacePath(path), true)
	}
	return os.RemoveAll(path)
}

// Rename renames within one UUSE drive; mixed backends or cross-drive moves
// are rejected (fail-closed).
func Rename(from, to string) error {
	fromFs := lookup(from)
	toFs := lookup(to)
	if fromFs == nil && toFs == nil {
		return os.Rename(from, to)
	}
	if fromFs == nil || toFs == nil || fromFs != toFs {
		return &os.LinkError{Op: "rename", Old: from, New: to, Err: syscall.EXDEV}
	}
	return fromFs.cRename(fromFs.namespacePath(from), fromFs.namespacePath(to))
}

// ReadDirFn calls fn for each entry directly under path; routed. A missing
// directory reports no error with no entries (stock readDirFn semantics).
func ReadDirFn(path string, fn func(name string, mode os.FileMode) error) error {
	if fs := lookup(path); fs != nil {
		return fs.cReadDirFn(fs.namespacePath(path), fn)
	}
	return readDirFnNative(path, fn)
}

// ReadDirN lists at most count names (count < 0 means all); routed.
func ReadDirN(path string, count int) ([]string, error) {
	if fs := lookup(path); fs != nil {
		return fs.cReadDirN(fs.namespacePath(path), count)
	}
	return readDirNNative(path, count)
}

// baseName returns the last path element.
func baseName(path string) string {
	idx := strings.LastIndexByte(path, '/')
	return path[idx+1:]
}

// namedStat wraps a FileInfo with a proper Name() and directory bit.
type namedStat struct {
	fs.FileInfo
	name string
}

func (s namedStat) Name() string { return s.name }

// --- UuseFs operations ------------------------------------------------------

func (u *UuseFs) cReadFile(path string) ([]byte, error) {
	cPath := C.CString(path)
	defer C.free(unsafe.Pointer(cPath))
	// Size hint first; the read loop grows the buffer as needed.
	var size int64
	if cStat, err := u.cPathStat(path); err == nil {
		size = int64(cStat.logical_size)
	} else {
		return nil, err
	}
	if size > 1<<31 {
		return nil, errors.New("uuseos: file too large for ReadFile: " + path)
	}
	buf := make([]byte, size)
	if size == 0 {
		return buf, nil
	}
	var handle *C.uuse_minio_file
	errbuf := make([]byte, int(C.UUSE_MINIO_ERRBUF))
	rc := C.uuse_minio_open(u.eng.handle, cPath, 0, &handle,
		(*C.char)(unsafe.Pointer(&errbuf[0])), C.size_t(len(errbuf)))
	if rc != 0 {
		return nil, errnoError(int(rc), "uuse open "+path, errbuf)
	}
	defer C.uuse_minio_close(handle)
	read := C.uuse_minio_pread(handle, unsafe.Pointer(&buf[0]), C.size_t(size), 0)
	if read < 0 {
		return nil, errnoError(int(read), "uuse read "+path, nil)
	}
	return buf[:int(read)], nil
}

func (u *UuseFs) cMkdirAll(path string) error {
	cPath := C.CString(path)
	defer C.free(unsafe.Pointer(cPath))
	errbuf := make([]byte, int(C.UUSE_MINIO_ERRBUF))
	rc := C.uuse_minio_mkdir_all(u.eng.handle, cPath,
		(*C.char)(unsafe.Pointer(&errbuf[0])), C.size_t(len(errbuf)))
	if rc != 0 {
		return errnoError(int(rc), "uuse mkdir_all "+path, errbuf)
	}
	return nil
}

func (u *UuseFs) cRemove(path string, recursive bool) error {
	cPath := C.CString(path)
	defer C.free(unsafe.Pointer(cPath))
	errbuf := make([]byte, int(C.UUSE_MINIO_ERRBUF))
	var rc C.int
	if recursive {
		rc = C.uuse_minio_remove_all(u.eng.handle, cPath,
			(*C.char)(unsafe.Pointer(&errbuf[0])), C.size_t(len(errbuf)))
	} else {
		rc = C.uuse_minio_remove(u.eng.handle, cPath, nil,
			(*C.char)(unsafe.Pointer(&errbuf[0])), C.size_t(len(errbuf)))
	}
	if rc != 0 {
		return errnoError(int(rc), "uuse remove "+path, errbuf)
	}
	return nil
}

func (u *UuseFs) cRename(from, to string) error {
	cFrom := C.CString(from)
	cTo := C.CString(to)
	defer C.free(unsafe.Pointer(cFrom))
	defer C.free(unsafe.Pointer(cTo))
	errbuf := make([]byte, int(C.UUSE_MINIO_ERRBUF))
	rc := C.uuse_minio_rename(u.eng.handle, cFrom, cTo,
		(*C.char)(unsafe.Pointer(&errbuf[0])), C.size_t(len(errbuf)))
	if rc != 0 {
		return errnoError(int(rc), "uuse rename "+from+" -> "+to, errbuf)
	}
	return nil
}

// cReadDirFn streams the directory through a bounded shim iterator.
func (u *UuseFs) cReadDirFn(path string, fn func(name string, mode os.FileMode) error) error {
	cPath := C.CString(path)
	defer C.free(unsafe.Pointer(cPath))
	errbuf := make([]byte, int(C.UUSE_MINIO_ERRBUF))
	var dir *C.uuse_minio_dir
	rc := C.uuse_minio_dir_open(u.eng.handle, cPath, &dir,
		(*C.char)(unsafe.Pointer(&errbuf[0])), C.size_t(len(errbuf)))
	if rc != 0 {
		if syscall.Errno(-rc) == syscall.ENOENT {
			return nil // missing directory: stock readDirFn returns nil
		}
		return errnoError(int(rc), "uuse dir_open "+path, errbuf)
	}
	defer C.uuse_minio_dir_close(dir)
	return u.dirIterate(dir, fn, -1, nil)
}

// cReadDirN lists at most count names (count < 0 means all). Directory names
// carry a trailing slash, matching the stock readDirN semantics that MinIO's
// WalkDir relies on to distinguish objects from prefixes.
func (u *UuseFs) cReadDirN(path string, count int) ([]string, error) {
	cPath := C.CString(path)
	defer C.free(unsafe.Pointer(cPath))
	errbuf := make([]byte, int(C.UUSE_MINIO_ERRBUF))
	var dir *C.uuse_minio_dir
	rc := C.uuse_minio_dir_open(u.eng.handle, cPath, &dir,
		(*C.char)(unsafe.Pointer(&errbuf[0])), C.size_t(len(errbuf)))
	if rc != 0 {
		return nil, errnoError(int(rc), "uuse dir_open "+path, errbuf)
	}
	defer C.uuse_minio_dir_close(dir)

	var names []string
	err := u.dirIterateNames(dir, count, &names)
	return names, err
}

// dirIterateNames collects entry names (directories slash-suffixed) until the
// limit or end of the directory.
func (u *UuseFs) dirIterateNames(dir *C.uuse_minio_dir, limit int, names *[]string) error {
	var entries [64]C.uuse_minio_dirent
	for {
		var count C.size_t
		var eof C.int
		rc := C.uuse_minio_dir_read(dir, &entries[0], 64, &count, &eof)
		if rc != 0 {
			return errnoError(int(rc), "uuse dir_read", nil)
		}
		for i := 0; i < int(count); i++ {
			name := cStringN(entries[i].name[:])
			if entries[i].flags&1 != 0 {
				name += "/"
			}
			*names = append(*names, name)
			if limit > 0 && len(*names) >= limit {
				return nil
			}
		}
		if eof != 0 {
			return nil
		}
	}
}

// dirIterate walks the shim iterator once. When names is non-nil it collects
// names up to limit; when fn is non-nil it invokes the callback per entry.
// limit < 0 means unlimited.
func (u *UuseFs) dirIterate(dir *C.uuse_minio_dir, fn func(name string, mode os.FileMode) error, limit int, names *[]string) error {
	var entries [64]C.uuse_minio_dirent
	for {
		var count C.size_t
		var eof C.int
		rc := C.uuse_minio_dir_read(dir, &entries[0], 64, &count, &eof)
		if rc != 0 {
			return errnoError(int(rc), "uuse dir_read", nil)
		}
		if count == 0 && eof == 0 {
			continue // defensive; should not happen
		}
		for i := 0; i < int(count); i++ {
			entry := &entries[i]
			name := cStringN(entry.name[:])
			mode := os.FileMode(0)
			if entry.flags&1 != 0 {
				mode = os.ModeDir
			}
			if fn != nil {
				if err := fn(name, mode); err != nil {
					return err
				}
			}
			if names != nil {
				*names = append(*names, name)
				if limit >= 0 && len(*names) >= limit {
					return nil
				}
			}
		}
		if eof != 0 {
			return nil
		}
	}
}

// cStringN converts a fixed-size NUL-terminated C char array into a Go string.
func cStringN(buf []C.char) string {
	var raw []byte
	for _, c := range buf {
		if c == 0 {
			break
		}
		raw = append(raw, byte(c))
	}
	return string(raw)
}
