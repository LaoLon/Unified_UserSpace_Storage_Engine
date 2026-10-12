package main

// minio-wipe: reset a UUSE drive for a fresh MinIO instance: remove everything
// (including .minio.sys) and pre-create the metadata volume directories.
// Must be built inside the MinIO fork (imports internal/uuseos).
import (
	"fmt"
	"os"

	uuseos "github.com/minio/minio/internal/uuseos"
)

func main() {
	conf := os.Args[1]
	drive := os.Args[2]
	fsys, err := uuseos.NewUuseFs(conf, drive)
	if err != nil {
		fmt.Println("engine:", err)
		os.Exit(1)
	}
	defer fsys.Close()

	// 1) wipe every top-level entry (missing entries are fine).
	var entries []string
	if err := uuseos.ReadDirFn(drive, func(name string, mode os.FileMode) error {
		entries = append(entries, name)
		return nil
	}); err != nil {
		fmt.Println("readdir:", err)
		os.Exit(1)
	}
	for _, name := range entries {
		target := drive + "/" + name
		if err := uuseos.RemoveAll(target); err != nil {
			fmt.Println("remove:", target, err)
			os.Exit(1)
		}
	}
	fmt.Printf("removed %d entries\n", len(entries))

	// 2) pre-create the metadata volume tree used by the first cluster format.
	dirs := []string{
		".minio.sys",
		".minio.sys/tmp",
		".minio.sys/tmp/.trash",
		".minio.sys/multipart",
		".minio.sys/buckets",
		".minio.sys/config",
	}
	for _, dir := range dirs {
		if err := uuseos.MkdirAll(drive+"/"+dir, 0o777); err != nil {
			fmt.Println("mkdir:", dir, err)
			os.Exit(1)
		}
	}

	// 3) commit the namespace mutations durably before exiting, then remove
	// the flush marker file so the namespace stays pristine.
	if err := fsys.Flush(); err != nil {
		fmt.Println("flush:", err)
		os.Exit(1)
	}
	if err := uuseos.Remove(drive + "/.minio-namespace-flush"); err != nil {
		fmt.Println("cleanup:", err)
		os.Exit(1)
	}
	if err := fsys.Flush(); err != nil {
		fmt.Println("flush2:", err)
		os.Exit(1)
	}

	// 4) verify
	var verify []string
	if err := uuseos.ReadDirFn(drive, func(name string, mode os.FileMode) error {
		verify = append(verify, name)
		return nil
	}); err != nil {
		fmt.Println("verify:", err)
		os.Exit(1)
	}
	fmt.Printf("entries=%v\nMINIO-WIPE-RESET-PASS\n", verify)
}
