//go:build cgo && !linux

package uuseos

import (
	"os"
)

// Fallback implementation for non-Linux platforms (macOS dev builds): uses the
// standard library instead of raw getdents.
func readDirFnNative(dirPath string, fn func(name string, mode os.FileMode) error) error {
	entries, err := os.ReadDir(dirPath)
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

// Fallback readDirN (names only; directories carry a trailing slash).
func readDirNNative(dirPath string, count int) ([]string, error) {
	entries, err := os.ReadDir(dirPath)
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
