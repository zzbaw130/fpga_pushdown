# FPGA Pushdown 项目上下文与实验记录

本文件是本仓库的长期上下文。后续开发者或自动化代理在修改代码前应完整阅读，
并把已经验证的新阶段、实验参数、结果路径和明确的局限同步补充到这里。本文既服务
工程接续，也为论文的系统设计、实现和实验章节保留可追溯事实。不要把尚未验证的
设计写成已实现结论。

## 1. 研究目标与三项贡献

项目目标是在 CEMU 模拟的 Computational Storage Device（CSD）内执行
PostgreSQL 12 的 SQL 计划，只向主机返回结果，并完整支持 TPC-H Q1-Q22。

1. FPGA 风格 SQL 引擎：PostgreSQL 将版本化执行计划通过 NVMe Program Execute
   发送给 CEMU。CSD 入口是参数为 `cemu_args` 的共享库函数 `csd_sql`，它解析
   计划 DAG、划分 streaming/blocking pipeline，并调度独立算子。C++ 代码模拟
   FPGA 的固定数据通路、复制计算 lane、流水 II 和有界片上资源，不把主机 CPU
   执行时间直接宣称为 FPGA 时间。超出 FDM 内存的 Sort、Join、Aggregate 使用
   query-local NVMe 临时区完成 spill-to-flash 和 replay。
2. AER 缓存回填：CSD 读取一个表数据 chunk 后，使用 vendor-specific NVMe AER
   通知主机。AER 只传递“有新范围可回填”的事件，范围元数据由 vendor log page
   读取；主机随后发普通读取/预读操作来填充缓存，并与 CSD 计算重叠。AER 不是
   arbitrary DMA。当前阶段先追求可测缓存收益，只要求 plan generation 级的陈旧
   事件过滤，不实现生产级强一致性。
3. 动态执行位置：`csd.exe_path=host|csd|auto` 分别强制 HOST、强制 CSD、或让
   PostgreSQL 的两状态动态规划比较两类路径。CSD cost 包含 scan、算子、spill、命令
   启动和结果返回；HOST scan cost 由实际缓存率降低。默认
   `csd.host_cache_fraction=-1` 通过 `mincore` 自动采样每个 heap segment，`[0,1]`
   仍可作实验覆盖。CSD 可向 HOST 切换一次，HOST 中间结果禁止重新上传到 CSD。

## 2. 分支、稳定点和提交规则

- 日常开发只在 `dev` 分支进行；阶段完成后提交并推送 `origin/dev`。不要推送
  `master`，主分支由用户自行处理。
- AER 开发前的稳定备份是提交 `bfbba6964`，本地和远端分支名均为
  `pre-aer-baseline`。它包含完整 TPC-H 执行、性能基线以及 staging-bounded Join
  probe replay，但不包含 AER 实现。
- 开始任务前检查 dirty worktree。用户已有修改必须保留，不使用 destructive reset。
- CEMU 会保留已 `dlopen` 的同名共享库映射。每次在 guest 重新编译 kernel 后，测试
  应复制/重命名为新的版本化 `.so` 路径，或重启 CEMU；仅覆盖同名文件可能继续执行
  老代码。

主要里程碑提交：

| Commit | 内容 |
| --- | --- |
| `ddfbc8a72` | 可复现的 PostgreSQL/TPC-H/CEMU 环境 |
| `424abd650` | CSD 算子引擎与计划 ABI |
| `82503b857`-`dc16b1c6e` | PostgreSQL CSD path、scan/表达式/阻塞算子 lowering |
| `d75fc5a1d` | TPC-H 子计划解相关并合并为 CSD DAG |
| `4e37227e6`-`654f4449d` | PGDATA 进入 NVMe、Program Execute、Q1-Q22 执行打通 |
| `6ae061bc5` | FPGA 风格并行与 22 条查询加速基线 |
| `d9624155b` | extent-driven 分块 Flash 扫描，不再整表驻留 FDM |
| `77036c1a5` | 双 FDM FIFO |
| `eb07162a6` | query-level FDM operator arena |
| `9cad68664` | 外部 Sort Flash replay |
| `52ff70a3d` | Hash Join radix/Grace Flash replay |
| `3c30a101f` | grouped Aggregate radix Flash replay |
| `9c60f0858`-`ea0554c25` | ordinal 分类、阈值 spill、直接 selection-to-Flash writer |
| `bfbba6964` | Join probe staging-bounded 流式 replay；AER 前稳定点 |
| `4d2573947`-`71b1cf9ae` | AER 回填、计算/缓存重叠和 no-copy pipeline |
| `acd9213b2` | AUTO Pareto 候选搜索、expected/resource bound 分离、Q2/Q16 纯 HOST 修复 |
| `7418e0ac2` | spill 写/回读设备时延、Sort ordered-target schema boundary、SF=1 校准与 22/22 回归 |
| `601b25fdb` | Scan/FIFO batch 直连 external Sort、有界 run generation、SF=1 22/22 回归 |
| `923166681` | external Sort 有界 spill cursor/writer merge、1 MiB staging 验证与 SF=1 22/22 回归 |
| `20a83f89a` | 保守准入的 Scan/FIFO 直连 Aggregate、Q1/Q6 profile 与 SF=1 22/22 回归 |
| `dade2128f` | resident-build one-sided streaming Join、低内存回退、Q3/Q5 profile 与 SF=1 22/22 回归 |
| `c7ff19c3e` | oversized-build Join probe-to-Flash、32 MiB Q3 exact 与 SF=1 22/22 回归 |
| `8fbe3622a` | two-sided Scan Join build-to-Flash、自适应 fan-out、32 MiB Q3 与 22/22 回归 |
| `218ba67a3` | streamed Join Flash handle-chain、Q3 常驻描述符 1,024 B 与 22/22 回归 |
| `9ffae8781` | partitioned Join binary-carry ordinal merge、Q3 常驻 handle 150 B 与 22/22 回归 |
| `7ce97b472` | partitioned Join 逐行扇出输出、Q3 pending peak 598,680 B 与 22/22 回归 |
| `57b3c607d` | partitioned Join 最终 Flash run 直连 non-DISTINCT Aggregate、Q12/Q14/Q19 32 MiB 与 22/22 回归 |
| `1a16674e3` | Join-to-Aggregate 超限直接写 Flash radix chain、递归 replay 与 22/22 回归 |
| `1bd92f9ca` | external Sort 最终 merge-and-stream、spill I/O 降低 35.68% 与 22/22 回归 |
| `d3a586cd8` | SQL LIMIT/OFFSET 下推 external Sort Top-N、spill I/O 降低 99.27% 与 22/22 回归 |
| `f549976cc` | staging-bounded adaptive 3--128-way external Sort、229-run spill I/O 降低 67.84% 与 22/22 回归 |
| `04f44df08` | 复用式 Flash extent、Join chain/wave replay、SF=16 forced-CSD 22/22 exact |

## 3. 环境不变量

- CEMU 的核心是 TCG + VM-Freeze；禁止改成 KVM。Program Execute 仍通过原始
  NVMe queue 和 `NVME_IOCTL_IO_CMD` 进入 CEMU。科研环境给 guest PostgreSQL
  `/opt/pgsql12/bin/postgres` 设置 `cap_sys_admin=ep`，因为普通 `csdpg` 用户会被
  Linux ioctl 的 `CAP_SYS_ADMIN` 检查拦截。
