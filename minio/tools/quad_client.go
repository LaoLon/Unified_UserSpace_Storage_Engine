package main

// 4-node cluster cross test: touch all four endpoints.
// Endpoints come from QUAD_ENDPOINTS (comma separated); default is the lab
// cluster .250-.253. The test writes via one node, reads via the next, lists
// via two nodes, and rotates small objects across all endpoints.
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

var endpoints = []string{"192.168.1.250:9000", "192.168.1.251:9000", "192.168.1.252:9000", "192.168.1.253:9000"}

func client(ep string) *minio.Client {
	c, err := minio.New(ep, &minio.Options{
		Creds: credentials.NewStaticV4("minioadmin", "minioadmin", ""), Secure: false,
	})
	if err != nil {
		fmt.Println("client", ep, err)
		os.Exit(1)
	}
	return c
}

func main() {
	if env := os.Getenv("QUAD_ENDPOINTS"); env != "" {
		endpoints = strings.Split(env, ",")
	}
	ctx := context.Background()
	clients := []*minio.Client{}
	for _, ep := range endpoints {
		clients = append(clients, client(ep))
	}

	// bucket via the first endpoint
	if err := clients[0].MakeBucket(ctx, "quad", minio.MakeBucketOptions{}); err != nil {
		if ok, _ := clients[0].BucketExists(ctx, "quad"); !ok {
			fmt.Println("make-bucket:", err)
			os.Exit(1)
		}
	}
	payload := bytes.Repeat([]byte("quad-payload-"), 1<<16) // 768 KiB
	if _, err := clients[1%len(clients)].PutObject(ctx, "quad", "big.bin", bytes.NewReader(payload),
		int64(len(payload)), minio.PutObjectOptions{}); err != nil {
		fmt.Println("put via", endpoints[1%len(clients)], err)
		os.Exit(1)
	}
	fmt.Println("PUT via", endpoints[1%len(clients)], "ok")

	// GET via the last endpoint
	last := len(clients) - 1
	rd, err := clients[last].GetObject(ctx, "quad", "big.bin", minio.GetObjectOptions{})
	if err != nil {
		fmt.Println("get via", endpoints[last], err)
		os.Exit(1)
	}
	got, err := io.ReadAll(rd)
	if err != nil || !bytes.Equal(got, payload) {
		fmt.Printf("data mismatch via %s len=%d err=%v\n", endpoints[last], len(got), err)
		os.Exit(1)
	}
	fmt.Println("GET via", endpoints[last], "byte-identical")

	// small objects: put on each node, read from the next node
	for i := 0; i < 8; i++ {
		key := fmt.Sprintf("small-%02d.txt", i)
		v := []byte(fmt.Sprintf("nv-%d", i))
		if _, err := clients[i%len(clients)].PutObject(ctx, "quad", key, bytes.NewReader(v),
			int64(len(v)), minio.PutObjectOptions{}); err != nil {
			fmt.Println("put", key, err)
			os.Exit(1)
		}
		rd, err := clients[(i+1)%len(clients)].GetObject(ctx, "quad", key, minio.GetObjectOptions{})
		if err != nil {
			fmt.Println("get", key, err)
			os.Exit(1)
		}
		got, err := io.ReadAll(rd)
		if err != nil || string(got) != fmt.Sprintf("nv-%d", i) {
			fmt.Println("bad", key, err, string(got))
			os.Exit(1)
		}
	}
	fmt.Println("cross-node small objects ok (8 rota)")

	// LIST via two different endpoints
	listIdx := []int{0, 2 % len(clients)}
	for _, idx := range listIdx {
		n := 0
		for obj := range clients[idx].ListObjects(ctx, "quad", minio.ListObjectsOptions{Recursive: true}) {
			if obj.Err != nil {
				fmt.Println("list err:", obj.Err)
				os.Exit(1)
			}
			n++
		}
		fmt.Printf("LIST via %s count=%d\n", endpoints[idx], n)
	}
	fmt.Println("QUAD-CROSS-PASS")
}
