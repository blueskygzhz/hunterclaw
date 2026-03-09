# ============================================================================
# HunterClaw 生产环境 Dockerfile
# ============================================================================
# 多阶段构建: Builder -> Runtime
# ============================================================================

# ============================================================================
# Stage 1: Builder
# ============================================================================
FROM centos:7 AS builder

# 安装构建依赖
RUN yum install -y centos-release-scl && \
    yum install -y \
        devtoolset-11-gcc \
        devtoolset-11-gcc-c++ \
        cmake3 \
        make \
        git \
        wget \
        openssl-devel \
        libcurl-devel && \
    yum clean all

# 创建cmake软链接
RUN ln -s /usr/bin/cmake3 /usr/bin/cmake

# 设置工作目录
WORKDIR /build

# 复制源代码
COPY . .

# 启用devtoolset-11并编译
RUN source /opt/rh/devtoolset-11/enable && \
    mkdir -p build && cd build && \
    cmake .. \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_INSTALL_PREFIX=/usr/local \
        -DBUILD_TESTS=OFF \
        -DBUILD_DOCS=OFF && \
    make -j$(nproc) && \
    make install DESTDIR=/install_root

# ============================================================================
# Stage 2: Runtime
# ============================================================================
FROM centos:7

# 安装运行时依赖
RUN yum install -y \
        openssl \
        libcurl \
        ca-certificates && \
    yum clean all

# 从builder复制编译产物
COPY --from=builder /install_root/usr/local /usr/local
COPY --from=builder /build/config/production.yaml /etc/hunterclaw/config.yaml

# 创建用户和目录
RUN useradd -r -s /bin/false hunterclaw && \
    mkdir -p \
        /var/lib/hunterclaw/workspaces \
        /var/lib/hunterclaw/memory \
        /var/lib/hunterclaw/db \
        /var/lib/hunterclaw/skills \
        /var/log/hunterclaw && \
    chown -R hunterclaw:hunterclaw \
        /var/lib/hunterclaw \
        /var/log/hunterclaw

# 更新动态链接库缓存
RUN ldconfig

# 切换用户
USER hunterclaw

# 暴露端口
EXPOSE 8080 8081 9090

# 健康检查
HEALTHCHECK --interval=30s --timeout=10s --start-period=40s --retries=3 \
    CMD curl -f http://localhost:8081/health || exit 1

# 工作目录
WORKDIR /var/lib/hunterclaw

# 启动命令
CMD ["/usr/local/bin/hunterclaw-gateway", "--config", "/etc/hunterclaw/config.yaml"]