- 主机编译 PostgreSQL 只用于快速创建 `data/tpch_s<SF>` 测试集群。真正执行 CSD
  查询的 PostgreSQL 必须在 CEMU guest 内编译一次并安装到 `/opt/pgsql12`，不能在
  guest 中运行主机编译产物。
- 9P 仅用于源码共享和一次性数据装载，不进入性能关键路径。PGDATA 从 9P 复制到
  CEMU NVMe 后运行，禁止每次查询复制整表或 cluster。
- 当前使用 CEMU 原版纯内存 NVM backend，不使用稀疏文件或持久 backing volume；FTL
  映射和 namespace 数据均不跨容器/控制器重启保留。重启后需重新执行
  `stage_tpch_nvme.sh`，不能把这段 9P->NVMe 恢复时间计入查询实验。
- `csd.execute_timeout_ms` 默认 600,000，只控制 host NVMe ioctl 对 TCG 功能模型的
  watchdog，不进入 FPGA/Flash 时延模型。VM-Freeze kernel 超时后 Linux abort/reset
  目前不能协作取消仍在 QEMU 中运行的 C++ worker；一旦发生超时，应重启 CEMU、重新
  stage PGDATA 后再测，不能继续使用可能带遗留热线程的环境。
- 默认先用 SF=1 做功能和快速回归，再用 SF=16 验证内存边界和可扩展性。服务器物理
  内存不足以同时容纳原版 pure-memory CEMU 的更大 namespace 与 SF=32 功能模型，故
  当前最大规模固定为 SF=16，不再使用此前试验过的 sparse backend。验证配置是
  48 GiB NVM namespace、4 GiB FDM、16 GiB guest RAM。
- FDMFS 表示 CSD 内存，不是无限磁盘。默认总容量 4 GiB。表数据按默认 32 MiB
  scan window 从 NVMe 分块进入 FDM；不得把大表整体复制到 FDMFS。
- FIFO 也消耗 FDMFS。每个 query 的 scan windows、两个 tuple FIFO、结果区、plan、
  status、spill staging 先从 4 GiB 扣除，剩余连续空间才是 operator arena。
- `csd.spill_mb` 是 NVMe Flash 上的临时文件容量，不计入 FDM；
  `csd.spill_stage_mb` 是 Flash I/O 使用的可复用 FDM staging window。

## 4. 目录与运行位置

| 路径 | 作用 |
| --- | --- |
| `code/CEMU` | 基于 FEMU/QEMU 的 CEMU 控制器，TCG VM-Freeze、FDMFS、CSD runtime |
| `code/linux-cemu` | guest Linux 6.8，含 CEMU/NVMe host driver 扩展 |
| `code/ubpf-cemu` | CEMU runtime 的 `ubpf_jit_args` ABI；修改 `cemu_args` 时必须同步 |
| `code/postgres` | 固定的 PostgreSQL REL_12_STABLE 源码 |
| `code/postgres-csd` | PGXS planner/executor/cost/lowering 扩展 |
| `code/test-cemu` | 唯一保留的 CSD kernel、runner、单元测试；挂载到 guest `/mnt/cemu` |
| `scripts/gen_data` | TPC-H cluster、schema、主外键和索引创建 |
| `tests` | planner、22-query exact comparison、performance、FDMFS reuse |
| `results` | 原始输出、验证报告、CSV/PNG/PDF；论文数据应从这里追溯 |
| `docs/csd_design.md` | 详细设计边界和算法 |
| `docs/tpch_operator_coverage.md` | PostgreSQL 计划节点与 CSD lowering 覆盖 |

常用 guest 配置：

- 容器：`cemu_fpga_pushdown`
- SSH：`ssh -p 2223 root@127.0.0.1`，开发镜像默认 root 密码 `root`
- guest PostgreSQL：`/opt/pgsql12`
- SF=1 PGDATA：`/mnt/nvme0/pgdata/tpch_s1`
- socket：`/tmp/csdpg_nvme_tpch_s1`
- port：`57432`
- database/user：`tpch_s1` / `dengyao`
- SF=16 对应路径/套接字/database 为 `/mnt/nvme0/pgdata/tpch_s16`、
  `/tmp/csdpg_nvme_tpch_s16`、`tpch_s16`，端口仍为 `57432`。
- guest 源码：`/root/CEMU/code/CEMU/tests/cemu` 或 `/mnt/cemu`
- CEMU 可见 kernel：`/workspace/code/CEMU/tests/cemu/build/<version>.so`

## 5. 数据与构建流程

主机数据准备：

```bash
git submodule update --init code/postgres
./scripts/build_postgres.sh
./scripts/gen_data/create_tpch_db.sh --sf 1
./scripts/gen_data/create_tpch_db.sh --sf 16 --keep-running
```

镜像和 guest：

```bash
docker compose build cemu
docker compose up -d
ssh -p 2223 root@127.0.0.1
/root/CEMU/scripts/guest/bootstrap_csdpg.sh       # fresh guest only
/root/CEMU/scripts/guest/build_postgres.sh        # guest compile, once
/root/CEMU/scripts/guest/stage_tpch_nvme.sh 1     # 9P -> NVMe, once
/mnt/cemu/scripts/guest/run_nvme_tpch.sh 1
```

`Dockerfile` 必须使 CEMU 编译错误传播为镜像失败。`femu-compile.sh` 历史上会在
内部 `make` 失败后仍返回 0，因此 Dockerfile 还需显式执行 Ninja 目标并检查
`qemu-system-x86_64` 存在。运行镜像保留 `libncurses-dev`，否则复制来的 Meson/Ninja
构建树无法增量链接 `libncursesw.so`。

guest 内只编译 CSD kernel 的快速命令：

```bash
cd /root/CEMU/code/CEMU/tests/cemu
rm -f build/csd_sql.so
make -C kernel ../build/csd_sql.so
cp build/csd_sql.so build/csd_sql_<new_version>.so
```

## 6. 计划 ABI 和 PostgreSQL lowering

- 当前稳定 ABI 为 v8。`CsdSqlPlanHeader` 后接固定大小 node table、表达式/descriptor
  payload 和 NVM extent。跨 NVMe 传输的是项目自定义、版本化 binary plan，绝不发送
  PostgreSQL 内部 struct 指针。
- PostgreSQL executor 使用 FIEMAP 获取所有 heap main-fork 的物理 extent。每个 Scan
  同时包含 relation OID、逻辑范围、namespace 和物理 offset。
- plan blob 通过 CSD MRS staged range 传输；`cparam1` 是 plan bytes，`cparam2` 是
  staged range index。这样避开 CEMU/Linux passthrough 中与标准 metadata 重叠的
  `dlen`。
- 支持的 ABI 节点：`SCAN`、`INDEX_SCAN`、`FILTER`、`PROJECT`、`HASH_AGG`、
  `GROUP_AGG`、`HASH_JOIN`、`MERGE_JOIN`、`NL_JOIN`、`SORT`、`MATERIALIZE`、
  `LIMIT`、`SUBPLAN`。
- `INDEX_SCAN` 暂规范化为 heap `SCAN+FILTER`；equality `MERGE_JOIN` 规范化为
  `HASH_JOIN`，并计入 canonicalization counter。`Gather` 是 host wrapper，不在 CSD
  模拟。
