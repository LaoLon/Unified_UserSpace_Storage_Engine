package main

// Big-write round trip through uuseos: sizes 64K, 1M, 5M (bypasses the S3
// layer; used as a UUSE-path smoke from inside the MinIO fork).
// Usage: big-probe <uuse.conf> <drive>
import (
	"bytes"
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

	for _, size := range []int{4096, 65536, 1 << 20, 5 << 20} {
		path := fmt.Sprintf("%s/bigprobe/obj-%d", drive, size)
		payload := bytes.Repeat([]byte{0xA5}, size)

		_, f, err := uuseos.OpenFile(path, os.O_CREATE|os.O_TRUNC|os.O_WRONLY, 0o666)
		if err != nil {
			// parent may not exist
			if uuseos.MkdirAll(drive+"/bigprobe", 0o777) != nil {
				fmt.Println("mkdir:", err)
				os.Exit(1)
			}
			_, f, err = uuseos.OpenFile(path, os.O_CREATE|os.O_TRUNC|os.O_WRONLY, 0o666)
			if err != nil {
				fmt.Printf("size=%d open: %v\n", size, err)
				os.Exit(1)
			}
		}
		n, werr := f.Write(payload)
		if werr != nil {
			fmt.Printf("size=%d write n=%d err=%v\n", size, n, werr)
			os.Exit(1)
		}
		if err := f.Sync(); err != nil {
			fmt.Printf("size=%d sync: %v\n", size, err)
			os.Exit(1)
		}
		f.Close()

		_, rf, err := uuseos.OpenFile(path, os.O_RDONLY, 0)
		if err != nil {
			fmt.Printf("size=%d reopen: %v\n", size, err)
			os.Exit(1)
		}
		buf := make([]byte, size)
		total := 0
		for total < size {
			n, rerr := rf.Read(buf[total:])
			if n > 0 {
				total += n
			}
			if rerr != nil {
				break
			}
		}
		rf.Close()
		ok := total == size && bytes.Equal(buf[:total], payload[:total])
		fmt.Printf("size=%d wrote=%d read=%d ok=%v\n", size, n, total, ok)
		if !ok {
			os.Exit(1)
		}
	}
	fmt.Println("BIG-PROBE-PASS")
}
