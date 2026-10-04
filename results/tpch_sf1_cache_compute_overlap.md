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
AER completion 会立即调度 Linux cache worker，由 SSD CPU 路径发普通 buffered reads
填充 PostgreSQL heap inode page cache；设置为 `off` 时，driver 将同一 AER 延迟到
Program Execute 完成后再处理。后者仅用作可控基线。`csd.program_runtime=0`，所以本次
运行使用自动时延模型，而非手工固定延迟。

## 方法

两组都执行 forced-CSD `SELECT count(*) FROM lineitem`，并设置
`csd.cache_wait_ms=180000`，以便在返回结果前确认全部缓存回填完成。每组查询前停止
PostgreSQL、`sync`、清空 guest page cache，然后重新启动。正确结果为 6,001,215 行。

驱动在 Program Execute ioctl 前后记录 `execute_start_ns`/`execute_complete_ns`，并在
每个 1 MiB `kernel_read` 完成时记录首末时间及 `overlap_prefetched_bytes`。因此重叠量
表示命令尚未完成时已经完成的真实文件读取，不是发出 `fadvise` 或收到 AER 的字节数。

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

两轮均满足：关闭时首个 read completion 不早于 execute completion 且重叠量为 0；
开启时首个 completion 严格位于 execute window 内且重叠量大于 0。每组有一个 data
event 和一个 FINAL event，跨两个 heap segment 完成两个请求，`errors=0`。

原始 CSV 和完整 ioctl JSON 位于：

- `tpch_sf1_cache_compute_overlap.csv` 与 `_off.json`/`_on.json`：首次冷启动；
- `tpch_sf1_cache_compute_overlap_warm.csv` 与 `_warm_off.json`/`_warm_on.json`：预热复测。

## 结论边界

实验直接证明 CEMU 在保留 TCG VM-Freeze 语义的同时，能够在模型化 FPGA/Flash 设备
执行窗口内由 guest SSD CPU 并行完成真实 cache reads。它没有证明当前查询本身已有
显著 wall-time 加速：TCG guest 完整搬运 1.27 GB 约需 24 秒，远长于 1.218 秒窗口，
预热复测的 47 ms 差异属于噪声。首次 20.6 秒差异主要是后端冷启动，不能归因于重叠。

缓存的可测收益体现在后续查询：既有三轮实验中，完整 AER 回填使下一条 HOST
`count(lineitem)` 均值从 22,537.964 ms 降至 10,818.313 ms，即 2.0833x；详见
`tpch_sf1_aer_cache_validation.md`。真实硬件或校准后的更长计算窗口可以覆盖更多传输，
但论文不得用本结果宣称同一 CSD 查询已获得显著加速。