- Q2 的 correlated scalar `MIN` 被解相关为 grouped Aggregate + two-key Join；Q16
  的 `NOT IN` 仅在两侧列都证明 NOT NULL 时转为 hash anti-join。22 条查询均为零
  pending non-scalar expression。
- `SORT` 保持输入物理行布局，因此 upper-relation lowering 在其后显式追加保持顺序的
  `PROJECT`，把 Join 内部列裁剪/重排为 ordered `PathTarget`。否则 Q2 会把第三个
  numeric cell 按 `n_name BPCHAR` 解码；此规则是通用 schema boundary，不按查询编号特判。
- 表达式 IR 是有界 postfix VM，无 jump、递归、间接内存和 UDF。覆盖定点算术、
  comparison、SQL 三值逻辑、NULL、`LIKE`、`CASE` mux、`substring`、date year、
  常量数组 `IN`。`BETWEEN`/`IN` 展开为 comparison tree。
- Numeric 用带 scale 的 `int64`，AVG/division 保留动态 scale；overflow 和除零必须
  显式失败，不能 wrap。乘法结果仅在尾随十进制零可被无损除去时降低 scale，再判断
  `int64` overflow；这保证 Q11 的 `SUM(...) * 0.0001000000` 不因声明尾零误报溢出。
- 只允许 frozen/all-visible snapshot raw heap scan。TOAST external/compressed varlena、
  未支持 Numeric 编码、collation、任意 PostgreSQL function 必须拒绝 lowering，不能
  返回部分结果或静默退回 HOST。

## 7. FPGA 风格算子选择与不适合硬件化的部分

- Scan 参考 Trinity：并行解析 PostgreSQL 8 KiB heap page、line pointer、tuple header、
  NULL bitmap、datum。实现 page-level、tuple-lane 和 independent-plan-task 三层并行。
- Filter/Project 采用 fixed-lane streaming bytecode，valid lane compaction，适合 II=1
  pipeline。
- Join 参考 Xilinx Vitis Database GQE：multi-key build/probe hash，支持 INNER、LEFT、
  SEMI、ANTI 和 residual；内存不足时采用 Grace/radix partition + Flash replay。
- Aggregate 参考 Vitis GQE：banked multi-key hash state，支持 SUM/COUNT/MIN/MAX/AVG
  及 grouped DISTINCT；完整 group key 做 radix partition。
- Sort 采用 NASCENT2 更适合本项目的 near-storage external-sort 思路：有界 run
  generation、Flash run、stable merge pass、top-N 截断。相比完全片上 Vitis Sort，
  它更自然地处理 SF=16 这类超过 4 GiB FDM 的数据集。
- 一般非等值/相关 Nested Loop、任意函数/UDF、复杂 locale collation、TOAST 解压、
  MVCC visibility 判断、索引 AM 遍历和事务控制不适合伪装成 FPGA primitive。
  `NL_JOIN` 仅作功能 fallback 并计入 `software_fallback_nodes`。规划器应优先改写为
  hash semi/anti、scalar aggregate + keyed join，或拒绝 CSD path。

## 8. 并行、FDM 和 spill-to-flash

`parallel_workers` 表示模拟的 FPGA replicated resources，最大 32。软件线程只是功能
模型：

- Page level：Scan 把不同 page 组成 decode tasks。
- Tuple level：一个 page 按 1024-bit binding word 与 attribute 数推导 tuple lanes，task
  单位为 `(page, tuple-lane)`。
- Task level：DAG 中同时 ready 的独立分支（例如 Join 下两侧 Scan）并行执行；共享
  query worker budget，禁止嵌套线程池无限超配。

阻塞算子内存机制：

- Sort：`Scan -> [Filter/Project] -> Sort` 单消费者链由 FIFO batch 直接驱动，不再先
  累积完整 Scan `Batch`；current run 逐行增长 arena lease，无法增长时排序并写 Flash，
  Flash 上的 `SortRun` 只保留 handle 和该 run 的最大行宽。merge 根据可用 spill staging
  与实际最大行宽自适应选择 3--128 路 fan-in：所有 cursor 的四倍 chunk reserve 合计不
  超过一半 staging，bounded writer 使用另一半；连单行都无法安全容纳时回退到原有两路
  路径。优先队列以原始 run 次序作相等 key 的稳定 tie-break，中间层直接写下一层 record，
  最后一层 k-way merge 直出到已通过容量准入的结果边界，不恢复完整 run 或构造 merged
  `Batch`。PostgreSQL 的有限 SQL LIMIT 会以 `LIMIT + OFFSET` 设置 Top-N，并在每个
  run/merge 后截断，后继 Limit 再跳过 OFFSET。OFFSET-only/无界排序保持 `top_n=0`。
  无法估算或超出当前 wire 整数范围的 LIMIT/OFFSET 不得用 planner estimate 冒充 SQL
  语义：AUTO 保留 HOST，forced CSD 显式拒绝。
- Hash Join：完整 canonical key 做 power-of-two radix partition；build/probe selection
  直接从 source row ordinal 两遍序列化到 Flash，不创建 partition `Batch`。一次只
  restore 一个 build partition 并申请 arena lease；probe record 按 staging capacity
  解码为 row-complete chunks。hidden probe ordinal 恢复原顺序。若 sole left
  `Scan -> [Filter/Project]` probe 链的 right/build 已完成且实际 batch bytes 可驻留，
  则 build hash 只构造并 lease 一次，left FIFO batch 直接逐批 probe。build 超限时
  left 不再累计：每个 FIFO batch 直接 selection-to-Flash，追加跨 batch 全局 ordinal 后
  逐 partition replay 并恢复顺序。若两侧均为 sole `Scan -> [Filter/Project]` 链且规划器
  通过 Join `arg0` 下发的保守 build 容量超出 arena，right/build 也逐 FIFO batch 直接
  写 Flash；保守值选择初始 fan-out，实际 partition bytes 超限时倍增 radix fan-out 后
  再 probe。每条 streamed data record 后附加 40 B Flash 反向链节点，controller 内每个
  partition 只保留 16 B `{tail,count}`，常驻句柄不再随 batch selection 数量增长。其他
  build 子树仍用完整 build 做精确分类。partition replay 的 tagged output 直接形成
  sorted Flash run；binary-carry merge 每层最多保留一个 handle，最终 run 分块读回并去掉
  hidden ordinal，不再保留 output-sized `OrderedJoinRow` vector。replay hash probe 逐匹配
  行发射到 spill-staging-bounded ordinal buffer，不再先构造完整 `partition_output`；每个
  Flash 反向链 handle 结束时强制 flush，以保证每个 leaf run 的 ordinal 单调性。若最终
  run 的唯一后继是任意 Filter/Project 链加 non-DISTINCT Aggregate，则按 staging-bounded
  chunk 直接更新聚合状态，不再恢复完整 visible Join `Batch`；聚合 state 实际超出 arena
  时释放 partial state、重读仍完整的 final run，并把有界 chunk 按完整 group key 直接写入
  Flash handle chain。之后每次只 replay 一个 partition，仍超限时使用更高 hash bits 递归
  repartition，不再回到完整 Join 输出物化路径。query-local Flash 使用可合并 free extent
  重用已经消费的 data/link/run 空间；超大 probe 在达到 spill 容量四分之一后分 wave
  replay，保留 build partition 并把带全局 ordinal 的结果并入同一 run merge，避免必须
  先容纳完整 probe。partitioned Join 还可按 staging-bounded chunk 直连后续 Join 的
  probe 或 build 侧，不再为 Join-to-Join 强制恢复完整可见 `Batch`。
