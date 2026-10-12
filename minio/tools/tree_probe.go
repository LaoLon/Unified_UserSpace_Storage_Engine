package main

// tree-probe: recursive namespace listing with sizes for one subtree under the
// UUSE drive (used to locate MinIO shard files for the bitrot probe).
// Usage: tree-probe <uuse.conf> <drive> <rel-path>
import (
	"fmt"
	"os"

	uuseos "github.com/minio/minio/internal/uuseos"
)

func walk(drive, rel string, depth int) {
	var names []string
	_ = uuseos.ReadDirFn(drive+rel, func(name string, mode os.FileMode) error {
		names = append(names, name)
		return nil
	})
	for _, n := range names {
		full := rel + "/" + n
		info, err := uuseos.Stat(drive + full)
		kind := "f"
		size := int64(0)
		isDir := false
		if err == nil {
			isDir = info.IsDir()
			size = info.Size()
			if isDir {
				kind = "d"
			}
		}
		fmt.Printf("%*s%s %s size=%d\n", depth*2, "", kind, n, size)
		if isDir && depth < 2 {
			walk(drive, full, depth+1)
		}
	}
}

func main() {
	fsys, err := uuseos.NewUuseFs(os.Args[1], os.Args[2])
	if err != nil {
		fmt.Println("engine:", err)
		os.Exit(1)
	}
	defer fsys.Close()
	walk(os.Args[2], os.Args[3], 0)
}
