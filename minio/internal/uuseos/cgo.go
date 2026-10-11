//go:build cgo

// Package uuseos is the MinIO disk-seam abstraction over the UUSE C ABI shim
// (design doc/design/uuse_minio_frontend.md, Shard-9.2). Paths under a
// registered UUSE drive root are routed to the shim engine; every other path
// keeps the stock os behavior. Routing is fail-closed: a registered drive
// never falls back to the host filesystem.
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
	"fmt"
	"syscall"
	"unsafe"
)

// engine wraps one UUSE engine handle (one drive).
type engine struct {
	handle *C.uuse_minio_engine
}

// errnoError converts a negative errno-style C return code into an error that
// satisfies errors.Is with the standard syscall.Errno sentinels.
func errnoError(rc int, operation string, errbuf []byte) error {
	if rc >= 0 {
		return nil
	}
	errno := syscall.Errno(-rc)
	message := ""
	if len(errbuf) > 0 && errbuf[0] != 0 {
		message = string(trimNul(errbuf))
	}
	if message != "" {
		return fmt.Errorf("%s: %w: %s", operation, errno, message)
	}
	return fmt.Errorf("%s: %w", operation, errno)
}

func trimNul(buf []byte) []byte {
	for i, b := range buf {
		if b == 0 {
			return buf[:i]
		}
	}
	return buf
}

// openEngine opens (or reuses) and mounts the engine of one .conf file.
func openEngine(confPath string) (*engine, error) {
	cConf := C.CString(confPath)
	defer C.free(unsafe.Pointer(cConf))
	errbuf := make([]byte, int(C.UUSE_MINIO_ERRBUF))
	var handle *C.uuse_minio_engine
	rc := C.uuse_minio_engine_open(cConf, &handle, (*C.char)(unsafe.Pointer(&errbuf[0])), C.size_t(len(errbuf)))
	if rc != 0 {
		return nil, errnoError(int(rc), "uuse engine open "+confPath, errbuf)
	}
	if handle == nil {
		return nil, errors.New("uuse engine open returned nil handle")
	}
	return &engine{handle: handle}, nil
}

// Close releases the caller reference of the engine handle.
func (e *engine) Close() {
	if e == nil || e.handle == nil {
		return
	}
	C.uuse_minio_engine_close(e.handle)
	e.handle = nil
}

// diskStats reports total/free/used bytes of the engine volume.
func (e *engine) diskStats() (total, free, used uint64, err error) {
	var cTotal, cFree, cUsed C.uint64_t
	errbuf := make([]byte, int(C.UUSE_MINIO_ERRBUF))
	rc := C.uuse_minio_disk_stats(e.handle, &cTotal, &cFree, &cUsed,
		(*C.char)(unsafe.Pointer(&errbuf[0])), C.size_t(len(errbuf)))
	if rc != 0 {
		return 0, 0, 0, errnoError(int(rc), "uuse disk stats", errbuf)
	}
	return uint64(cTotal), uint64(cFree), uint64(cUsed), nil
}