- Grouped Aggregate：每个 group 永远落入一个 partition，保留 DISTINCT 与 AVG
  sum/count 语义；collision partition 使用更高 hash bits 递归 partition。新 group 和
  DISTINCT item 在插入前原子计费，一旦过 arena threshold 就丢弃 partial table 并按
  已处理比例估计 fan-out。
- Aggregate 流式快路径：直接 `Scan -> [Filter/Project] -> Aggregate` 的无 DISTINCT
  global state 固定，始终逐 FIFO batch 更新；grouped state 仅当 PostgreSQL 通过
  `arg0` 下发的保守输出容量再乘 8 仍可放入 arena 时启用。超限的 spillable grouped
  Scan 不再累计完整输入，而是逐 FIFO batch 写 Flash radix chain，再逐 partition replay。
  大型 Scan 后接 HAVING 时还利用 heap 页容量防止严重低估的 group 数进入直接驻留路径；
  sole Filter/Project 后继以 4,096 行 tile 消费聚合结果。其他输入形状及 DISTINCT
  仍可能走兼容物化路径，runtime arena accounting 是最终保护。
- 单行/单 group/极端 skew 无法落入 arena 时显式拒绝。并行 branch 暂占 arena 时，
  replay 等待 lease，而不是误报 OOM。

必须诚实保留的当前边界：直接 Scan-to-Sort、符合准入条件的 Scan-to-Aggregate，以及
Join 左/probe 链已消除对应完整 Scan `Batch`；符合准入的超大 Join build 即使 probe
侧是非 Scan 子树，也直接逐 batch 分区。其他输入形状的超大/不确定 Aggregate、直接
Scan 输入的 DISTINCT Aggregate 以及最终阻塞输出仍可能累计。cursor chunk、hash state 仍是普通
STL container，而非真实 FDM
arena allocator；streamed Join 的句柄 payload 已由 Flash 链约束为 O(partition count)，
但 descriptor vector 仍有 STL allocator 开销。partitioned Join 的 ordinal reorder
payload 已约束为 O(log run count)，probe fan-out 暂存已由 spill staging 容量约束，
但最终 visible output
仅在最终返回、DISTINCT/不支持 Aggregate 或容量回退时仍可能完整物化；符合
sole-consumer non-DISTINCT Aggregate 形状的 final run 已按 chunk 直连，runtime state
超限也已由直接 Aggregate partition writer + recursive replay 约束。
虽然 scan window、FIFO、run generation、cursor merge、部分 Aggregate/Join state、spill
I/O 和 blocking lease 已有界，但在实现 final-result consumer、其他 Aggregate 输入形状
的 streaming partition writer 与 arena-backed container 前，不能声称 SF=16 端到端
物理内存已严格有界。旧版 SF=16 Q17/Q18/Q20 的普通 STL `Batch` 保守峰值分别为
22,194,284,144 / 15,550,897,984 / 7,852,313,760 B；新路径在三条定向 exact
测试以及新版 22-query 整套 exact 回归中均分别降为 714,716,656 /
571,052,512 / 2,844,175,384 B。整套最大保守 `Batch` 峰值转为 Q22 的
3,230,706,240 B；但这些统计不等于严格的物理内存上界。global Aggregate 的大型
DISTINCT 仍保留 compatibility/protection-copy 路径。

## 9. 时延模型和已测性能

每个 node 当前计 `64-cycle launch + rows * arg1`，`arg1` 是 II，默认 1；总计写入
`modeled_cycles`。CSD kernel 通过追加字段的 `cemu_args` 同时返回总 Flash 读、总 Flash
写、spill replay 读和逻辑 spill 写字节；基础表读由总读减去 spill 读得到。
当 PostgreSQL 的 `csd.program_runtime=0` 时，CEMU 自动使用下式决定解冻 guest 后
Program Execute 仍保持 outstanding 的设备时延；正数 `program_runtime` 仍可显式覆盖：

```text
T_stream = max(modeled_cycles / fpga_clock,
               base_read_bytes / flash_read_bandwidth)
T_device = T_stream
         + spill_read_bytes / flash_read_bandwidth
         + flash_write_bytes / flash_write_bandwidth
```

基础扫描可与FPGA流水重叠；依赖有序的spill写和replay读不再被一个全局`max`隐藏。模型
在完成逐算子重叠校准前有意采用保守累加。当前 `config_example.json` 默认
`fpga_clock_mhz=250`、`flash_read_mb_s=1000`、`flash_write_mb_s=1000`。功能性C++ kernel
仍在TCG VM-Freeze中运行，其host wall time不计入 `T_device`。论文
校准目标公式：

```text
T = T_launch + ceil(rows / lanes) * II / f_clk
    + bytes_in / BW_in + bytes_out / BW_out + T_spill
```

FPGA frequency、lanes、II、SRAM/FDM budget、Flash bandwidth、command launch latency
必须和每组实验一同记录。不得把 C++ wall time 当作真实 FPGA latency；现阶段 wall
time 是“CEMU functional implementation + TCG/VM-Freeze”的系统结果。

历史 SF=1 forced-CSD cold-start 原型基线位于
`results/tpch_sf1_csd_performance.*`：

- CSD/HOST 每次样本前重启 PostgreSQL 并 drop guest page cache；外部 host wall clock
  计时，因此包含 TCG/VM-Freeze 功能 C++ 开销。它只能作为原型系统 wall-time 记录，
  不能用作 FPGA/Flash 模型时延或与第 12 节 `guest_ms` 混算。
- Q1-Q22 全部 byte-for-byte exact，22/22 都快于 HOST。
- CSD 总时间 210,090 ms；HOST 总时间 951,977 ms。
- 汇总吞吐 speedup 4.531x；per-query geometric mean 4.249x。
- 最小 1.476x（Q22），最大 15.880x（Q1）。

论文中的 AUTO/FPGA device-time 性能应优先使用第 12 节双时钟实验；旧 forced-CSD
wall-time 数字保留只为历史可追溯。

关键低内存实验：

