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

docker-compose.yml中的CSD_COUNT指定了CSD的个数, 考虑到内存限制, 默认为1（运行测试脚本时需要注释掉Three CSDs Tests）, 虚拟机为默认配置, 即8核CPU+8G内存+1块48GB CSD

虚拟机启动后使用ssh -p 2222 root@localhost, 连接虚拟机(密码root)

进入虚拟机后，需要运行cemu-mount.sh挂载CSD，现在运行该脚本需要指定一个整型参数，用于指定初始化的数据文件大小(GB)，防止占用过多主机内存

之后即可从论文Appendix的A.5中的连接ssh步骤之后开始实验

```
# 编译&运行镜像
Host:~/cemu_docker& docker compose build
Host:~/cemu_docker& docker compose up -d

# 虚拟机启动后(约1分钟), ssh连接进入容器(跳过了容器层, 直接连接到容器内部的虚拟机)
Host:~/cemu_docker& ssh root@localhost -p 2222

# 运行cemu-mount.sh挂载CSD之后开始后续实验, 参数表示每个CSD的数据文件大小(此处以4GB为例)
# CEMU对主机内存的需求 = 8G虚拟机主存 + 32*32M的CSD内存  CSD_COUNT*数据文件大小 = 13GB
inside_VM:~# ./cemu-mount.sh 4

.....
```

## 测试说明
目录结构中有一个test-cemu目录，它是code/CEMU/tests/cemu复制目录，我单独将他拉出来并且挂载到了虚拟机内部，因此只需要在主机端修改其中的测试文件，虚拟机内部就会自动同步，方便二次开发

整个实验阶段，我们只需要在该目录下，参考其内部的实现方法进行二次开发即可，比如将diskann源码clone到该目录下，将其改造成kernel目录下形式的函数，并按照vadd_example或cemu_benchmark的方式调用即可
