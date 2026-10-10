# CEMU Docker
## 架构说明
此项目中的CEMU源码clone时间为2026.04.11, 源码目录未进行任何编译, 所有编译都在dockerfile中进行

CEMU目录中是基于FEMU(基于QEMU)修改后的CEMU源码, 对其进行了一些配置上的修改

linux-cemu目录中是基于linux-6.8 kernel, 为cemu增加了适配接口的修改版kernel

ubpf-cemu目录中是为cemu增加ubpf能力的插件

dqib_amd64-pc目录中是qemu启动时所需的引导盘

Dockerfile中包括两阶段, 编译阶段和运行阶段, 后者删除了编译后所有不再使用的文件, 将docker镜像压缩为6.28GB, 其中包括:
- ubuntu:22.04 + 编译工具链, 约为1GB
- CEMU目录: 编译后的build目录仍然依赖于CEMU根目录, 因此暂未删减, 约为2.6GB
- linux-cemu目录: 只保留编译后的vmlinux, 约为0.5GB
- dqib_amd64-pc目录: 引导盘, 无法删减, 约为2.7GB

## 使用说明
使用docker compose build编译cemu_vm镜像, 之后使用docker compose up -d后台启动容器即可

docker-compose.yml中的CSD_COUNT指定了CSD的个数, 考虑到内存限制, 默认为1（运行测试脚本时需要注释掉Three CSDs Tests）, 虚拟机为默认配置, 即8核CPU+16G内存+1块48GB CSD

虚拟机启动后使用ssh -p 2223 root@localhost, 连接虚拟机(密码root)。本项目
使用 2223 以免与主机上已有的 CEMU 实验冲突。

进入虚拟机后，需要运行cemu-mount.sh挂载CSD，现在运行该脚本需要指定一个整型参数，用于指定初始化的数据文件大小(GB)，防止占用过多主机内存

之后即可从论文Appendix的A.5中的连接ssh步骤之后开始实验

```
# 编译&运行镜像
Host:~/cemu_docker& docker compose build
Host:~/cemu_docker& docker compose up -d

# 虚拟机启动后(约1分钟), ssh连接进入容器(跳过了容器层, 直接连接到容器内部的虚拟机)
Host:~/cemu_docker& ssh root@localhost -p 2223

# 运行cemu-mount.sh挂载CSD之后开始后续实验, 参数表示每个CSD的数据文件大小(此处以4GB为例)
# CEMU对主机内存的需求 = 8G虚拟机主存 + 32*32M的CSD内存  CSD_COUNT*数据文件大小 = 13GB
inside_VM:~# ./cemu-mount.sh 4

.....
```

## PostgreSQL 与 TPC-H 数据准备

先初始化并编译 `code/postgres`（REL_12_STABLE submodule），再用 DuckDB 的
`tpch` 与 `postgres` 扩展生成数据。一个 scale factor 对应一个独立的
PostgreSQL cluster，数据目录为 `data/tpch_s<SF>`、数据库名为
`tpch_s<SF>`；脚本会创建表、装载数据、创建主键/外键/索引并执行
`ANALYZE`。

```bash
git submodule update --init code/postgres
./scripts/build_postgres.sh
./scripts/gen_data/create_tpch_db.sh --sf 1
./scripts/gen_data/create_tpch_db.sh --sf 32 --keep-running
```

`--keep-running` 保留本地实例（默认端口为 `55432 + SF`），便于检查；
不带该选项时脚本在完成后干净停止集群。SF32 装载时间较长，若主机或
终端会话中断，可执行 `./scripts/gen_data/create_tpch_db.sh --sf 32
--resume --keep-running`。续传会保留已完整导入的表，并继续空表；如需
明确重建某个 cluster，才使用 `--force`。

CEMU 必须运行在 TCG 下以支持 VM-Freeze；本项目不启用 KVM。PostgreSQL
在 CEMU 来宾内编译一次并安装到持久化 qcow2 覆盖层的 `/opt/pgsql12`；
之后每次启动均直接复用。虚拟机启动后，首次以 root 登录一次，创建
UID/GID 与共享数据一致的 `csdpg` 用户并编译 PostgreSQL：

```bash
docker compose up -d

# Guest: only needed once for a fresh named volume
ssh -p 2223 root@localhost
/root/CEMU/scripts/guest/bootstrap_csdpg.sh
/root/CEMU/scripts/guest/build_postgres.sh
# 之后使用密码 csdpg 登录；首次登录后应执行 passwd 修改密码
ssh -p 2223 csdpg@localhost
```