| 场景 | 限额与结果 |
| --- | --- |
| Sort replay | 8 MiB arena；23,984,748 B 写/读；3 records；结果 exact |
| Join replay | 1 MiB arena + 1 MiB staging；241,548,724 B 写/读；peak 750,240 B；结果 299808 exact |
| Aggregate replay | 1 MiB arena；5,996,736 B 写/读；16 records；peak 796,876 B；74,999 groups exact |
| FDMFS reuse | fragmented free 后重新申请完整 4,294,967,296 B 成功 |
| Q8 normal | 3.594 GiB arena；peak 572.320 MiB；0 spill |
| Scan-to-Sort stream | 32 MiB FIFO；1 MiB arena；30,209 rows exact；peak 1,048,560 B；2,416,828 B spill write/read |
| Cursor Sort merge | 1 MiB arena/staging；30,209 varlena rows exact；3 runs/2 merges；cursor peak 757,278 B；writer peak 1,048,566 B |
| Scan-to-Aggregate stream | Q1 5,884,033 rows / 29 batches / 2,528 B query peak；Q6 119,810 rows / 29 batches / 134 B；均 0 spill |
| Scan-to-Join stream | unit: 80 B build 在 128 B arena 下 3 probe batches、0 spill；Q3 29,949 probe rows/58,973,200 B build；Q5 两个 stream Join、10,001 probe rows；均 0 spill |
| Join probe-to-Flash | Q3 32 MiB arena；1 个 partitioned Join；30 probe batches / 3,231,383 rows；peak 29,494,560 B；60,171,304 B spill write/read；结果 exact |
| Join build-to-Flash | Q3 32 MiB arena；7 build batches / 737,165 rows + 30 probe batches；peak 8,832,780 B；60,180,376 B spill write/read；结果 exact |
| Join Flash handle chain | Q3 256 data/chain records；resident descriptor peak 1,024 B；40 B/record metadata；60,190,616 B spill write/read；结果 exact |
| Join ordinal Flash merge | Q3 32 leaf runs / 31 merges；736,065 merge rows；resident handles 150 B；166,186,244 B spill write/read；结果 exact |
| Join fan-out stream | Q3 147,213 partitioned output rows / 32 buffers；pending peak 598,680 B；Flash I/O 与 ordinal-merge 版相同；结果 exact |
| Join-to-Aggregate handoff | 32 MiB Q12/Q14/Q19 分别直连 31,235/77,062/127 行；最大 replay chunk 3,510,755/10,835,462/24,738 B；0 fallback；Flash I/O 不变；结果 exact |

详细报告分别位于 `results/tpch_sf1_{sort,join,aggregate}_spill_replay_validation.md`、
`results/tpch_sf1_fdm_arena_validation.md` 和 `results/tpch_sf1_fdm_fifo_validation.md`。
新增时延校准与完整回归位于 `results/tpch_sf1_spill_timing_validation.md`：1 MiB Join
replay 的模型增量为 460.718 ms，两轮 guest executor 实测均值增量为 454.376 ms
（差异 1.38%），修复 Sort 输出 schema boundary 后 forced-CSD Q1-Q22 为 22/22 exact。
直接 Scan-to-Sort 的批次消费验证位于
`results/tpch_sf1_streaming_sort_validation.md`；版本化 kernel 下 forced-CSD Q1-Q22
再次 22/22 exact，forced planner 22/22 与 AUTO 定向回归同时通过。
cursor-based merge 验证位于 `results/tpch_sf1_cursor_merge_validation.md`；整数与
varlena 两组低内存 Flash spill 均 exact，最终 kernel 的 forced-CSD Q1-Q22、planner
22/22 与 AUTO 定向回归全部通过。
Sort 最后一轮采用旧 AOE 的 merge-and-stream 思想，不再写出后立刻读回最终 Flash run；
验证位于 `results/tpch_sf1_sort_final_stream_validation.md`。1 MiB arena/staging 的
30,209 行 varlena 查询 exact，spill 总读写由 14,646,232 B 降至 9,420,282 B（-35.68%），
forced-CSD 22/22 exact、planner 22/22、AUTO 定向回归与 optimized/ASan/UBSan/guest
单元测试均通过。
SQL LIMIT/OFFSET 到 external Sort Top-N 的端到端 lowering 验证位于
`results/tpch_sf1_sort_topn_validation.md`。同一 1 MiB arena/staging、30,209 行 varlena
查询在 `LIMIT 100` 下仅保留 100 行，spill 总读写由 9,420,282 B 降至 69,190 B
（-99.2655%，136.15x）；`LIMIT 100 OFFSET 50` 保留 150 行后正确返回 100 行，
OFFSET-only 的 30,159 行与 `LIMIT 0` 也均与 HOST exact。最终 forced-CSD 22/22 exact、
planner 22/22、AUTO 定向回归与 optimized/ASan/UBSan/guest 单元测试均通过；原始证据
归档 SHA-256 为
`e4cdd50e20861205b43eda547342f676981aa8811d536c246559c05448ca77df`。
adaptive k-way external Sort 验证位于
`results/tpch_sf1_sort_kway_validation.md`。同一 30,209 行 varlena 无界排序的 spill
总读写由两路归并的 9,420,282 B 降至 5,226,094 B（-44.5230%）；6,001,215 行
`ORDER BY ... LIMIT 100` 形成 229 个初始 run，多层 128-way merge 将总读写由
3,680,832 B 降至 1,183,632 B（-67.8434%），最大 staging reservation 恰为 1 MiB。
最终 forced-CSD 22/22 exact、planner 22/22、AUTO 定向回归以及
optimized/ASan/UBSan/guest 单元测试均通过；原始证据归档 SHA-256 为
`12e13208a9a947b90989fb8006f780b6ec93d24e6df96ae294a399c83c6ddfed`。
Scan-to-Aggregate 流式驻留验证位于
`results/tpch_sf1_streaming_aggregate_validation.md`；最终 v3 kernel 的 Q1/Q6 profile、
forced-CSD 22/22 exact、planner 22/22、AUTO 定向回归与 ASan/UBSan 均通过。
Join-to-Aggregate 的 runtime 聚合状态超限不再恢复完整 Join 输出，而是直接写入 Flash
radix chain 并递归 replay；验证位于
`results/tpch_sf1_stream_aggregate_partition_validation.md`。192 B arena 的定向测试触发
fallback/repartition 并得到 3 个 exact group；1 MiB Q12 证明正常融合路径的 31,235 行由
3 个有界 batch 消费，最终 forced-CSD 22/22 exact、planner 22/22、AUTO 定向回归以及
optimized/ASan/UBSan/guest 单元测试均通过。
one-sided Scan-to-Join 验证位于
`results/tpch_sf1_streaming_join_validation.md`；最终 v2 kernel 的 Q3/Q5 profile、
build 超限回退、forced-CSD 22/22 exact、planner 22/22、AUTO 定向回归与 ASan/UBSan
均通过，原始证据归档 SHA-256 为
`a8b22c61121bce30ee7a07c88e84ef73ef9469b36aac75dbeedc81e077e02619`。
oversized-build Join 的 probe-to-Flash 验证位于
`results/tpch_sf1_partition_probe_validation.md`；最终 v2 kernel 在 Q3 32 MiB arena 下
逐批分区并 exact，默认容量 forced-CSD 22/22、planner 22/22、AUTO 定向回归与
ASan/UBSan 均通过，原始证据归档 SHA-256 为
`460c833204a395dc15ff2a965d2d017331f3559f7b0222472eaeb04759739485`。
two-sided Scan Join 的 build-to-Flash 验证位于
`results/tpch_sf1_streaming_build_validation.md`；最终 v2 kernel 的 Q3 32 MiB profile、
forced-CSD 22/22 exact、planner 22/22、AUTO 定向回归与 ASan/UBSan 均通过，原始证据
归档 SHA-256 为
`32d310595fd6095d077704f01c767198d4c96debac008eb4209cf67ca7608bbd`。
streamed Join 的 bounded Flash handle-chain 验证位于
`results/tpch_sf1_spill_chain_validation.md`；最终 v1 kernel 的 Q3 常驻链描述符峰值为
1,024 B，forced-CSD 22/22 exact、planner 22/22、AUTO 定向回归与 ASan/UBSan 均通过，
原始证据归档 SHA-256 为
`75646d7446fd7712a724a8e56121629767bd4077f2006a257896b4405f92c271`。
partitioned Join 的 bounded ordinal merge 验证位于
`results/tpch_sf1_ordinal_merge_validation.md`；最终 v1 kernel 的 Q3 常驻 run handle
payload 为 150 B，新增 Flash 模型时延与实测差异 1.92%，forced-CSD 22/22 exact、
planner 22/22、AUTO 定向回归与 ASan/UBSan 均通过，原始证据归档 SHA-256 为
`881662128094edca4e98f190eac2d32e2957997e9f6b3e416bf227a39de7f978`。
partitioned Join 的 fan-out-bounded row emission 验证位于
`results/tpch_sf1_fanout_stream_validation.md`；最终 v1 kernel 的 Q3 pending output
峰值为 598,680 B，Flash I/O 未增加，forced-CSD 22/22 exact、planner 22/22、AUTO
定向回归与 ASan/UBSan 均通过，原始证据归档 SHA-256 为
`750d61f6231bf67a4e2c675958b706b8e2646bb0be4c03d06d8de3cd9780e772`。
partitioned Join 最终 run 到 Aggregate 的 bounded handoff 验证位于
`results/tpch_sf1_join_aggregate_handoff_validation.md`；Q12/Q14/Q19 在 32 MiB arena
下均命中一次、零 fallback，Flash 读写与前一版本逐字节相同；清洁恢复后的 forced-CSD
22/22 exact、planner 22/22、AUTO 定向回归与 ASan/UBSan 均通过。完整回归、新路径和
旧 kernel 基线三份归档 SHA-256 分别为
`a294c7517f8ee080aba913126ed3ec572f850b22f2b0b4be8a8321710e004672`、
`5beda81163615312135ca69e936d8f92234939f21152ca7ec4a940b1fb865082`、
`1f773d85941895d002864dd1e272c84d5f8b3e1563d438bac8d152d883d8c87e`。

