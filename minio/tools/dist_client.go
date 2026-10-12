package main

// Cross-node distributed test: PUT via one endpoint, GET/LIST via the other.
// Endpoints come from DIST_ENDPOINTS (comma separated); default is .251/.252.
import (
	"bytes"
	"context"
	"fmt"
	"io"
	"os"
	"strings"

	"github.com/minio/minio-go/v7"
	"github.com/minio/minio-go/v7/pkg/credentials"
)

func client(ep string) (*minio.Client, error) {
	return minio.New(ep, &minio.Options{
		Creds: credentials.NewStaticV4("minioadmin", "minioadmin", ""), Secure: false,
	})
}

func main() {
	eps := []string{"192.168.1.251:9000", "192.168.1.252:9000"}
	if env := os.Getenv("DIST_ENDPOINTS"); env != "" {
		eps = strings.Split(env, ",")
	}
	ctx := context.Background()
	c1, err := client(eps[0])
	if err != nil {
		fmt.Println("client", eps[0], err)
		os.Exit(1)
	}
	c2, err2 := client(eps[1])
	if err2 != nil {
		fmt.Println("client", eps[1], err2)
		os.Exit(1)
	}

	payload := bytes.Repeat([]byte("dist-payload-"), 1<<16) // ~0.8 MiB
	if err := c1.MakeBucket(ctx, "dist", minio.MakeBucketOptions{}); err != nil {
		if ok, _ := c1.BucketExists(ctx, "dist"); !ok {
			fmt.Println("make-bucket:", err)
			os.Exit(1)
		}
	}
	if _, err := c1.PutObject(ctx, "dist", "big.bin", bytes.NewReader(payload),
		int64(len(payload)), minio.PutObjectOptions{}); err != nil {
		fmt.Println("put via", eps[0], err)
		os.Exit(1)
	}
	fmt.Println("PUT via", eps[0], "ok")

	rd, err := c2.GetObject(ctx, "dist", "big.bin", minio.GetObjectOptions{})
	if err != nil {
		fmt.Println("get via", eps[1], err)
		os.Exit(1)
	}
	got, err := io.ReadAll(rd)
	if err != nil || !bytes.Equal(got, payload) {
		fmt.Printf("data mismatch via %s: len=%d err=%v\n", eps[1], len(got), err)
		os.Exit(1)
	}
	fmt.Println("GET via", eps[1], "byte-identical")

	n := 0
	for obj := range c2.ListObjects(ctx, "dist", minio.ListObjectsOptions{Recursive: true}) {
		if obj.Err != nil {
			fmt.Println("list via", eps[1], obj.Err)
			os.Exit(1)
		}
		n++
	}
	fmt.Println("LIST via", eps[1], "count:", n)

	// small objects across both endpoints
	for i := 0; i < 10; i++ {
		key := fmt.Sprintf("small-%02d.txt", i)
		v := []byte(fmt.Sprintf("v-%d", i))
		cli := c1
		if i%2 == 1 {
			cli = c2
		}
		if _, err := cli.PutObject(ctx, "dist", key, bytes.NewReader(v), int64(len(v)),
			minio.PutObjectOptions{}); err != nil {
			fmt.Println("put", key, err)
			os.Exit(1)
		}
	}
	// read all through the other node
	other := c2
	for i := 0; i < 10; i++ {
		key := fmt.Sprintf("small-%02d.txt", i)
		if i%2 == 0 {
			other = c2
		} else {
			other = c1
		}
		rd, err := other.GetObject(ctx, "dist", key, minio.GetObjectOptions{})
		if err != nil {
			fmt.Println("get", key, err)
			os.Exit(1)
		}
		got, err := io.ReadAll(rd)
		if err != nil || string(got) != fmt.Sprintf("v-%d", i) {
			fmt.Println("bad", key, err, string(got))
			os.Exit(1)
		}
	}
	fmt.Println("cross-node small objects ok")
	fmt.Println("DIST-CROSS-PASS")
}