`build_postgres.sh` 会为 guest 内的 `/opt/pgsql12/bin/postgres` 设置
`cap_sys_admin=ep`，使普通 `csdpg` backend 可以继续通过
`NVME_IOCTL_IO_CMD` 提交 CEMU Program Execute，并保持原有 NVMe queue 与
VM-Freeze 路径。已有安装无需重编，可由 root 重新授权：

```bash
/root/CEMU/scripts/guest/grant_csd_capability.sh
```

该能力仅用于科研仿真环境，不适用于生产部署。

Docker named volume 会保存来宾 qcow2 覆盖层，因此 `csdpg` 用户与来宾
内编译的 PostgreSQL 会跨 `docker compose up --force-recreate` 保留。为保证
PostgreSQL 数据访问和 CSD 页读取都经过同一个 CEMU NVMe namespace，先停止
主机上的对应实例，再以 root 将 cluster 从 9p 共享目录一次性装载到 NVMe。
当前开发和验证默认使用 SF=1：

```bash
# Guest root：自动挂载 NVMe/FDMFS 并复制 SF=1 PGDATA
/root/CEMU/scripts/guest/stage_tpch_nvme.sh 1

# Guest csdpg：使用 guest 内编译的 PostgreSQL 验证 NVMe 副本
/mnt/cemu/scripts/guest/run_nvme_tpch.sh 1
```

装载脚本拒绝复制带有 `postmaster.pid` 的活动 cluster，并在 NVMe 上已有同名
副本时要求显式使用 `--force`。9p 只承担一次性数据导入，不进入查询执行或
CSD 性能测量路径。CEMU 设备重新初始化后需要重新运行装载脚本。

CSD 计划 ABI、FPGA 风格算子、spill-to-flash、AER 缓存回填和 PostgreSQL
`csd.exe_path` 放置策略的实现边界见 [CSD 设计文档](docs/csd_design.md)。

PostgreSQL 规划器扩展位于 `code/postgres-csd`。它目前支持
`csd.exe_path = host|csd|auto`、TPC-H 所需表达式白名单、CSD
scan/project/join/aggregate/sort/limit 的真实 ABI v8 lowering 和递归算子成本
元数据；Q2 的相关标量 SubPlan 会解相关为分组聚合加双键连接，Q16 在证明
两侧列均为 NOT NULL 后转为 hash anti-join，因此 22 条查询已无 pending
表达式。执行器通过 FIEMAP 将 heap 的 NVMe 物理 extent 写入计划；CEMU 使用
每个逻辑 Scan 的 32 MiB 可复用 FDM 窗口分块读取，不再把整表复制进 FDMFS。
解码后的元组经过两块交替的 32 MiB FDM FIFO，再进入算子 DAG。ABI v8 先从
默认 4 GiB FDM 中扣除输出、FIFO、Scan 窗口、spill staging、计划和状态，再把
全部剩余空间建成查询级 operator arena；`csd.device_work_mem_mb=0` 表示自动使用
这些剩余空间，正值才是人为上限。Join/Sort 等阻塞状态只有申请 arena 失败时
才 spill 到 NVMe Flash。执行仍通过原始
`NVME_IOCTL_IO_CMD` Program Execute 路径触发 TCG VM-Freeze，随后把结果 cell
解码回 PostgreSQL slot。query-local range 和 spill 文件在成功或错误退出时都会
回收。FDMFS 空闲区按地址双向合并，避免不同查询释放不等长 range 后出现“总量
充足但无连续空间”的假性 OOM；guest 内可用 `tests/test_fdmfs_reuse.sh` 验证完整
4 GiB 的回收与再分配。可用 SF1 已启动实例验证 planner：

```bash
./tests/test_csd_planner.sh --sf 1 --mode csd
./tests/test_csd_planner.sh --sf 1 --mode host --skip-build
```

在 guest 内启动 NVMe PGDATA 后，可执行完整结果对比。脚本逐条运行 CSD/HOST、
保存原始输出并要求 22 条结果精确一致：

```bash
cd /mnt/cemu
./tests/test_csd_tpch_execution.sh --sf 1 \
  --socket-dir /tmp/csdpg_nvme_tpch_s1 --port 57432 \
  --shared-library /workspace/code/CEMU/tests/cemu/build/csd_sql.so
```

