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
   PostgreSQL 动态规划比较两类路径。CSD cost 包含 scan/传输、算子、spill、命令
   启动和结果返回；HOST scan cost 由实际/估计缓存率降低。现有
   `csd.host_cache_fraction` 已进入 cost model，后续由 AER/cache 统计反馈替代纯手工值。

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
- 默认先用 SF=1 做功能和快速回归，再用 SF=32 验证内存边界和可扩展性。
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
- guest 源码：`/root/CEMU/code/CEMU/tests/cemu` 或 `/mnt/cemu`
- CEMU 可见 kernel：`/workspace/code/CEMU/tests/cemu/build/<version>.so`

## 5. 数据与构建流程

主机数据准备：

```bash
git submodule update --init code/postgres
./scripts/build_postgres.sh
./scripts/gen_data/create_tpch_db.sh --sf 1
./scripts/gen_data/create_tpch_db.sh --sf 32 --keep-running
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
- 表达式 IR 是有界 postfix VM，无 jump、递归、间接内存和 UDF。覆盖定点算术、
  comparison、SQL 三值逻辑、NULL、`LIKE`、`CASE` mux、`substring`、date year、
  常量数组 `IN`。`BETWEEN`/`IN` 展开为 comparison tree。
- Numeric 用带 scale 的 `int64`，AVG/division 保留动态 scale；overflow 和除零必须
  显式失败，不能 wrap。
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
  它更自然地处理 SF=32 和有限 FDM。
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

- Sort：申请 arena 失败后生成有界 sorted runs，Flash 上只保留 handle；两两读回、
  stable merge、写下一层，最终读结果。Top-N 在每个 run/merge 后截断。
- Hash Join：完整 canonical key 做 power-of-two radix partition；build/probe selection
  直接从 source row ordinal 两遍序列化到 Flash，不创建 partition `Batch`。一次只
  restore 一个 build partition 并申请 arena lease；probe record 按 staging capacity
  解码为 row-complete chunks。hidden probe ordinal 恢复原顺序。
- Grouped Aggregate：每个 group 永远落入一个 partition，保留 DISTINCT 与 AVG
  sum/count 语义；collision partition 使用更高 hash bits 递归 partition。新 group 和
  DISTINCT item 在插入前原子计费，一旦过 arena threshold 就丢弃 partial table 并按
  已处理比例估计 fan-out。
- 单行/单 group/极端 skew 无法落入 arena 时显式拒绝。并行 branch 暂占 arena 时，
  replay 等待 lease，而不是误报 OOM。

必须诚实保留的当前边界：完整 decoded Scan 输入仍会累计到 C++ `Batch`；hash state
仍是普通 STL container，而非真实 FDM arena allocator。虽然 scan window、FIFO、spill
I/O 和 blocking lease 已有界，但在实现 streaming Scan consumer 与 arena-backed
container 前，不能声称 SF=32 端到端物理内存已严格有界。global Aggregate 的大型
DISTINCT 仍保留 compatibility/protection-copy 路径。

## 9. 时延模型和已测性能

每个 node 当前计 `64-cycle launch + rows * arg1`，`arg1` 是 II，默认 1；总计写入
`modeled_cycles`。CEMU Program Execute `runtime` 可施加 query-level 模拟时延。论文
校准目标公式：

```text
T = T_launch + ceil(rows / lanes) * II / f_clk
    + bytes_in / BW_in + bytes_out / BW_out + T_spill
