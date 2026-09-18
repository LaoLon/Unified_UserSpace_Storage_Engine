# Repair-93：性能构建有效性边界

日期：2026-09-18  
范围：HFS 2.0 `fio-hfs` external engine 与 raw SPDK 对照

## 结论

Repair-93 修正了一个会污染性能 gate 的构建问题：`fio-hfs` 不再默认被目标级
`-O0 -g` 强制编译。默认行为现在跟随顶层 `CMAKE_BUILD_TYPE`；正式性能构建使用
`Release`，调试构建必须显式设置 `-DHFS_FIO_DEBUG=ON`。

此前 `doc/performance/hfs2_shard92_formal_gate_20260918.md` 中的 HFS/raw 数字仍保留为
历史结果，但在优化构建重跑前不能作为核心性能差距结论。

## 构建口径

正式 gate 必须满足：

```text
cmake -S <hfs> -B <build> \
  -DCMAKE_BUILD_TYPE=Release \
  -DFIO_SOURCE_DIR=<fio> \
  -DHFS_FIO_DEBUG=OFF
cmake --build <build> --target hfs_core fio-hfs hfs_trace_dump
```

每次报告同时保存：

- `CMAKE_BUILD_TYPE`、`HFS_FIO_DEBUG` 和 `FIO_SOURCE_DIR`；
- `compile_commands.json` 中 `fio/fio_hfs.cpp` 的实际优化参数；
- HFS 与 raw 使用的 FIO/SPDK 版本、配置文件、CPU 绑定和设备绑定状态；
- 运行时 closure counters、错误数和有效 outstanding 证据。

调试构建只用于故障定位，不得参与 HFS/raw 性能比值或 p99 gate。

## 后续动作

Repair-95 完成统一 completion wait path 后，使用本口径重新构建并重跑正式五次 gate。
在此之前，不把历史 `70.239%` 或任何新单次结果标记为性能通过。
