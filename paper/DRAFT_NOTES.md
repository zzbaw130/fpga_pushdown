# 初稿审查与证据索引

本轮按英文 PVLDB 模板撰写 `main.tex`，正文约 6,200 词。日期：2026-10-23。
保留 `template.tex`、模板样式和原有示例素材；不运行 LaTeX/BibTeX，不启动数据库实验。
作者、单位、DOI、页码、公开 artifact 地址需作者后续填写。

## 组织与主张

章节顺序为 Introduction → Background and Design Requirements → System Overview →
Device SQL Execution with Limited Memory → AER-Based Host Cache Population →
Cache-Aware Execution Placement → Evaluation → Discussion and Limitations →
Related Work → Conclusion。

引用会话 `6ada4aa2-cecc-83ec-a0d6-9c4b1e5d0d0f` 的可读用户消息明确了三项边界：
覆盖 TPC-H 所需算子，不涵盖 UDF/MVCC；按节点自底向上选择位置；规划时观测缓存并降低
HOST 代价。会话读取工具没有返回两条历史助手答复的正文，而仅返回内容引用标记，
因此本稿依据可读讨论和当前实现组织，未假称逐字复现历史助手建议。

主线是设备 SQL 执行、AER 回填、缓存感知位置选择之间的闭环；接口作为实现基础说明。
代码确认了 CEMU program download/load、MRS、Program Execute 等机制，尚未找到完整
SNIA API 逐项映射/符合性报告，因此没有把“完整实现 SNIA 标准”写成已验证贡献。
SNIA API v1.0 的官方文档日期为 2023-10-03，与 2022 年的架构规范不同。

## 代码核对入口

| 内容 | 仓库路径 |
| --- | --- |
| ABI v8、节点、表达式、状态字段 | `code/test-cemu/kernel/csd_plan.h` |
| DAG 调度、arena、周期累计和执行入口 | `code/test-cemu/kernel/csd_sql.cc` |
| Scan/FIFO/Join/Aggregate/Sort | `code/test-cemu/kernel/csd_{scan,fifo,join,aggregate,sort}.cc` |
| HOST/CSD frontier、mincore、资源准入、边界代价 | `code/postgres-csd/csd_cost.c` |
| lowering、CustomScan、extent、回填注册 | `code/postgres-csd/csd_lower.c`、`csd_wire.c`、`csd_executor.c` |
| GUC 默认值 | `code/postgres-csd/csd_pg.c` |
| NVMe host cache worker | `code/linux-cemu/drivers/nvme/host/cemu.c` |
| 频率与带宽默认值 | `code/CEMU/hw/femu/config_example.json` |
| 双时钟采集方法 | `tests/benchmark_csd_tpch.exp` |

当前代码中包含本轮开始前已存在的未提交 SF=16 planner/cost 改动。正文描述当前机制，
但 SF=1 历史时间仍严格归属于各报告指定的版本，不宣称本轮执行过当前代码回归。

## 图表与数字来源

所有本地实验路径均相对于仓库根目录。

