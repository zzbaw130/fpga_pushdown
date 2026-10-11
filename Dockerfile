# =============================编译阶段=============================
FROM ubuntu:22.04 AS cemu_builder

# 构建期间静止交互
ARG DEBIAN_FRONTEND="noninteractive"

# 获取编译工具链依赖, 900MB
RUN set -eu && \
    apt update && \
    apt --no-install-recommends -y install \
    # femu pkgdep.sh
    gcc \
    pkg-config \
    git \
    libglib2.0-dev \
    libfdt-dev \
    libpixman-1-dev \
    zlib1g-dev \
    libaio-dev \
    libslirp-dev \
    flex \
    bison \
    libelf-dev \
    libssl-dev \
    cmake \
    libbpf-dev \
    liblz4-dev \
    libboost-dev \
    libboost-program-options-dev \
    libboost-filesystem-dev \
    libnuma-dev \
    ninja-build \
    clang \
    libnvme-dev \
    liburing-dev \
    libcjson-dev \
    libcap-ng-dev \
    libattr1-dev \
    libncurses-dev \
    # 额外所需包
    build-essential \
    bc \
    zstd \
    # postgre所需包
    libreadline-dev \
    libicu-dev && \
    apt clean

# 分层复制，避免修改内核/驱动时使 uBPF 和 2.8GB guest 镜像缓存同时失效。
COPY ./code/linux-cemu /workspace/code/linux-cemu/
RUN set -eu && \
    cd /workspace/code/linux-cemu && \
    make bzImage -j$(nproc)

COPY ./code/ubpf-cemu /workspace/code/ubpf-cemu/
RUN set -eu && \
    cd /workspace/code/ubpf-cemu && \
    cmake -S . -B build -DUBPF_ENABLE_TESTS=true && \
    cmake --build build --config Release

COPY ./code/dqib_amd64-pc /workspace/code/dqib_amd64-pc/

# 最后拷贝可能频繁变动的CEMU项目, 9.9GB
COPY ./code/CEMU /workspace/code/CEMU/

# 编译CEMU, 11.6GB
RUN set -eu && \
    cd /workspace/code/CEMU && \
    mkdir -p build && cd build && \
    ../femu-scripts/femu-copy-scripts.sh . && \
    ./femu-compile.sh && \
    ninja qemu-system-x86_64 && \
    test -x ./qemu-system-x86_64 && \
    cd ../tests/cemu && \
    make kernel

# 从builder阶段删除多余文件, 10.8GB
RUN set -eu && \
    rm -rf /workspace/code/ubpf-cemu && \
    cd /workspace/code/linux-cemu && find . -mindepth 1 -maxdepth 1 ! -name vmlinux -exec rm -rf {} +



# =============================运行阶段=============================
# 精简镜像
FROM ubuntu:22.04

# 获取编译工具链依赖, 900MB
RUN set -eu && \
    apt update && \
    apt --no-install-recommends -y install \
    # femu pkgdep.sh
    gcc \
    pkg-config \
    git \
    libglib2.0-dev \
    libfdt-dev \
    libpixman-1-dev \
    zlib1g-dev \
    libaio-dev \
    libslirp-dev \
    flex \
    bison \
    libelf-dev \
    libssl-dev \
    cmake \
    libbpf-dev \
    liblz4-dev \
    libboost-dev \
    libboost-program-options-dev \
    libboost-filesystem-dev \
    libnuma-dev \
    ninja-build \
    clang \
    libnvme-dev \
    liburing-dev \
    libcjson-dev \
    libcap-ng-dev \
    libattr1-dev \
    libncurses-dev \
    # 额外所需包
    build-essential \
    bc \
    zstd && \
    apt clean


COPY --from=cemu_builder /workspace/code /workspace/code/

# 启动后运行CEMU虚拟机
CMD ["bash","/workspace/code/CEMU/build/run-csd.sh"]

