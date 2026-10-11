# SF=1 CSD 计算与 AER 缓存回填重叠验证

日期：2026-10-04。分支：`dev`。数据库：TPC-H SF=1，PostgreSQL 12.22；PGDATA 位于
CEMU 模拟 NVMe。CEMU 使用 TCG + VM-Freeze，没有启用 KVM。

## 实现与时间线

CSD functional kernel 在 VM-Freeze 内完成结果计算，同时通过 `cache_notify` 把 scan
范围写入 vendor log，最后发送 FINAL event。kernel 返回后 guest 解冻，CEMU scheduler
按照 kernel 返回的 `modeled_cycles` 和 `flash_read_bytes` 计算设备时延：

```text
T_device = max(ceil(modeled_cycles / 250 MHz),
               ceil(flash_read_bytes / 1000 MiB/s))
```

Program Execute 在该时延结束前保持 outstanding。默认 `csd.cache_overlap=on` 时，NVMe
AER completion 会立即调度 Linux cache worker，由 SSD CPU 路径提交 inode readahead
填充 PostgreSQL heap inode page cache；设置为 `off` 时，driver 将同一 AER 延迟到
Program Execute 完成后再处理。后者仅用作可控基线。`csd.program_runtime=0`，所以本次
运行使用自动时延模型，而非手工固定延迟。

## 方法

两组都执行 forced-CSD `SELECT count(*) FROM lineitem`，并设置
`csd.cache_wait_ms=180000`，以便在返回结果前确认全部缓存回填完成。每组查询前停止
PostgreSQL、`sync`、清空 guest page cache，然后重新启动。正确结果为 6,001,215 行。

驱动在 Program Execute ioctl 前后记录 `execute_start_ns`/`execute_complete_ns`。旧版
在每个 1 MiB `kernel_read` 完成时计数；最终版不再把数据复制到临时缓冲区，而是按
backing-device readahead window 提交 `POSIX_FADV_WILLNEED`，每 32 MiB 形成一个流水
提交组，并仅在该组 folio 全部 Uptodate 后计数。因此两版重叠量都表示命令尚未完成时
已经进入 heap inode page cache 的真实字节数，不是发出预读或收到 AER 的字节数。

## 原始结果

首次容器冷启动：

| overlap | Query wall | Execute window | First prefetch | Total prefetched | During execute |
| --- | ---: | ---: | ---: | ---: | ---: |
| off | 48,151 ms | 1,219 ms | 1,831 ms | 1,276,665,856 B | 0 B |
| on | 27,562 ms | 1,217 ms | 59 ms | 1,276,665,856 B | 36,700,160 B |

同一实例完成预热后的复测：

| overlap | Query wall | Execute window | First prefetch | Total prefetched | During execute |
| --- | ---: | ---: | ---: | ---: | ---: |
| off | 24,351 ms | 1,218 ms | 1,248 ms | 1,276,665,856 B | 0 B |
| on | 24,398 ms | 1,218 ms | 389 ms | 1,276,665,856 B | 17,825,792 B |

去掉 TCG 临时缓冲复制并加入 32 MiB 流水提交后的最终复测：

| overlap | Query wall | Execute window | First prefetch | Total prefetched | During execute |
| --- | ---: | ---: | ---: | ---: | ---: |
| off | 13,198 ms | 1,218 ms | 1,503 ms | 1,276,665,856 B | 0 B |
| on | 11,970 ms | 1,217 ms | 343 ms | 1,276,665,856 B | 134,217,728 B |

最终版开启重叠时，首末回填完成时间相差 10.553106 s，有效吞吐为
115.37 MiB/s；旧 warm 版本相差 22.973111 s、53.00 MiB/s，因此该 TCG 路径的有效
回填吞吐提升 2.18x。关闭重叠时对应值为 10.570109 s、115.19 MiB/s，说明吞吐改善
来自回填实现，不是查询模式之间的计时偏差。将 block readahead 从 128 KiB 增至 4 MiB
的探索实验反而失去计算重叠并增加 wall time，已恢复默认值，未作为最终配置。

三组验证均满足：关闭时首个 read completion 不早于 execute completion 且重叠量为 0；
开启时首个 completion 严格位于 execute window 内且重叠量大于 0。每组有一个 data
event 和一个 FINAL event，跨两个 heap segment 完成两个请求，`errors=0`。

原始 CSV 和完整 ioctl JSON 位于：

- `tpch_sf1_cache_compute_overlap.csv` 与 `_off.json`/`_on.json`：首次冷启动；
- `tpch_sf1_cache_compute_overlap_warm.csv` 与 `_warm_off.json`/`_warm_on.json`：预热复测。
- `tpch_sf1_cache_pipeline_raw/overlap.csv` 与 `off.json`/`on.json`：最终
  no-copy/pipelined 实现。

## 结论边界

实验直接证明 CEMU 在保留 TCG VM-Freeze 语义的同时，能够在模型化 FPGA/Flash 设备
执行窗口内由 guest SSD CPU 并行完成真实 cache fill。最终版在 1.218 秒窗口中完成
128 MiB，但完整 1.27 GB 仍需约 10.55 秒；因此它没有证明当前查询本身已有显著
wall-time 加速。11.970 s 与 13.198 s 的单次差值不能直接归因于重叠，论文应以明确
完成时间线和后续 HOST 查询收益作为证据。115.37 MiB/s 是 TCG/Linux 原型路径吞吐，
不是对真实 NVMe SSD 带宽的模拟结果。

缓存的可测收益体现在后续查询：既有三轮实验中，完整 AER 回填使下一条 HOST
`count(lineitem)` 均值从 22,537.964 ms 降至 10,818.313 ms，即 2.0833x；详见
`tpch_sf1_aer_cache_validation.md`。最终 no-copy/pipelined 版本的一轮复测为
23,492.340 ms 降至 10,474.203 ms，即 2.2429x，原始结果位于
`tpch_sf1_aer_cache_pipeline_raw/`。真实硬件或校准后的更长计算窗口可以覆盖更多传输，
但论文不得用本结果宣称同一 CSD 查询已获得显著加速。
