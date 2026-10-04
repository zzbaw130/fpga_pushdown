# SF=1 AER 主机页缓存回填验证

日期：2026-10-04。分支：`dev`。数据库：TPC-H SF=1，PostgreSQL 12.22，
PGDATA 位于 CEMU NVMe `/mnt/nvme0/pgdata/tpch_s1`。CEMU 使用 TCG + VM-Freeze，
没有使用 KVM。

## 方法

被测查询为 `SELECT count(*) FROM lineitem`，CSD 与 HOST 都返回 6,001,215。
每个样本按以下顺序执行：

1. 停止 PostgreSQL，执行 `sync` 和 guest `drop_caches=3`，再启动 PostgreSQL，
   从而同时清除 PostgreSQL shared buffers 和 guest filesystem page cache。
2. 强制 `csd.exe_path=csd` 执行同一查询。baseline 设置 `csd.cache_fill=off`；
   AER 组设置 `csd.cache_fill=on`、`csd.cache_wait_ms=180000`。
3. 紧接着强制 `csd.exe_path=host` 执行相同查询，以
   `EXPLAIN (ANALYZE, BUFFERS, TIMING OFF)` 的 PostgreSQL `Execution Time` 计时。

AER 组的显式等待只用于建立可重复的实验屏障。正常配置默认
`csd.cache_wait_ms=0`，cache worker 异步运行。guest wall clock 会在 VM-Freeze 时暂停，
因此没有用 guest shell `time` 统计 CSD wall time。

## 原始结果

| Round | 回填关闭 HOST | AER 回填后 HOST | 本轮 speedup |
| ---: | ---: | ---: | ---: |
| 1 | 22,732.395 ms | 10,681.806 ms | 2.1281x |
| 2 | 22,226.711 ms | 10,887.709 ms | 2.0414x |
| 3 | 22,654.786 ms | 10,885.423 ms | 2.0812x |
| Mean | **22,537.964 ms** | **10,818.313 ms** | **2.0833x** |

平均 HOST 延迟降低 52.00%。三轮 AER 统计完全相同：1 个合并后的 relation event，
`notified_bytes=1,276,665,856`，跨两个 PostgreSQL heap segment 形成 2 个 buffered-read
请求，`prefetched_bytes=1,276,665,856`，`unmapped_bytes=0`、
`controller_dropped=0`、`errors=0`。原始数字也保存在
`tpch_sf1_aer_cache_validation.csv`。

## 正确性回归

启用默认异步回填（`csd.cache_fill=on`、`csd.cache_wait_ms=0`）后，TPC-H Q1-Q22
的 forced-CSD 输出仍与 HOST 输出逐字一致，22/22 `EXACT`。88 个 raw/normalized
输出保存在 `results/tpch_sf1_aer_exact_final.tar.gz`；归档 SHA-256 为
`5fc0083c0424f52351a08095071def4bb454995fee7e5cd3bd79d3a38b48ee11`。

完整连续回归结束时，最后一个 generation 回填 243,138,560 B，零 unmapped/error；
controller 的全局累计 dropped 为 30。它说明 64-entry bounded ring 在部分复杂查询的
一次 VM-Freeze 窗口内可能被覆盖。当前原型按 best-effort cache hint 处理，丢事件只
降低缓存收益、不会影响 SQL 结果；论文不能把连续 22-query 工作负载描述为零丢弃。

## 模拟器边界与后续重叠实现

本文数据来自最初的缓存收益阶段，当时 Program Execute 没有在 functional kernel 返回
后保留独立的模型化设备窗口，所以它只证明回填能够加速后续 HOST 查询。后续实现已
保留 VM-Freeze：C++ functional kernel 仍在冻结区间运行；kernel 返回并解冻 guest 后，
CEMU 根据 FPGA 周期和 Flash 字节数继续保持 NVMe 命令 outstanding，Linux AER worker
可在该窗口内运行。SF=1 已实测 17--37 MB buffered reads 在约 1.218 s 命令窗口内完成，
详见 `tpch_sf1_cache_compute_overlap.md`。不得把冻结区间的 C++ wall time 计入 FPGA
时延，也不得把该重叠结果扩写为当前查询已有显著 wall-time speedup。
