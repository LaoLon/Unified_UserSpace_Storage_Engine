package main

// Crash-matrix tool for MinIO on UUSE (Shard-9.x curated copy):
//
//	write        uploads numbered 1 MiB objects, prints ACK <key> per success
//	verify       re-reads every key from the given ack file, checks content
//	sweep        reads every object currently listed under each bucket
//	rm           removes one key
//
// Endpoint comes from MATRIX_ENDPOINT (default 127.0.0.1:9000).
import (
	"bufio"
	"bytes"
	"context"
	"fmt"
	"io"
	"os"
	"strings"

	"github.com/minio/minio-go/v7"
	"github.com/minio/minio-go/v7/pkg/credentials"
)

func client() (*minio.Client, error) {
	endpoint := os.Getenv("MATRIX_ENDPOINT")
	if endpoint == "" {
		endpoint = "127.0.0.1:9000"
	}
	return minio.New(endpoint, &minio.Options{
		Creds: credentials.NewStaticV4("minioadmin", "minioadmin", ""), Secure: false,
	})
}

func main() {
	cli, err := client()
	if err != nil {
		fmt.Println("client:", err)
		os.Exit(1)
	}
	ctx := context.Background()
	payload := bytes.Repeat([]byte("crash-payload-"), 1<<16) // 1 MiB

	switch os.Args[1] {
	case "write":
		_ = cli.MakeBucket(ctx, "crash", minio.MakeBucketOptions{})
		for i := 0; i < 60; i++ {
			key := fmt.Sprintf("obj-%03d.bin", i)
			if _, err := cli.PutObject(ctx, "crash", key, bytes.NewReader(payload),
				int64(len(payload)), minio.PutObjectOptions{}); err != nil {
				fmt.Println("write-stopped", key, err)
				return
			}
			fmt.Println("ACK", key)
		}
		fmt.Println("write-complete")
	case "verify":
		fh, err := os.Open(os.Args[2])
		if err != nil {
			fmt.Println("ack-file:", err)
			os.Exit(1)
		}
		defer fh.Close()
		good, bad := 0, 0
		sc := bufio.NewScanner(fh)
		for sc.Scan() {
			line := strings.TrimSpace(sc.Text())
			if !strings.HasPrefix(line, "ACK ") {
				continue
			}
			key := strings.TrimPrefix(line, "ACK ")
			rd, err := cli.GetObject(ctx, "crash", key, minio.GetObjectOptions{})
			if err != nil {
				fmt.Println("GET-ERR", key, err)
				bad++
				continue
			}
			got, err := io.ReadAll(rd)
			if err != nil || !bytes.Equal(got, payload) {
				fmt.Println("DATA-BAD", key, err)
				bad++
				continue
			}
			good++
		}
		fmt.Printf("VERIFY-ACKED good=%d bad=%d\n", good, bad)
		if bad > 0 {
			os.Exit(1)
		}
	case "sweep":
		good, bad := 0, 0
		for obj := range cli.ListObjects(ctx, "crash", minio.ListObjectsOptions{Recursive: true}) {
			if obj.Err != nil {
				fmt.Println("LIST-ERR", obj.Err)
				bad++
				continue
			}
			rd, err := cli.GetObject(ctx, "crash", obj.Key, minio.GetObjectOptions{})
			if err != nil {
				fmt.Println("GET-ERR", obj.Key, err)
				bad++
				continue
			}
			got, err := io.ReadAll(rd)
			if err != nil || !bytes.Equal(got, payload) {
				fmt.Println("DATA-BAD", obj.Key, err)
				bad++
				continue
			}
			good++
		}
		fmt.Printf("SWEEP good=%d bad=%d\n", good, bad)
		if bad > 0 {
			os.Exit(1)
		}
	case "rm":
		if len(os.Args) < 3 {
			fmt.Println("usage: rm <key>")
			return
		}
		if err := cli.RemoveObject(ctx, "crash", os.Args[2], minio.RemoveObjectOptions{}); err != nil {
			fmt.Println("rm:", err)
			return
		}
		fmt.Println("RM-OK", os.Args[2])
	default:
		fmt.Println("usage: write|verify <ack>|sweep|rm <key>")
		os.Exit(2)
	}
}