| 正文内容 | 来源与解释 |
| --- | --- |
| 22-query AUTO 图、18/2/2 布局、总时间与几何均值 | `results/tpch_sf1_auto_global_search.{csv}` 和 `_validation.md`；2026-10-09，`csd_sql_overlap_v2.so` |
| AUTO exact 和最终 planner 证据 | `results/tpch_sf1_auto_global_search_raw.tar.gz`；性能轮先于 scan-count 等价键最后收紧，最终布局相同，报告有明确说明 |
| 三轮后续 HOST 缓存收益 | `results/tpch_sf1_aer_cache_validation.{csv,md}`；旧 worker，完整回填后再测 HOST |
| 最终 worker 单轮复测 | 同上报告及 `results/tpch_sf1_aer_cache_pipeline_raw/`；单轮，不与旧三轮合并 |
| 128 MiB overlap、343 ms、1.217/1.218 s | `results/tpch_sf1_cache_pipeline_raw/overlap.csv`；`results/tpch_sf1_cache_compute_overlap.md` |
| 环形日志全局 dropped=30 | `results/tpch_sf1_aer_cache_validation.md`；完整连续回归累计，不是独立 count 三轮 |
| Q1/Q6 流式 state peak | `results/tpch_sf1_streaming_aggregate_validation.md`；v3，2,528/134 B 只计 operator lease |
| 1 MiB Join、Aggregate replay | `results/tpch_sf1_join_spill_replay_validation.md`、`tpch_sf1_aggregate_spill_replay_validation.md` |
| Q3 two-sided streaming | `results/tpch_sf1_streaming_build_validation.md`；32 MiB arena，v2 |
| Q3 descriptor 与 ordinal handle | `results/tpch_sf1_spill_chain_validation.md`、`tpch_sf1_ordinal_merge_validation.md`；不同阶段，payload 不是总物理内存 |
| Sort 最终直出 | `results/tpch_sf1_sort_final_stream_validation.md`；14,646,232 → 9,420,282 B |
| Sort SQL Top-N | `results/tpch_sf1_sort_topn_validation.md`；9,420,282 → 69,190 B，SQL 输出要求不同 |
| Sort adaptive k-way | `results/tpch_sf1_sort_kway_validation.md`；229 runs，3,680,832 → 1,183,632 B |
| spill 时延一致性 | `results/tpch_sf1_spill_timing_validation.md`；两对样本，不是硬件校准 |

总耗时比值、逐查询比值和几何均值由 CSV 重新计算。正文不使用旧
`tpch_sf1_csd_performance.*` 的 4.531x 墙钟比值替代 AUTO guest 数据。
AUTO 历史轮早于后续 spill-aware 时延与 streaming 优化，不能把新公式、新优化与旧
AUTO 数字合并为同一版本的最终性能结论。全套性能每条一对样本，没有置信区间。

## 图片与复现

- `figures/architecture.tex`：据实现重画的 TikZ 架构图，明确 AER/log 控制面与普通读取。
- `figures/timeline.tex`：TikZ 时序示意，不按真实持续时间比例；区分冻结区与 modeled window。
- `figures/sf1_auto_speedup.{pdf,png}`：22 条查询、按 placement 着色的对数坐标比值图。
- `figures/sf1_cache_benefit.{pdf,png}`：旧 worker 三轮的均值与原始散点。
- `figures/sf1_sort_io.{pdf,png}`：三个独立对照，明确不同基线和 SQL Top-N 语义。
- `plot_results.py`：读取 CSV/报告中的已测字节，使用 matplotlib 生成上述实验图。
  普通 Python 环境安装 matplotlib 后执行 `python paper/plot_results.py`；不运行实验或 LaTeX。
- `references.bib`：使用官方规范、作者论文和原项目资料，替代模板示例参考文献。
  目前引用 NASCENT 原论文和 Trinity Hot Chips 2022 作者材料；未伪造 NASCENT2 的全文核对。

## 下一轮值得补充的实验证据

现有数据足以形成有边界的初稿，以下未执行内容不能写成已验证结论：

1. 同一最终 kernel 下匹配的 HOST / forced CSD / AUTO，重复全套冷/热实验并报告分布。
2. 原生 HOST 与独立校准设备模型/真实硬件的对照；补齐每轮宿主 CPU、guest RAM、
   PostgreSQL 内存和并行参数。不能把同 guest 的 TCG HOST 当作真实 CPU 基线。
3. AER 开/关、overlap 开/关与实际 resident fraction 的多查询序列；把额外读取、回填
   成本、淘汰与总完成时间计入，检验闭环整体收益。
4. mincore 冷到热的 residency sweep，明确 HOST/CSD/mixed 切换阈值及估计误差。
5. 常驻 / spill、不同 FDM / staging / fan-in、skew 与较大数据集的敏感性；测物理 RSS
   和临时副本，补 arena-backed allocator 前不宣称 4 GiB 硬上界。
6. 逐项 SNIA API 符合性、生产级 relation 生命周期一致性以及硬件综合/功耗。
7. 扩展 related work，系统比较已有缓存协作与自适应下推研究，确定最终 novelty 表述。

本轮仅进行数据和源码静态核查，未执行 PostgreSQL/CEMU 回归，也未编译论文。