SF=16 可扩展性与正确性验证位于 `results/tpch_sf16_validation.{md,csv}`，原始证据归档
为 `results/tpch_sf16_validation_raw.tar.gz`，SHA-256
`4ae3b446c59d37268f9a35ccf25dc58f70b7fe031e60705e49b7d541f3adb42d`。原版
pure-memory backend、48 GiB NVM、4 GiB FDM、13,312 MiB query-local spill 下，forced
CSD Q1--Q22 全部与 HOST byte-for-byte exact，forced planner 22/22、pending expression
为 0，optimized 与 ASan/UBSan 单元测试通过。25 个 CSD 子计划均为零 spill allocation
failure；最大 spill high-water 是 Q21 的 9,137,763,384 B，Q9 probe-wave replay 为
7,830,014,467 B。该实验只证明超过 4 GiB FDM 的数据与中间结果可正确执行，不能用作
HOST/CSD speedup 数据，也不能掩盖上一节记录的 Q17/Q18 STL 物化峰值。

2026-10-14 bounded SF=16 复测使用版本化 kernel
`csd_sql_sf16_bounded_v4.so`（SHA-256
`888aa13914807f48d59d62d4f74cc1e9b85207314668542d92724571b33e1e24`）及
同一 48 GiB NVM、4 GiB FDM、13,312 MiB spill 配置。原始结果位于
`results/tpch_sf16_bounded_full_v4_final/`：forced-CSD Q1--Q22 全部 byte-exact，
全部 profile 的 `spill_failed_required_bytes=0`。最大保守 `Batch` 峰值为 Q22 的
3,230,706,240 B；最大“非 operator FDM 预留 + operator peak + Batch peak”的
逐计划分离峰值和为 3,705,643,948 B；最大 spill high-water 仍是 Q21 的
9,137,763,384 B。新 CSD guest modeled-path 总时间 863,532.928 ms，旧版
850,480.919 ms；增加的 Flash partition/replay 使总模型时间上升约 1.53%，换来
Q17/Q18/Q20 的大幅物化下降。该峰值和遗漏其他普通 STL 分配及瞬时副本，绝不
证明 4 GiB 物理内存硬上界。性能对照必须使用冷缓存原生 HOST 基线并标注为
跨 native HOST 与参数化 CEMU device model 的预测比值；同 guest TCG HOST 时间
不能当作真实主机基线。

对应的 SF=16 冷缓存对照和方法见
`results/tpch_sf16_bounded_cold_validation.md`、
`results/tpch_sf16_bounded_vs_native_cold.csv`。原生 HOST 独立轮按每条查询 restart
PostgreSQL、定向逐出本 cluster `base/` 页缓存，逐出后最高驻留比例为 0.000778%，
`autovacuum=off`，Q1--Q22 也全部与 golden exact。HOST 总计 1,039,543.923 ms，
CSD modeled-path 总计 863,532.928 ms，跨平台预测总耗时比值 1.2038x、逐查询
geomean 1.1372x，仅 12/22 单查询 CSD 较快；离线逐条取最小值的上限 1.5513x
不是 `AUTO` 的实测结果。原始 HOST 记录在
`results/tpch_sf16_native_host_cold_isolated_v2/`，之前并行探索轮在
`results/tpch_sf16_native_host_cold_v1/`。所有本阶段原始目录与 paired guest Q6
诊断归档为 `results/tpch_sf16_bounded_cold_raw.tar.gz`，SHA-256 为
`dbf484d1055fc6b77b951ef6c642b8c077fb1ccbf97fea408695b9ddb50c69cb`；
解压到 `results/` 即还原上述路径。CSD 完整轮曾与探索性 HOST 轮部分并行，
但 Q17/Q18/Q20 单独 CSD 测量与完整轮 guest 时间相差均小于 1%；论文应继续标注
TCG guest 控制开销、参数化 Flash/FPGA 时延以及物理内存上界未证明这三项限制。

## 10. 回归命令与通过标准

主机单元测试：

```bash
g++ -Wall -Wextra -O2 -march=native -std=c++11 -pthread \
  -o /tmp/csd_sql_test \
  code/test-cemu/kernel/csd_sql_test.cc code/test-cemu/kernel/csd_sql.cc
/tmp/csd_sql_test

g++ -Wall -Wextra -O1 -g -std=c++11 -pthread \
  -fsanitize=address,undefined -fno-omit-frame-pointer \
  -o /tmp/csd_sql_san \
  code/test-cemu/kernel/csd_sql_test.cc code/test-cemu/kernel/csd_sql.cc
ASAN_OPTIONS=detect_leaks=0 /tmp/csd_sql_san
```

完整 guest 结果回归：

```bash
cd /mnt/cemu
./tests/test_csd_tpch_execution.sh --sf 1 \
  --mode auto \
  --socket-dir /tmp/csdpg_nvme_tpch_s1 --port 57432 \
  --shared-library /workspace/code/CEMU/tests/cemu/build/<version>.so \
  --output-dir /tmp/<run-name>
```

SF=16 最终验证使用预生成 HOST golden，避免在 16 GiB guest 中为每条查询重复执行 HOST：

