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

Docker named volume 会保存来宾 qcow2 覆盖层，因此 `csdpg` 用户与来宾
内编译的 PostgreSQL 会跨 `docker compose up --force-recreate` 保留。登录
`csdpg` 后，直接在 9p 挂载的 `data/tpch_s<SF>` 上运行只读冒烟测试，
不复制数据库：

```bash
/mnt/cemu/scripts/guest/run_shared_tpch.sh 1
```

运行直接挂载测试前必须停止对应的主机 PostgreSQL 实例。PostgreSQL 即使
只读查询也会创建 PID 文件和少量状态文件，因此同一 PGDATA 不能同时被
主机和来宾使用。9p 直接运行仅用于功能验证；正式 CSD 性能实验应让
PostgreSQL 主机端使用 CSD 的本地块设备，避免将 9p 的额外开销当作 CSD
性能。

CSD 计划 ABI、FPGA 风格算子、spill-to-flash、AER 缓存回填和 PostgreSQL
`csd.exe_path` 放置策略的实现边界见 [CSD 设计文档](docs/csd_design.md)。

PostgreSQL 规划器扩展位于 `code/postgres-csd`。它目前支持
`csd.exe_path = host|csd|auto`、TPC-H 所需表达式白名单、CSD
scan/project/join/aggregate/sort/limit 的真实 ABI v4 lowering 和递归算子成本
元数据；Q2 的相关标量 SubPlan 会解相关为分组聚合加双键连接，Q16 在证明
两侧列均为 NOT NULL 后转为 hash anti-join，因此 22 条查询已无 pending
表达式。执行器仍会明确拒绝尚未接通的 NVMe 传输。可用 SF1 已启动实例验证：

```bash
./tests/test_csd_planner.sh --sf 1 --mode csd
./tests/test_csd_planner.sh --sf 1 --mode host --skip-build
```

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

算子实现位于 `kernel/csd_{scan,expression,filter,project,aggregate,join,sort,control}.cc`；
`kernel/csd_sql.cc` 仅负责 ABI 校验、任务 DAG 调度、并行 worker 预算、状态和
结果序列化。ABI v4 的无跳转表达式指令流覆盖 TPC-H 所需的定点算术、布尔
逻辑、`LIKE`、`CASE`、`substring` 和日期年份提取；单元测试同时覆盖多键
group/join/sort、join residual、`DISTINCT`、top-N、spill 及规范化节点计数。
