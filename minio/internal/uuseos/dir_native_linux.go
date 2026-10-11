//go:build cgo && linux

package uuseos

import (
	"bytes"
	"fmt"
	"os"
	"syscall"
	"unsafe"
)

// Stock readDirFn behavior for Linux: raw getdents parsing, one callback per
// entry (used for unregistered paths only).
func readDirFnNative(dirPath string, fn func(name string, mode os.FileMode) error) error {
	fd, err := syscall.Open(dirPath, syscall.O_RDONLY|syscall.O_DIRECTORY, 0)
	if err != nil {
		if err == syscall.ENOENT {
			return nil
		}
		return &os.PathError{Op: "open", Path: dirPath, Err: err}
	}
	defer syscall.Close(fd)

	buf := make([]byte, 8192)
	for {
		nbuf, err := syscall.ReadDirent(fd, buf)
		if err != nil {
			if err == syscall.ENOTDIR {
				return nil
			}
			return &os.PathError{Op: "readdirent", Path: dirPath, Err: err}
		}
		if nbuf <= 0 {
			return nil
		}
		for boff := 0; boff < nbuf; {
			consumed, name, typ, err := parseDirEntNative(buf[boff:nbuf])
			if err != nil {
				return err
			}
			boff += consumed
			if len(name) == 0 || bytes.Equal(name, []byte{'.'}) || bytes.Equal(name, []byte{'.', '.'}) {
				continue
			}
			if typ == unexpectedFileModeNative || typ&os.ModeSymlink == os.ModeSymlink {
				fi, err := os.Lstat(dirPath + "/" + string(name))
				if err != nil {
					continue
				}
				if typ&os.ModeSymlink == os.ModeSymlink && fi.IsDir() {
					continue
				}
				typ = fi.Mode() & os.ModeType
			}
			if err := fn(string(name), typ); err != nil {
				return err
			}
		}
	}
}

// Stock readDirN behavior for Linux (names only, count < 0 means all;
// directories carry a trailing slash like the stock implementation).
func readDirNNative(dirPath string, count int) ([]string, error) {
	var entries []string
	err := readDirFnNative(dirPath, func(name string, mode os.FileMode) error {
		nameStr := name
		if mode.IsDir() {
			nameStr = name + "/"
		}
		entries = append(entries, nameStr)
		if count > 0 && len(entries) >= count {
			return errStopIteration
		}
		return nil
	})
	if err == errStopIteration {
		err = nil
	}
	return entries, err
}

var errStopIteration = fmt.Errorf("stop iteration")

const unexpectedFileModeNative os.FileMode = os.ModeNamedPipe | os.ModeSocket | os.ModeDevice

// parseDirEntNative mirrors the stock getdents record parser.
func parseDirEntNative(buf []byte) (consumed int, name []byte, typ os.FileMode, err error) {
	dirent := (*syscall.Dirent)(unsafe.Pointer(&buf[0]))
	if len(buf) < int(dirent.Reclen) {
		return 0, nil, 0, fmt.Errorf("buf size %d < record length %d", len(buf), dirent.Reclen)
	}
	consumed = int(dirent.Reclen)
	if dirent.Ino == 0 {
		return consumed, nil, typ, nil
	}
	switch dirent.Type {
	case syscall.DT_REG:
		typ = 0
	case syscall.DT_DIR:
		typ = os.ModeDir
	case syscall.DT_LNK:
		typ = os.ModeSymlink
	default:
		typ = unexpectedFileModeNative
	}
	name = direntName(dirent)
	return consumed, name, typ, nil
}

// direntName extracts the NUL-terminated name bytes from a dirent.
func direntName(dirent *syscall.Dirent) []byte {
	nameBuf := (*[256]byte)(unsafe.Pointer(&dirent.Name[0]))
	var length int
	for length < len(nameBuf) {
		if nameBuf[length] == 0 {
			break
		}
		length++
	}
	out := make([]byte, length)
	copy(out, nameBuf[:length])
	return out
}
