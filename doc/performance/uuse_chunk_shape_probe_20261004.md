# UUSE chunk 形状数据面探针（3FS 本地引擎适配 M1）

> 2026-10-04，.250（3FS storage-2，克隆机重建后）。目的：回答"UUSE 能不能支撑
> 3FS chunk 数据面"（64 KiB / 512 KiB / 4 MiB 随机读写），为 M1 chunk-store PoC
> 提供数据面基线。本文是 `doc/design/uuse_3fs_local_engine.md` M1 的配套证据。

## 口径

- 设备：VMware NVME_0002，PCI BDF `0000:13:00.0`，UIO 绑定（SPDK setup.sh）
- UUSE 配置：`config/uuse_nvme.conf`
  （sha256 `69fbb1803a5e4fc7936f82f15afdeb881fc617f96faa33abb31668b945285913`）
- 引擎：`build-verify/fio-uuse.so`
  （sha256 `0b4da06b6b61ef5e94c97fbe36f1767515b1c7cdb4e15ed28d281449fb1e72e7`），
  `libuuse_core.so.2.0.0`
  （sha256 `92e090c52ac4c6499187176dc1e05d5165d1b0f3b0c17cdd579c2c3b7262ed76`）
- SPDK：`f0b63ed866f0e4d3c56e73bd4a98d0042956147a`（克隆机内 Git 树）
- fio 3.42，external `fio-uuse` 引擎：`--thread=1 --uuse_async=1 --uuse_client_depth=32`
- 负载：每档先 prep 写填充 2 GiB 文件 `/chunk`，再计时 randread / randwrite /
  randrw(50/50)，bs = 64k / 512k / 4m，QD32（1 job），runtime 10 s，direct=1
- 证据目录（VM）：`/home/uuse/uuse-chunk-shape-20261004`（25 个文件：JSON + stdout + format.log）
- 引擎 closure 样本（randrw-64k）：`completed_error=0 token_mismatch=0 peak=32`，
  无软件侧错误或句柄泄漏

## 结果

| bs | 负载 | IOPS | BW MiB/s | mean us | p99 us | p99.9 us |
|---|---|---:|---:|---:|---:|---:|
| 64k | randread | 48 170 | 3 011 | 612 | 1 122 | 1 663 |
| 64k | randwrite | 37 334 | 2 333 | 789 | 1 352 | 1 958 |
| 64k | randrw | 21 001 / 20 991 | 1 313 / 1 312 | 700 / 705 | 1 253 | 1 745 |
| 512k | randread | 8 028 | 4 014 | 3 687 | 4 817 | 6 259 |
| 512k | randwrite | 5 911 | 2 956 | 4 975 | 6 193 | 8 290 |
| 512k | randrw | 3 472 / 3 474 | 1 736 / 1 737 | 4 249 / 4 261 | 5 276 | 6 259 |
| 4m | randread | 949 | 3 796 | 31 187 | 35 389 | 71 828 |
| 4m | randwrite | 669 | 2 678 | 43 985 | 51 118 | 60 031 |
| 4m | randrw | 405 / 415 | 1 621 / 1 659 | 35 698 / 36 271 | 41 681 | 46 400 |

（randrw 两列依次为 read / write。）

## 观察

1. 512k/4m 档带宽达到 ~3.0–4.0 GiB/s 读、~2.7–3.0 GiB/s 写，已接近该虚拟 NVMe 的
   设备上限（与历史 raw SPDK / 正式矩阵同量级），UUSE 软件路径未成为瓶颈。
2. 64k 随机读 48.2K IOPS、随机写 37.3K IOPS、混合 ~42K IOPS，覆盖 3FS 常见
   chunk 尺寸的并发需求。
3. 延迟随 QD 线性放大（64k mean 0.6–0.8 ms；4m mean ~31–44 ms），与 QD32 ×
   带宽受限的预期一致，p99 未出现异常长尾。
4. 全部 9 个计时 cell 退出码为 0，无 FIO job error、无 closure 异常。

## 结论与下一步

- M1 第一问（数据面可行性）结论：**通过**——UUSE 在 chunk 形状下可持续服务，
  无软件错误，性能受设备带宽限制。
- 下一步：M1 chunk-store PoC（固定 slot 大文件布局 + chunk 元数据 + CRC32C +
  sync/恢复语义），随后进入 M2 的 3FS `UuseChunkStore` 后端。
