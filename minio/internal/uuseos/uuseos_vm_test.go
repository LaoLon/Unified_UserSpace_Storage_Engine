//go:build cgo

// Shard-9.2 bring-up test: the Go uuseos layer against a real UUSE volume.
// Run manually on the VM (device bound to uio, hugepages set):
//
//	go build ./internal/uuseos
//	go test -v -run TestUuseFs -count=1 ./internal/uuseos \
//	    -args /root/code/config/uuse_nvme.conf
//
// The test file exists so the cgo layer is exercised before the full MinIO
// seam integration.
package uuseos

import (
	"bytes"
	"os"
	"testing"
)

func TestUuseFs(t *testing.T) {
	conf := "/root/code/config/uuse_nvme.conf"
	if env := os.Getenv("UUSE_MINIO_TEST_CONF"); env != "" {
		conf = env
	}
	root := "/mnt/uuse-go-test/data"

	fsys, err := NewUuseFs(conf, root)
	if err != nil {
		t.Fatalf("NewUuseFs: %v", err)
	}
	defer fsys.Close()

	// Fresh state.
	_ = RemoveAll(root + "/test")

	if err := MkdirAll(root+"/test/a/b", 0o755); err != nil {
		t.Fatalf("MkdirAll: %v", err)
	}

	native, fh, err := OpenFile(root+"/test/a/b/part.1", os.O_CREATE|os.O_EXCL|os.O_WRONLY, 0o666)
	if err != nil {
		t.Fatalf("OpenFile: %v", err)
	}
	if native != nil {
		t.Fatalf("native file returned for registered drive")
	}
	payload := []byte("uuse-go-bridge")
	if _, err := fh.Write(payload); err != nil {
		t.Fatalf("write: %v", err)
	}
	if err := fh.Sync(); err != nil {
		t.Fatalf("sync: %v", err)
	}
	if err := fh.Close(); err != nil {
		t.Fatalf("close: %v", err)
	}

	got, err := ReadFile(root + "/test/a/b/part.1")
	if err != nil {
		t.Fatalf("ReadFile: %v", err)
	}
	if !bytes.Equal(got, payload) {
		t.Fatalf("content mismatch: %q", got)
	}

	info, err := Stat(root + "/test/a/b/part.1")
	if err != nil {
		t.Fatalf("Stat: %v", err)
	}
	if info.IsDir() || info.Size() != int64(len(payload)) {
		t.Fatalf("stat: dir=%v size=%d", info.IsDir(), info.Size())
	}

	dirInfo, err := Stat(root + "/test/a")
	if err != nil {
		t.Fatalf("Stat dir: %v", err)
	}
	if !dirInfo.IsDir() {
		t.Fatalf("expected directory")
	}

	var names []string
	if err := ReadDirFn(root+"/test/a", func(name string, mode os.FileMode) error {
		names = append(names, name)
		return nil
	}); err != nil {
		t.Fatalf("ReadDirFn: %v", err)
	}
	if len(names) != 1 || names[0] != "b" {
		t.Fatalf("ReadDirFn names: %v", names)
	}

	n2, err := ReadDirN(root+"/test/a/b", 10)
	if err != nil {
		t.Fatalf("ReadDirN: %v", err)
	}
	if len(n2) != 1 || n2[0] != "part.1" {
		t.Fatalf("ReadDirN names: %v", n2)
	}

	if err := Rename(root+"/test/a/b/part.1", root+"/test/a/b/part.2"); err != nil {
		t.Fatalf("Rename: %v", err)
	}
	if err := Remove(root + "/test/a/b/part.2"); err != nil {
		t.Fatalf("Remove: %v", err)
	}
	if err := RemoveAll(root + "/test"); err != nil {
		t.Fatalf("RemoveAll: %v", err)
	}
	if _, err := Stat(root + "/test"); err == nil {
		t.Fatalf("expected stat error after RemoveAll")
	}

	// Native routing still works for unregistered paths.
	if _, err := os.Stat("/etc/hostname"); err != nil {
		t.Fatalf("native stat: %v", err)
	}

	t.Log("UUSE_GO_BRIDGE_PASS")
}