```bash
tar -xzf /mnt/cemu/results/tpch_sf16_validation_raw.tar.gz -C /mnt/cemu
./tests/test_csd_tpch_execution.sh --sf 16 --mode csd \
  --socket-dir /tmp/csdpg_nvme_tpch_s16 --port 57432 \
  --shared-library /workspace/code/CEMU/tests/cemu/build/<version>.so \
  --host-golden-dir /mnt/cemu/results/tpch_sf16_host_golden \
  --output-dir /tmp/<run-name> --profile --cache-fill off \
  --spill-mb 13312 --execute-timeout-ms 3600000
```

通过标准是 Q1-Q22 全部报告 `EXACT`，不能只检查成功退出。规划器回归同时要求 forced
CSD plan 中没有 host scan/join/aggregate/sort，允许顶层 PostgreSQL `Limit` 作为控制
编排。性能回归需固定 cache 状态、runtime、parallel workers、FDM/spill 参数，保留
原始 result/status，不可只保存汇总图。

AUTO 定向回归：

```bash
PG_CONFIG=/opt/pgsql12/bin/pg_config \
  CSD_AUTO_SKIP_BUILD=1 \
  CSD_AUTO_SOCKET_DIR=/tmp/csdpg_nvme_tpch_s1 \
  CSD_AUTO_PORT=57432 \
  ./tests/test_csd_auto_planner.sh
```

该脚本的 `hot=HOST` 是 SF=1 校准断言，不应直接改用 SF=16：在 SF=16 即使 base table
全部命中主机缓存，`count(lineitem)` 的 FPGA 聚合收益仍可能使 AUTO 正确选择 CSD。

## 11. AER 与缓存回填（2026-10-04 已实现）

前置事实：FEMU 原代码虽声明 `aer_reqs`、`aer_queue` 和 command-effect support，管理
命令 switch 实际没有 `ASYNC_EVENT_REQUEST`，Linux 发出的 AER 会收到 invalid opcode。

已实现的端到端闭环：

1. FEMU 支持真正 pending 的 AER：admin SQ 消费命令但不立即 CQ completion；事件到达
   后由 QEMU bottom half 在 admin CQ 有空间时完成。controller reset/exit 回收 pending
   request 和 queued event。
2. vendor log page `0xC0` 保存 64-entry bounded chunk ring。entry 包含 sequence、plan
   generation、nsid、physical offset、relation OID、relation logical offset、length 和
   flags。相同 generation/relation 的相邻 logical chunk 会合并；ring 满时覆盖最旧
   entry 并累计 dropped，符合当前 best-effort、弱一致性目标。
3. `cemu_args`/`ubpf_jit_args` 增加 `cache_notify` callback。chunked Scan 每次成功读取
   physical extent 后发通知；通知失败是 opportunistic cache miss，不能使 SQL 失败。
4. Linux NVMe host driver 即使 FEMU 未声明标准 OAES，也会为 CEMU quirk 保持一个
   outstanding AER。收到 vendor event 后在 `nvme_wq` 读取 log page 并调度 cache
   worker；AER 本身不携带或 DMA table data。
5. PostgreSQL 在 Program Execute 前调用 `IOCTL_CEMU_CACHE_BEGIN`，再按 plan relation
   去重并注册每个 main-fork segment fd、OID、generation、relation logical base 和
   length。kernel 持有 file reference，用户态关闭 fd 不影响 worker。
6. cache worker 用 relation OID + logical range 找到正确 heap inode segment。Linux
   会把单个 `POSIX_FADV_WILLNEED` 限制为 backing-device readahead window（本机默认
   128 KiB），因此 worker 以该窗口提交预读，但先提交一个 32 MiB pipeline group 再
   等待。它用 folio Uptodate 状态确认真实 I/O 完成，不再把每个字节 `kernel_read` 到
   临时缓冲区；仅在内存压力漏页时以 `read_mapping_page` 补页。仅按 physical LBA 读
   block device 不等价于填 heap inode page cache。
7. 新 ioctl：`CACHE_BEGIN(0x07)`、`CACHE_REGISTER(0x08)`、`CACHE_STATS(0x09)`、
   `CACHE_WAIT(0x0a)`。统计包含 active/completed generation、sequence、events、
   notified/prefetched/unmapped bytes、requests、controller dropped、errors，以及
   execute start/complete、first/last prefetch 和 overlap-prefetched bytes 时间线。
8. PostgreSQL GUC：`csd.cache_fill` 默认 `on`；`csd.cache_wait_ms` 默认 `0`，即查询
   不等待异步 worker。实验可设非零值作为完成屏障；回填失败只产生 warning/cache
   miss，不允许让本来正确的 CSD SQL 失败。`csd.cache_overlap` 默认 `on`；关闭时
   driver 保留 AER 事件但延迟到 Program Execute 完成后才启动 worker，作为 A/B
   基线。`csd.profile=on` 输出每代统计。
9. CSD kernel 在最后一个 scan 通知后发送显式 FINAL event；只有 cache worker 按序
   处理完此前的数据 event 后才推进 `completed_generation`，因此 `CACHE_WAIT` 不会
   把“最后一个通知已发出”误当成“全部文件页已进入 host page cache”。

计算/回填重叠验证位于 `results/tpch_sf1_cache_compute_overlap.md`：默认自动模型在
SF=1 `count(lineitem)` 上产生约 1.218 s outstanding window。最终 no-copy/pipelined
版本关闭重叠时窗口内完成 0 B，开启时完成 134,217,728 B（128 MiB）；完整回填的
首末完成区间为 10.553 s、115.37 MiB/s，旧 buffered-read warm 版本为 22.973 s、
53.00 MiB/s。6,001,215 行结果、总回填 1,276,665,856 B、零 error 均保持一致。

已验证的 SF=1 缓存收益位于 `results/tpch_sf1_aer_cache_validation.{md,csv}`：

- 三轮都执行“restart PostgreSQL + drop guest page cache + forced CSD count(lineitem) +
  forced HOST count(lineitem)”。关闭回填 HOST 均值 22,537.964 ms；AER 完整回填后
  10,818.313 ms，speedup 2.0833x，平均延迟降低 52.00%。
- no-copy/pipelined 版本另做一轮同方法复测：关闭回填后 HOST 为 23,492.340 ms，
  回填后为 10,474.203 ms，speedup 2.2429x、延迟降低 55.41%；原始结果位于
  `results/tpch_sf1_aer_cache_pipeline_raw/`。
- 每轮结果均为 6,001,215；AER 合并为 1 个 event，通知/完成 1,276,665,856 B，跨
  两个 heap segment 形成 2 个 cache-fill 请求，unmapped/drop/error 均为 0。
- 默认异步回填下重新执行 TPC-H Q1-Q22，22/22 与 HOST byte-for-byte exact；原始
  输出归档在 `results/tpch_sf1_aer_exact_final.tar.gz`。

必须诚实保留的限制：

- 功能性 C++ kernel 仍在 VM-Freeze 内执行；真正观测到的重叠发生在功能模型返回、
  guest 解冻，但 Program Execute 仍由 CEMU scheduler 按 FPGA/Flash 模型保持 pending
  的设备执行窗口。不能把冻结期间的 C++ host wall time当作 FPGA 计算时间。
