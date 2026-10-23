# 论文架构初稿审查说明（修订版）

本稿用于指导后续实现和填充实验，采用英文 PVLDB 模板。2026-10-23 根据用户的新方向
重写：最终源码以 `~/docker_workspace/femu_docker` 为基础，设备计算单元采用 CPU，
应用通过 SNIA API 组织算子、缓冲和依赖。系统暂名 **CoSQL**，可后续替换。
本轮没有修改 `femu_docker` 的代码，没有运行数据库实验，也没有编译 LaTeX。
第三轮按用户要求增加文献动机、API/重入 demo、公式释义与六组理论图。

## 四项贡献与论文主线

1. 标准接口：SNIA API 公共层与 FEMU adapter 分离，应用不直接拼装 NVMe 传输。
2. 完整算子链：CPU CSF、列批次/SIMD、sink/source、共享工作内存和外部执行，
   超越 Filter-only 的中间结果返回。
3. 主动缓存回填：AER 通知 → 范围日志 → 正确 heap inode 的普通读取，形成后续缓存收益。
4. 缓存感知位置选择：节点级 HOST/CSD 动态规划，将已有缓存、设备计算、spill 和回传代价结合。

Introduction 用四个递进缺口引出这四项贡献；Related Work 增加 Ibex、PushdownDB、
FlexPushdownDB 和 adaptive pushdown；本轮进一步阅读 YourSQL、Ibex、POLARDB、AQUOMAN 原论文，重写 Background & Motivation。不能写“所有/绝大多数现有工作都只支持 Filter”：
Ibex 已支持聚合，FPDB 已联合缓存与下推；Filter-only 是本稿的受限对照组。

按用户要求删除旧稿的逐项实现审计、SNIA 未实现说明、FPGA 周期/II 模型、VM-Freeze
计时章节以及大量旧项目边界讨论。正文是目标架构，非当前两套代码的功能验收报告。
实验明确区分两类内容：已有 measured anchors 与 analytical projections。用户授权估算后，
用显式假设计算五组消融的理论情景，移除空白主结果表；图面采用正常标题，CSV 中保留
MODEL_DERIVED 来源字段，不把情景当作新版 CPU/SNIA 实测或新正确性证据。

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

正文已将主结果表模板替换为理论图；后续仍需按以下项目补充真实测量：

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
最终 CPU 方案的实测耗时。因此它们只在 reference 小节使用。五组比较的实测结果尚未补齐；本轮在单独 model/ 目录中给出明确标注的理论情景，不是补做了实验。

`plot_results.py` 保留旧参考图脚本。本轮开始时 architecture.tex、timeline.tex 和
sf1_auto_speedup.{pdf,png} 已有用户修改，本轮不覆盖；正文改用单独的
`architecture_cpu.tex` / `timeline_cpu.tex`，维持 CPU/SNIA 叙述与用户工作区。
原始测量数据不修改。`references.bib` 新增 YourSQL、POLARDB、AQUOMAN。

本轮验证：检查引用/标签/输入文件、LaTeX 括号与环境配对，重新生成参考实验图；
不进行 LaTeX 编译或新的数据库回归。作者、单位、DOI、页码和公开代码地址仍待填写。


## 原论文阅读入口与表述依据

