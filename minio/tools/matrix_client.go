package main

// Functional matrix for MinIO on UUSE (Shard-9.3 / 9.6 curated copy).
// Covers: object sizes, multipart (incl. resume via list-parts and abort),
// versioning, large-dir listing, range GET, metadata, copy, long paths.
// Endpoint comes from MATRIX_ENDPOINT (default 127.0.0.1:9000), so it can run
// against a single node or any cluster node.
import (
	"bytes"
	"context"
	"fmt"
	"io"
	"log"
	"os"

	"github.com/minio/minio-go/v7"
	"github.com/minio/minio-go/v7/pkg/credentials"
)

var (
	cli  *minio.Client
	core *minio.Core
	ctx  = context.Background()
	pass = 0
	fail = 0
)

func check(name string, err error) {
	if err != nil {
		fail++
		fmt.Printf("FAIL %s: %v\n", name, err)
		return
	}
	pass++
	fmt.Printf("PASS %s\n", name)
}

func makePayload(n int, seed byte) []byte {
	payload := make([]byte, n)
	for i := range payload {
		payload[i] = byte((i + int(seed)) % 251)
	}
	return payload
}

func putGet(bucket, key string, payload []byte, opts minio.PutObjectOptions) error {
	if _, err := cli.PutObject(ctx, bucket, key, bytes.NewReader(payload),
		int64(len(payload)), opts); err != nil {
		return fmt.Errorf("put: %w", err)
	}
	obj, err := cli.GetObject(ctx, bucket, key, minio.GetObjectOptions{})
	if err != nil {
		return fmt.Errorf("get: %w", err)
	}
	got, err := io.ReadAll(obj)
	if err != nil {
		return fmt.Errorf("read: %w", err)
	}
	if !bytes.Equal(got, payload) {
		return fmt.Errorf("content mismatch len=%d/%d", len(got), len(payload))
	}
	return nil
}