当前 SF=1 的全部 22 条查询已经在真实 CEMU Program Execute 路径执行并与
同一 guest PostgreSQL 的 HOST 结果逐字符一致。该结论是功能正确性里程碑，
不是最终性能结论；正式性能实验仍需固定 cache 状态、CEMU runtime 参数和
主机墙钟/模拟时钟口径。ABI v8 operator arena、Q8 零 spill 以及 FDMFS 跨查询
回收数据记录在 [SF=1 FDM arena 验证](results/tpch_sf1_fdm_arena_validation.md)。
外部 Sort 的 Flash run 写入、真实读回和稳定归并记录在
[SF=1 Sort spill replay 验证](results/tpch_sf1_sort_spill_replay_validation.md)。
Hash Join 的 radix 分区、Flash replay 和低内存 build lease 记录在
[SF=1 Join spill replay 验证](results/tpch_sf1_join_spill_replay_validation.md)。
分组 Aggregate（含 grouped `DISTINCT`/`AVG`）的递归 radix replay 记录在
[SF=1 Aggregate spill replay 验证](results/tpch_sf1_aggregate_spill_replay_validation.md)。
Scan/FIFO 直连 external Sort、cursor merge 与保守准入的流式 Aggregate 分别记录在
[Scan-to-Sort 验证](results/tpch_sf1_streaming_sort_validation.md)、
[Sort cursor merge 验证](results/tpch_sf1_cursor_merge_validation.md) 和
[Scan-to-Aggregate 验证](results/tpch_sf1_streaming_aggregate_validation.md)。
右侧 build 驻留、左侧 Scan/FIFO 逐批 probe 的 one-sided streaming Join 及其低内存
Grace/radix 回退记录在
[Scan-to-Join 验证](results/tpch_sf1_streaming_join_validation.md)。
当 build 超出 arena 时，probe Scan 逐 FIFO batch 直接写 Flash partition、用全局 ordinal
保序 replay 的后续验证记录在
[Join probe-to-Flash 验证](results/tpch_sf1_partition_probe_validation.md)。
当 Join 两侧均为可融合的 Scan 流水链时，超大 build 也按 FIFO batch 直接写 Flash，
并用实际 partition 字节自适应扩大 radix fan-out；Q3 32 MiB 的验证记录在
[Join build-to-Flash 验证](results/tpch_sf1_streaming_build_validation.md)。
两侧流式 Join 的每条 resident handle 进一步改为 Flash 反向链表，内存中只保留每个
radix partition 的固定链头；Q3 32 MiB 与完整回归记录在
[Join spill-chain 验证](results/tpch_sf1_spill_chain_validation.md)。
partition replay 的完整 `OrderedJoinRow` 向量进一步替换为 binary-carry Flash runs，
常驻 reorder handle 为 O(log runs)；Q3 32 MiB 的时延/I/O 校准与完整回归记录在
[Join ordinal-merge 验证](results/tpch_sf1_ordinal_merge_validation.md)。

## 测试说明
`code/test-cemu` 是挂载到来宾 `/root/CEMU/code/CEMU/tests/cemu` 的最小 CSD
开发目录；修改主机端源码会直接反映到来宾。目录只保留 CSD 计划 ABI、独立
算子文件、CEMU Program Execute runner，以及它们必需的 ioctl/helper 代码；原始
CEMU 的 vadd、grep、knn、BPF、io_uring、benchmark 和测试样例已移除。

```bash
make -C code/test-cemu test                 # 主机侧算子与并行调度单元测试
make -C code/test-cemu all                  # 构建 csd_sql.so 与 CEMU runner

# 在 TCG CEMU 来宾中
cd /root/CEMU/code/CEMU/tests/cemu
make -C kernel all && make all
./build/csd_sql_example
```

算子实现位于 `kernel/csd_{fifo,scan,expression,filter,project,aggregate,join,sort,control}.cc`；
`kernel/csd_sql.cc` 仅负责 ABI 校验、任务 DAG 调度、并行 worker 预算、状态和
结果序列化。ABI v8 的无跳转表达式指令流覆盖 TPC-H 所需的定点算术、布尔
逻辑、`LIKE`、`CASE`、`substring` 和日期年份提取；单元测试同时覆盖多键
group/join/sort、join residual、`DISTINCT`、top-N、spill 及规范化节点计数。
