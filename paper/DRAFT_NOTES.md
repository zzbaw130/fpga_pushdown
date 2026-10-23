# 论文架构初稿审查说明（修订版）

本稿用于指导后续实现和填充实验，采用英文 PVLDB 模板。2026-10-23 根据用户的新方向
重写：最终源码以 `~/docker_workspace/femu_docker` 为基础，设备计算单元采用 CPU，
应用通过 SNIA API 组织算子、缓冲和依赖。系统暂名 **CoSQL**，可后续替换。
本轮没有修改 `femu_docker` 的代码，没有运行数据库实验，也没有编译 LaTeX。

## 四项贡献与论文主线

1. 标准接口：SNIA API 公共层与 FEMU adapter 分离，应用不直接拼装 NVMe 传输。
2. 完整算子链：CPU CSF、列批次/SIMD、sink/source、共享工作内存和外部执行，
   超越 Filter-only 的中间结果返回。
3. 主动缓存回填：AER 通知 → 范围日志 → 正确 heap inode 的普通读取，形成后续缓存收益。
4. 缓存感知位置选择：节点级 HOST/CSD 动态规划，将已有缓存、设备计算、spill 和回传代价结合。

Introduction 用四个递进缺口引出这四项贡献；Related Work 增加 Ibex、PushdownDB、
FlexPushdownDB 和 adaptive pushdown。不能写“所有/绝大多数现有工作都只支持 Filter”：
Ibex 已支持聚合，FPDB 已联合缓存与下推；Filter-only 是本稿的受限对照组。

按用户要求删除旧稿的逐项实现审计、SNIA 未实现说明、FPGA 周期/II 模型、VM-Freeze
计时章节以及大量旧项目边界讨论。正文是目标架构，非当前两套代码的功能验收报告。
保留实验开头一段说明：旧项目数据是 preliminary reference，不将其伪装成新版 CPU/SNIA
五组消融的实测结果。未测数字留空，不通过重命名旧数据制造新实验结果。

## femu_docker 中实际参考的文件

以下路径均相对 `/home/dengyao/docker_workspace/femu_docker`：

| 参考文件 | 提取到正文的内容 |
| --- | --- |
| `code/apps/snia/README.md` | `cs.h`/`libsnia`、AFDM、函数调用、batch、重入 I/O |
| `code/apps/snia/include/cs.h` | 发现、内存、计算、存储、batch 和 event 的公开 API 名称 |
| `code/apps/snia/doc/architecture.md` | 公共层/插件边界、句柄生命周期、SERIAL/PARALLEL/HYBRID、host-side scheduling |
| `code/apps/snia/src/plugins/femu/README.md` | NVMe 映射、单个请求执行、STORE/LOAD/FREE 重入 |
| `code/apps/snia/examples/README.md` | 四路 LOAD/sink 和 source→avg 的应用编排示例 |
| `code/apps/snia/examples/cs_batch_sort_avg.c` | 真实 API 调用、请求配置、函数加载与激活 |
| `code/apps/pg_sina_csd/README.md` | PG 应用构造计划和 SNIA 请求、可并行流水线/执行槽 |
| `code/apps/pg_sina_csd/src/executor/batch.c` | 扩展直接调用 API、共享 AFDM、buffer/slot 管理 |
| `code/csd-programs/pg_sina_csd/README.md` | CPU CSF、256 行列向量、SIMD、int128、Join/Agg/Sort 的工作方式 |
| `code/csd-programs/csd-workmem/README.md` | AFDM 与私有 workmem 分开、64 MiB 初始设置、根/子 context 共享计费 |

架构分工必须保持：**应用确定算子图和 pipeline，SNIA 公共库调度应用指定的依赖，
FEMU 插件转换请求，设备 CPU 执行 CSF**。不能恢复成设备通用 runtime 解析完整 SQL DAG。
应用自己的设备函数可以解释应用描述，仍不意味着 SNIA/FEMU 理解 SQL 或自行编排。

SNIA 公共 API 与 FEMU 特定 spill continuation 要区分：后者利用 CompValue 和重入
约定，不把 STORE/LOAD/FREE token 编码写成 SNIA 通用标准。

## 五组实验结构

| 对照组 | 下推范围 | 回填 | 自适应位置选择 |
| --- | --- | --- | --- |
| Host-only | 无 | 无 | 无 |
| Filter-only | Scan/Filter；其余 HOST | 无 | 无 |
| CSD-only | 完整支持的算子链，强制 CSD | 无 | 无 |
| CSD + Cache Refill | 与 CSD-only 同一强制计划 | 有 | 无 |
| Full system | 节点级 HOST/CSD | 有 | 有 |

正文已提供主结果表模板。建议依次补充：

- 冷启动 Q1--Q22：五组每条查询时间、总时间、传输字节和正确性。
- 热缓存 Q1--Q22：检查已有缓存是否改变位置选择。
- 先 CSD 后 HOST 的两查询实验：完整回填屏障只用于隔离潜在收益。
- 无屏障多查询序列：计入回填成本，比较总完成时间、额外 I/O 和缓存污染。
- CU=1/2/4、AFDM/workmem 独立扫描、spill 和内存敏感性。
- API 请求、batch 并发、事件与重入开销。
- Full system 对比强制 CSD+回填、cache-oblivious planner 和离线最佳参考。

固定 CPU/CU 数、总内存和带宽；并发请求耗时有重叠，不能累加当总延迟。

## 暂时复用的 SF=1 参考数字

以下路径相对当前 `fpga_pushdown` 仓库，均为已有证据，不是本轮测量：

| 数据或图 | 既有来源 |
| --- | --- |
| `sf1_auto_speedup`；183,019.742 / 825,114.403 ms；18/2/2；4.5083、6.9207 | `results/tpch_sf1_auto_global_search.csv` 及 `_validation.md` |
| `sf1_cache_benefit`；三轮 22,537.964 → 10,818.313 ms | `results/tpch_sf1_aer_cache_validation.{csv,md}` |
| 128 MiB overlap，约 1.218 s window | `results/tpch_sf1_cache_pipeline_raw/overlap.csv` 与 `tpch_sf1_cache_compute_overlap.md` |
| Q1/Q6 state peaks 2,528 / 134 B，29 batches | `results/tpch_sf1_streaming_aggregate_validation.md` |
| 1 MiB Join/聚合 replay | `results/tpch_sf1_{join,aggregate}_spill_replay_validation.md` |
| `sf1_sort_io` 三组 before/after | `results/tpch_sf1_sort_final_stream_validation.md`、`tpch_sf1_sort_topn_validation.md`、`tpch_sf1_sort_kway_validation.md` |

旧 AUTO 数字包含参数化设备时延；CPU 函数同样由 CPU 执行并不让旧 guest 时间自动变成
最终 CPU 方案的实测耗时。因此它们只在 reference 小节使用。五组比较结果尚未补齐。

`plot_results.py` 可重画三幅参考图；`figures/architecture.tex` 和 `timeline.tex` 已重画为
目标 CPU/SNIA 架构和正常并发时间线。原始测量数据不修改。`references.bib` 新增上述
对比论文，保留已有文献条目以供后续扩展。

本轮验证：检查引用/标签/输入文件、LaTeX 括号与环境配对，重新生成参考实验图；
不进行 LaTeX 编译或新的数据库回归。作者、单位、DOI、页码和公开代码地址仍待填写。
