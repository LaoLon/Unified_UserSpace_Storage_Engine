package main

// poke: overwrite bytes at an offset inside a UUSE namespace file. Used by the
// Shard-9.6 bitrot-injection acceptance test to flip data underneath MinIO.
// Usage: poke <conf> <drive> <rel-path> <offset> <length> [byte]
import (
	"bytes"
	"fmt"
	"io"
	"os"
	"strconv"

	uuseos "github.com/minio/minio/internal/uuseos"
)

func main() {
	if len(os.Args) < 6 {
		fmt.Println("usage: poke <conf> <drive> <rel> <offset> <length> [byte]")
		os.Exit(2)
	}
	conf, drive, rel := os.Args[1], os.Args[2], os.Args[3]
	offset, err := strconv.ParseInt(os.Args[4], 10, 64)
	if err != nil {
		fmt.Println("offset:", err)
		os.Exit(2)
	}
	length, err := strconv.Atoi(os.Args[5])
	if err != nil {
		fmt.Println("length:", err)
		os.Exit(2)
	}
	value := byte(0xA5)
	if len(os.Args) > 6 {
		parsed, err := strconv.Atoi(os.Args[6])
		if err != nil {
			fmt.Println("byte:", err)
			os.Exit(2)
		}
		value = byte(parsed)
	}

	fsys, err := uuseos.NewUuseFs(conf, drive)
	if err != nil {
		fmt.Println("engine:", err)
		os.Exit(1)
	}
	defer fsys.Close()

	path := drive + rel
	info, err := uuseos.Stat(path)
	if err != nil {
		fmt.Println("stat:", err)
		os.Exit(1)
	}
	if offset+int64(length) > info.Size() {
		fmt.Printf("poke range beyond size: size=%d offset=%d len=%d\n", info.Size(), offset, length)
		os.Exit(1)
	}
	_, f, err := uuseos.OpenFile(path, os.O_RDWR, 0o666)
	if err != nil {
		fmt.Println("open:", err)
		os.Exit(1)
	}
	if _, err := f.Seek(offset, io.SeekStart); err != nil {
		fmt.Println("seek:", err)
		os.Exit(1)
	}
	written, err := f.Write(bytes.Repeat([]byte{value}, length))
	if err != nil {
		fmt.Println("write:", err)
		os.Exit(1)
	}
	if err := f.Sync(); err != nil {
		fmt.Println("sync:", err)
		os.Exit(1)
	}
	f.Close()
	fmt.Printf("POKE-PASS path=%s size=%d offset=%d len=%d written=%d value=0x%02x\n",
		rel, info.Size(), offset, length, written, value)
}