- YourSQL，PVLDB 2016：[原文](https://www.vldb.org/pvldb/vol9/p924-jo.pdf)，
  §2.2、§3.1--3.2：ISC tasks、match hints、bulk prefetch；不是完全没有预取。
- Ibex，PVLDB 2014：[原文](https://www.vldb.org/pvldb/vol7/p963-woods.pdf)，
  §3.2、§5：存储引擎/寄存器接口，hash collision bypass 是具体 host assistance 例子。
- POLARDB，FAST 2020：[原文](https://www.usenix.org/system/files/fast20-cao_wei.pdf)，
  §2.2、§3.1：跨数据库、PolarFS、驱动的 scan 请求；不编造其大型 Join 溢出机制。
- AQUOMAN，MICRO 2020：[作者原文](https://people.csail.mit.edu/bthom/micro20-2.pdf)，
  §VI-E：容量导致 host 接续；§VIII-A：讨论后采用 cold page cache。不能说作者完全没考虑缓存。

新的动机是“接口分散、capacity host assistance、完整卸载后的主动 heap-cache refill”，
不是宣称所有前人只做 Filter、所有外部算法都回退 HOST，或完全没有缓存研究。

## 两个最小 demo

- Listing `lst:snia`：SQL SUM + predicate，SERIAL batch 组织 LOAD→Scan→Filter→Aggregate；
  只回读最终 sum/count 状态。公开 cs-prefixed API 保留真实签名，应用构造器是明确标注的伪代码。
- Listings `lst:sortcontinuation`、`lst:plugincontinuation`：Sort 保存 cursor/pending，返回
  `csNeedStore`；plugin 分配 extent、STORE、设置 task.reentry（映射 cparam2），然后重入。
  CSF 自己更新私有 run 状态，插件不直接改私有排序结构。示例只用一个 spill block，实际大 run 循环。
- 公式后逐项解释符号、单位和候选状态，包括 memory、CPU/I/O cost、DP、cache、projection 和 Sort 模型。

## 理论模型与新图

执行 `python paper/project_results.py`（依赖 numpy/matplotlib）生成以下图，每幅同时有 PDF/PNG：

1. `theory_query_comparison`：22-query cold/warm 五组比值。
2. `theory_suite_totals`：cold/warm 总耗时。
3. `theory_sequences`：重复 reduction 与 mixed sequence 的累计耗时。
4. `theory_traffic`：归一化 foreground/refill 传输和后续 forced-HOST 读取。
5. `theory_memory_spill`：256 MiB Sort，在不同 budget 下的算法内存与理想 spill。
6. `theory_cache_crossover`：probe 的 cache residency 位置切换。

`figures/theory_overview.png` 是上述六组图的审查总览；正文分别引用各 PDF。

模型输入为旧 AUTO CSV 的 H/A 参考时间、三轮 cache latency 比值。alpha=.55、逐 query 的
sigma、forced-device penalties=1.20/1.25、refill contention=.03、mixed host fraction=.30
均为显式**假设**；不能写成测出的 selectivity、CPU throughput 或真实 planner 准确率。
Full system 用候选最小值，是偏乐观的离线规划代理，而非执行了 DP。比例 .52 来自 count
复用实验，跨查询使用是模型假设。cold/warm 分别是各 query 初始缓存情景；多查询序列则
从 cold 开始，让 Host-only 也获得自身扫描的缓存收益，不给回填组免费预热。

`model/assumptions.json` 保存全部假设，`summary.json` 保存汇总；query、sequence、traffic、
memory、crossover CSV 均有 MODEL_DERIVED 字段。cold 总时间依次为
825.114 / 622.337 / 207.613 / 213.841 / 184.196 reference-scale s；warm 为
396.058 / 298.953 / 207.613 / 213.841 / 127.109。它们不是物理 CPU 时间预测。

Sort 情景用 P=A+min(S,W)、I=2max(0,S-W)，忽略元数据和额外归并轮次，只描述理想
payload 下限，不能冒充进程 RSS 或严格 allocator 证明。反例也保留：只重复 reduction
时 CSD+refill 比 CSD-only 慢 3%；mixed 情景 Filter-only 可因只缓存部分页而慢于 Host-only。
原 results/ 目录完全不写入理论数据。


## 图文呈现修订

按用户要求取消图面横幅，标题/图注采用正常实验主题；正文集中保留 Evaluation Model、
参数来源和计算方式，不反复插入警示段落。数值没有变化，未新增测量。
JSON/CSV 中用 MODEL_DERIVED 标识计算输出，原始 results/ 数据不改。