```

FPGA frequency、lanes、II、SRAM/FDM budget、Flash bandwidth、command launch latency
必须和每组实验一同记录。不得把 C++ wall time 当作真实 FPGA latency；现阶段 wall
time 是“CEMU functional implementation + TCG/VM-Freeze”的系统结果。

已验证 SF=1 cold-start 基线位于 `results/tpch_sf1_csd_performance.*`：

- CSD/HOST 每次样本前重启 PostgreSQL 并 drop guest page cache；外部 host wall clock
  计时，因为 VM-Freeze 会暂停 guest clock。
- Q1-Q22 全部 byte-for-byte exact，22/22 都快于 HOST。
- CSD 总时间 210,090 ms；HOST 总时间 951,977 ms。
- 汇总吞吐 speedup 4.531x；per-query geometric mean 4.249x。
- 最小 1.476x（Q22），最大 15.880x（Q1）。

关键低内存实验：

| 场景 | 限额与结果 |
| --- | --- |
| Sort replay | 8 MiB arena；23,984,748 B 写/读；3 records；结果 exact |
| Join replay | 1 MiB arena + 1 MiB staging；241,548,724 B 写/读；peak 750,240 B；结果 299808 exact |
| Aggregate replay | 1 MiB arena；5,996,736 B 写/读；16 records；peak 796,876 B；74,999 groups exact |
| FDMFS reuse | fragmented free 后重新申请完整 4,294,967,296 B 成功 |
| Q8 normal | 3.594 GiB arena；peak 572.320 MiB；0 spill |

详细报告分别位于 `results/tpch_sf1_{sort,join,aggregate}_spill_replay_validation.md`、
`results/tpch_sf1_fdm_arena_validation.md` 和 `results/tpch_sf1_fdm_fifo_validation.md`。

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
  --socket-dir /tmp/csdpg_nvme_tpch_s1 --port 57432 \
  --shared-library /workspace/code/CEMU/tests/cemu/build/<version>.so \
  --output-dir /tmp/<run-name>
```

通过标准是 Q1-Q22 全部报告 `EXACT`，不能只检查成功退出。规划器回归同时要求 forced
CSD plan 中没有 host scan/join/aggregate/sort，允许顶层 PostgreSQL `Limit` 作为控制
编排。性能回归需固定 cache 状态、runtime、parallel workers、FDM/spill 参数，保留
原始 result/status，不可只保存汇总图。

## 11. AER 与缓存阶段（2026-10-04 开始）

前置事实：FEMU 原代码虽声明 `aer_reqs`、`aer_queue` 和 command-effect support，管理
命令 switch 实际没有 `ASYNC_EVENT_REQUEST`，Linux 发出的 AER 会收到 invalid opcode。

正在实现的最小闭环：

1. FEMU 支持真正 pending 的 AER：admin SQ 消费命令但不立即 CQ completion；事件到达
   后由 QEMU bottom half 在 admin CQ 有空间时完成。controller reset/exit 回收 pending
   request 和 queued event。
2. vendor log page `0xC0` 保存 bounded chunk ring。entry 至少包含 sequence、plan
   generation、nsid、physical offset 和 length；后续为 host file-cache mapping 增加
   relation OID 与 relation logical offset。ring 满时允许覆盖最旧 entry 并累计 dropped，
   暂不追求强一致性。
3. `cemu_args`/`ubpf_jit_args` 增加 `cache_notify` callback。chunked Scan 每次成功读取
   physical extent 后发通知；通知失败是 opportunistic cache miss，不能使 SQL 失败。
4. Linux host driver 识别 vendor AER，读取 log page，并把 entry 暴露给 cache worker；
   AER 本身不能携带或 DMA table data。
5. cache worker 根据 relation OID + logical offset 对 heap file 发普通 `readahead`/buffered
   read，以真正填 PostgreSQL 所在主机的 filesystem page cache。仅按 physical LBA 读
   block device 不等价于填 heap inode page cache，不能用它伪造缓存效果。
6. 若 host file-cache mapping 成本过高，可并行实现 query 间持久 FDM chunk cache，键为
   `(nsid, physical offset, length, generation)`，只使用 4 GiB FDM 中未被 query reservation
   占用的富余空间；必须可逐出并与 operator arena 共同计费。

缓存收益实验至少包含：

- AER/backfill 关闭 vs 开启；首次 CSD 查询相同，随后执行复用同一大表的 HOST 或 CSD
  查询。
- 记录 event、log entries、dropped、prefetch bytes、cache hits/misses、重复 Flash reads、
  query wall time 和 cache fraction。
- 结果仍需 exact；generation 不匹配的 entry 丢弃即可。当前论文原型不要求处理所有
  truncate/rewrite/drop 并发竞态，但必须在局限章节明确。
- 最终要证明“计算与回填重叠后，后续查询比无回填 baseline 更快”，不能仅证明 AER
  completion 被触发。

## 12. 常见陷阱

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
- 任何“支持全部 TPC-H”的结论都必须同时有 planner 22/22 和 result exact 22/22 证据。