- TCG guest 的 no-copy/pipelined worker 完整回填 1.27 GB 的首末完成区间仍约 10.55 s，
  显著慢于当前 1.218 s 模型窗口；因此本轮证明 128 MiB 实际重叠，但没有证明同一条
  CSD 查询 wall time 显著降低。2.0833x（三轮旧基线）与 2.2429x（最终版本一轮复测）
  都是完整回填后下一条 HOST scan 的收益。该 115.37 MiB/s 是 TCG/Linux 原型路径，
  不能写成真实 NVMe SSD 的介质带宽。
- 连续 22-query 回归结束时 controller 全局累计 dropped=30，说明 64-entry ring 在
  某些复杂查询的一次 freeze 窗口中会覆盖事件。它不影响 SQL 正确性，只损失部分
  cache opportunity；论文应报告该值，后续可扩大 log 或做跨 ring 的 range 合并。
- generation 只处理计划轮次陈旧事件，不覆盖生产级 truncate/rewrite/drop 并发一致性。
- 本阶段选择 host filesystem page cache，没有占用 4 GiB FDM 的 operator arena；若
  后续实现持久 FDM chunk cache，必须支持逐出并与 scan/FIFO/spill staging 共同计费。

## 12. AUTO 全候选动态规划（2026-10-09 已实现）

AUTO 不再依赖 PostgreSQL pathlist 恰好保留 CSD 中间路径。PostgreSQL 正常维护 HOST
paths，扩展私有 registry 则为每个 `RelOptInfo` 保存 CSD 候选 Pareto frontier，而不是
只保存 device cost 最小的一条。Join 枚举左右 frontier 的笛卡尔积，upper operator
枚举全部输入候选。只有 pathkeys、未决 SubPlan、selectivity risk、scan count 和 expected
rows 相同的状态才允许比较；device cost、resource-bound rows、resource-bound bytes 均
不大的候选才能支配另一候选。这避免了 naive 全枚举在 Q5 上的候选爆炸，同时不会按
单一局部 cost 过早剪枝。CSD parent 只接受 CSD child；HOST parent 可接受 HOST 或已物化
CSD boundary。因此合法状态转移只有 `CSD->CSD`、`CSD->HOST`、`HOST->HOST`，一旦
进入 HOST，所有祖先必须留在 HOST。

内部 device cost 不重复收取结果回传：

```text
scan = pages * flash_page_cost + input_tuples * tuple_cost
join = child costs + input_tuples * join_tuple_cost + spill
agg  = child cost + input_tuples * aggregate_tuple_cost + spill
sort = child cost + n*log2(n) * sort_compare_cost + spill
boundary = setup_cost + device cost + output_bytes * result_byte_cost
```

默认 `setup_cost=1000`、`result_byte_cost=0.001`、
`host_cached_page_cost=0.33`、`spill_page_cost=0.10`。FDM operator budget 从 4 GiB
减去 output、spill staging、两个 FIFO 和每个 scan 的 chunk。正常时延 cost 使用
PostgreSQL/ANALYZE 的 expected rows；容量准入另用 resource bound：Scan 采用物理页可容纳
tuple 上界，Join 采用笛卡尔积或 preserved-side 结构上界，Sort 继承输入上界，LIMIT 截断，
Group/DISTINCT 使用 `min(input_bound, expected_rows * csd.stats_safety_factor)`，默认 factor=8。
结果容量包含 ABI header、每列 result cell 和有界 varlena payload。resource-bound 输出超过
`csd.output_mb` 时 boundary 加 `disable_cost`，但 device state 仍可继续被 CSD Aggregate
等缩减。Group/DISTINCT 的统计倍数是保守准入估计而非数学硬上界；runtime capacity/spill
检查仍是最终保护，低估必须显式失败，不能静默改走 HOST。

主机缓存收益只计算一次：`mincore` 的实际 resident fraction 降低 HOST SeqScan cost，
不再同时作为 penalty 重复加到 CSD。定向回归
`tests/test_csd_auto_planner.sh` 覆盖 cold=CSD、hot=HOST、合法 CSD->HOST cut、输出容量、
未决 SubPlan、SEMI/ANTI 基数边界，以及冷缓存下 Q2/Q16 必须选择纯 HOST。

两个 correctness guard：含未重写 `SubPlan` 的 device state 不允许直接物化给 HOST；
SEMI/ANTI 用 preserved-side rows 作保守边界估计，并把 risk 沿后续 CSD join 传播，直到
Aggregate、DISTINCT 或 LIMIT 清除。后者修复了 Q21 将实际 337,680 行误估成 1 行、
随后触发 HOST nested-loop explosion 的问题，AUTO Q21 从约 100.8 s 降到最终 20.0 s。

旧的 2026-10-05 单候选结果仍在 `results/tpch_sf1_auto_placement_validation.md` 和 CSV，
用于追溯，不代表当前 frontier 实现。它的 Q2/Q16 mixed miss 已由一般化容量准入修复，
没有在 planner 中按 query 编号硬编码。当前阶段的双时钟完整冷启动结果写入新的
`tpch_sf1_auto_global_search*` 文件；`guest_ms` 才是排除 VM-Freeze C++ wall time 后的
FPGA/Flash 模型指标，`wall_ms` 只诊断 TCG 原型开销，禁止把后者写成设备时延。
最终默认布局为 18 条完整 CSD、2 条 mixed（Q17/Q20）、2 条纯 HOST（Q2/Q16），22/22
byte-exact。每个样本独立 restart PostgreSQL 并 drop guest page cache 后，AUTO guest 总计
183,019.742 ms，HOST 825,114.403 ms，吞吐 speedup 4.5083x，per-query geometric mean
6.9207x，20/22 单条更快。Q2/Q16 的单次差异是同一纯 HOST 计划的冷 I/O/TCG 方差；独立
Q2 复测为 19,176.669/19,218.811 ms。Q20 是 1.024x 的剩余 near-tie。详细方法和数据见
`results/tpch_sf1_auto_global_search_validation.md` 与 CSV。

## 13. 常见陷阱

- 不要启用 KVM；它破坏本项目依赖的 TCG VM-Freeze 语义。
- 不要让 guest 执行主机编译的 PostgreSQL 或 CSD binary。
- 不要把 9P 放进测量路径，也不要每次查询复制整表到 FDM。
- 不要把 AER 当作 data DMA；AER -> log -> ordinary read/readahead 才是正确控制面。
- `NVME_IOCTL_IO_CMD` 需要 `CAP_SYS_ADMIN`；科研原型已经明确接受该权限。
- `fdm_reserved_bytes` 是总 query reservation，`operator_arena_bytes` 才是阻塞算子预算；
  spill Flash file 不属于 FDM。
- `flash_read_bytes` 包括 base relation 与 replay，`spill_read_bytes` 只包括 replay；分析时
  不要重复相加。
- Q15 会创建临时 view，测试脚本必须显式处理 read-only transaction 设置。
- Program Execute watchdog 超时不是普通 SQL 失败：当前 abort/reset 可能留下继续占用宿主
  CPU 的 VM-Freeze C++ worker。看到 `EINTR`/`ETIMEDOUT` 后不要直接继续测下一条；重启
  CEMU、重新 stage 数据并验证干净 Q1 profile。低内存压力实验可显式提高
  `csd.execute_timeout_ms`，但该值不得写入设备时延。
- 任何“支持全部 TPC-H”的结论都必须同时有 planner 22/22 和 result exact 22/22 证据。