func main() {
	var err error
	endpoint := os.Getenv("MATRIX_ENDPOINT")
	if endpoint == "" {
		endpoint = "127.0.0.1:9000"
	}
	cli, err = minio.New(endpoint, &minio.Options{
		Creds: credentials.NewStaticV4("minioadmin", "minioadmin", ""), Secure: false,
	})
	if err != nil {
		log.Fatal(err)
	}
	core, err = minio.NewCore(endpoint, &minio.Options{
		Creds: credentials.NewStaticV4("minioadmin", "minioadmin", ""), Secure: false,
	})
	if err != nil {
		log.Fatal(err)
	}
	bucket := "matrix"
	if err := cli.MakeBucket(ctx, bucket, minio.MakeBucketOptions{}); err != nil {
		if ok, _ := cli.BucketExists(ctx, bucket); !ok {
			log.Fatalf("bucket: %v", err)
		}
	}

	// 1) object sizes
	sizes := []int{0, 1, 4096, 65536, 1 << 20, 5 << 20, (5 << 20) + 1, 12 << 20}
	for _, size := range sizes {
		key := fmt.Sprintf("sizes/obj-%d", size)
		payload := makePayload(size, byte(size))
		check(fmt.Sprintf("size-%d", size), putGet(bucket, key, payload, minio.PutObjectOptions{}))
	}

	// 2) multipart (5 MiB part size, 2.5 parts worth)
	big := makePayload(12<<20, 7)
	partSize := uint64(5 << 20)
	check("multipart-12MiB", putGet(bucket, "mp/big.bin", big, minio.PutObjectOptions{PartSize: partSize}))

	// 3) range GET (object was written with seed byte(size) = byte(1048576&0xff) = 0)
	ref := makePayload(1<<20, 0)
	obj, err := cli.GetObject(ctx, bucket, "sizes/obj-1048576", minio.GetObjectOptions{})
	if err == nil {
		var got []byte
		got, err = io.ReadAll(obj)
		if err == nil && bytes.Equal(got, ref) {
			pass++
			fmt.Println("PASS range-full-read")
		} else {
			check("range-full-read", fmt.Errorf("err=%v len=%d", err, len(got)))
		}
		// ranged read
		obj2, rerr := cli.GetObject(ctx, bucket, "sizes/obj-1048576", minio.GetObjectOptions{})
		if rerr == nil {
			buf := make([]byte, 4096)
			n, rerr2 := obj2.ReadAt(buf, 4096)
			if rerr2 == nil && n == 4096 && bytes.Equal(buf, ref[4096:8192]) {
				pass++
				fmt.Println("PASS range-readat")
			} else {
				check("range-readat", fmt.Errorf("n=%d err=%v", n, rerr2))
			}
		}
	} else {
		check("range-full-read", err)
	}

	// 4) versioning
	check("versioning-enable", cli.EnableVersioning(ctx, bucket))
	for i := 0; i < 3; i++ {
		v := []byte(fmt.Sprintf("version-%d", i))
		if _, err := cli.PutObject(ctx, bucket, "ver/obj.txt", bytes.NewReader(v),
			int64(len(v)), minio.PutObjectOptions{}); err != nil {
			check(fmt.Sprintf("version-put-%d", i), err)
			continue
		}
		pass++
		fmt.Printf("PASS version-put-%d\n", i)
	}
	versions := cli.ListObjects(ctx, bucket, minio.ListObjectsOptions{
		Prefix: "ver/", Recursive: true, WithVersions: true,
	})
	{
		count := 0
		for range versions {
			count++
		}
		if count >= 3 {
			pass++
			fmt.Printf("PASS version-list count=%d\n", count)
		} else {
			check("version-list", fmt.Errorf("count=%d", count))
		}
	}

	// 5) large directory listing (1000 objects)
	if err := cli.MakeBucket(ctx, "bigdir", minio.MakeBucketOptions{}); err != nil {
		if ok, _ := cli.BucketExists(ctx, "bigdir"); !ok {
			check("bigdir-bucket", err)
		}
	}
	small := []byte("x")
	putErrs := 0
	for i := 0; i < 1000; i++ {
		key := fmt.Sprintf("dir/%04d.txt", i)
		if _, err := cli.PutObject(ctx, "bigdir", key, bytes.NewReader(small), 1,
			minio.PutObjectOptions{}); err != nil {
			putErrs++
		}
	}
	if putErrs == 0 {
		pass++
		fmt.Println("PASS bigdir-put 1000")
	} else {
		check("bigdir-put", fmt.Errorf("errors=%d", putErrs))
	}
	listed := 0
	for obj := range cli.ListObjects(ctx, "bigdir", minio.ListObjectsOptions{Recursive: true}) {
		if obj.Err != nil {
			check("bigdir-list", obj.Err)
			break
		}
		listed++
	}
	if listed == 1000 {
		pass++
		fmt.Println("PASS bigdir-list 1000")
	} else {
		check("bigdir-list", fmt.Errorf("listed=%d", listed))
	}

	// 6) copy
	src := minio.CopySrcOptions{Bucket: bucket, Object: "sizes/obj-4096"}
	dst := minio.CopyDestOptions{Bucket: bucket, Object: "copy/obj-4096"}
	if _, err := cli.CopyObject(ctx, dst, src); err == nil {
		pass++
		fmt.Println("PASS copy-object")
	} else {
		check("copy-object", err)
	}

	// 7) remove + delete marker interplay
	check("remove-object", cli.RemoveObject(ctx, bucket, "copy/obj-4096", minio.RemoveObjectOptions{}))

	// 8) stat/metadata
	info, err := cli.StatObject(ctx, bucket, "sizes/obj-1048576", minio.StatObjectOptions{})
	if err == nil && info.Size == 1<<20 {
		pass++
		fmt.Println("PASS stat-size")
	} else {
		check("stat-size", fmt.Errorf("%v size=%d", err, info.Size))
	}

	// 9) multipart resume: upload part 1, list parts, upload part 2, complete.
	func() {
		key := "mp-resume/obj"
		uploadID, err := core.NewMultipartUpload(ctx, bucket, key, minio.PutObjectOptions{})
		if err != nil {
			check("mp-resume-create", err)
			return
		}
		part1 := makePayload(5<<20, 1)
		p1, err := core.PutObjectPart(ctx, bucket, key, uploadID, 1,
			bytes.NewReader(part1), int64(len(part1)), minio.PutObjectPartOptions{})
		if err != nil {
			check("mp-resume-part1", err)
			return
		}
		parts, err := core.ListObjectParts(ctx, bucket, key, uploadID, 0, 1000)
		if err != nil || len(parts.ObjectParts) != 1 ||
			parts.ObjectParts[0].PartNumber != 1 || parts.ObjectParts[0].Size != int64(len(part1)) {
			check("mp-resume-list-parts", fmt.Errorf("%v parts=%d", err, len(parts.ObjectParts)))
			return
		}
		part2 := makePayload(3<<20, 2)
		p2, err := core.PutObjectPart(ctx, bucket, key, uploadID, 2,
			bytes.NewReader(part2), int64(len(part2)), minio.PutObjectPartOptions{})
		if err != nil {
			check("mp-resume-part2", err)
			return
		}
		_, err = core.CompleteMultipartUpload(ctx, bucket, key, uploadID,
			[]minio.CompletePart{{PartNumber: p1.PartNumber, ETag: p1.ETag},
				{PartNumber: p2.PartNumber, ETag: p2.ETag}}, minio.PutObjectOptions{})
		if err != nil {
			check("mp-resume-complete", err)
			return
		}
		got, err := cli.GetObject(ctx, bucket, key, minio.GetObjectOptions{})
		if err != nil {
			check("mp-resume-get", err)
			return
		}
		body, err := io.ReadAll(got)
		want := append(append([]byte{}, part1...), part2...)
		if err != nil || !bytes.Equal(body, want) {
			check("mp-resume-content", fmt.Errorf("err=%v len=%d/%d", err, len(body), len(want)))
			return
		}
		pass++
		fmt.Printf("PASS mp-resume list-parts+complete size=%d\n", len(body))
	}()

	// 10) multipart abort leaves no visible object.
	func() {
		key := "mp-abort/obj"
		uploadID, err := core.NewMultipartUpload(ctx, bucket, key, minio.PutObjectOptions{})
		if err != nil {
			check("mp-abort-create", err)
			return
		}
		part := makePayload(1<<20, 3)
		if _, err := core.PutObjectPart(ctx, bucket, key, uploadID, 1,
			bytes.NewReader(part), int64(len(part)), minio.PutObjectPartOptions{}); err != nil {
			check("mp-abort-part", err)
			return
		}
		if err := core.AbortMultipartUpload(ctx, bucket, key, uploadID); err != nil {
			check("mp-abort", err)
			return
		}
		if _, err := cli.StatObject(ctx, bucket, key, minio.StatObjectOptions{}); err == nil {
			check("mp-abort-invisible", fmt.Errorf("object visible after abort"))
			return
		}
		pass++
		fmt.Println("PASS mp-abort invisible")
	}()

	// 11) long path: deep directories plus a long key.
	func() {
		deep := "longpath"
		for i := 0; i < 12; i++ {
			deep += fmt.Sprintf("/d%02d", i)
		}
		key := deep + "/" + string(bytes.Repeat([]byte("k"), 200)) + ".bin"
		payload := makePayload(64<<10, 4)
		if err := putGet(bucket, key, payload, minio.PutObjectOptions{}); err != nil {
			check("long-path", err)
			return
		}
		pass++
		fmt.Printf("PASS long-path keylen=%d\n", len(key))
	}()

	fmt.Printf("MATRIX-DONE pass=%d fail=%d\n", pass, fail)
	if fail > 0 {
		os.Exit(1)
	}
}
