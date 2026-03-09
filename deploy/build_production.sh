#!/bin/bash
# ============================================================================
# HunterClaw 生产环境构建脚本
# ============================================================================
# 版本: 0.1.0
# 用途: 编译、打包、生成生产就绪的二进制和配置
# 使用: ./deploy/build_production.sh [options]
# ============================================================================

set -euo pipefail

# ============================================================================
# 颜色定义
# ============================================================================
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m' # No Color

# ============================================================================
# 日志函数
# ============================================================================
log_info() {
    echo -e "${BLUE}[INFO]${NC} $1"
}

log_success() {
    echo -e "${GREEN}[SUCCESS]${NC} $1"
}

log_warn() {
    echo -e "${YELLOW}[WARN]${NC} $1"
}

log_error() {
    echo -e "${RED}[ERROR]${NC} $1"
}

# ============================================================================
# 配置变量
# ============================================================================
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
BUILD_DIR="${PROJECT_ROOT}/build"
DIST_DIR="${PROJECT_ROOT}/dist"
VERSION="0.1.0"
BUILD_TYPE="${BUILD_TYPE:-Release}"

log_info "==================================================================="
log_info "HunterClaw 生产环境构建脚本"
log_info "==================================================================="
log_info "项目根目录: $PROJECT_ROOT"
log_info "构建目录: $BUILD_DIR"
log_info "发布目录: $DIST_DIR"
log_info "构建类型: $BUILD_TYPE"
log_info "版本: $VERSION"
log_info "==================================================================="

# ============================================================================
# 检查依赖
# ============================================================================
log_info "Step 1/7: 检查构建依赖..."

check_command() {
    if ! command -v "$1" &> /dev/null; then
        log_error "未找到命令: $1"
        log_error "请先安装: $2"
        exit 1
    fi
    log_success "✓ $1 已安装"
}

check_command "cmake" "cmake >= 3.20"
check_command "g++" "gcc/g++ >= 11"
check_command "make" "make"

# 检查GCC版本
GCC_VERSION=$(g++ -dumpversion | cut -d. -f1)
if [ "$GCC_VERSION" -lt 11 ]; then
    log_warn "GCC版本低于11，尝试启用 devtoolset-11..."
    if [ -f /opt/rh/devtoolset-11/enable ]; then
        source /opt/rh/devtoolset-11/enable
        log_success "✓ 已启用 devtoolset-11"
    else
        log_error "需要 GCC 11+ 但未找到 devtoolset-11"
        exit 1
    fi
fi

# ============================================================================
# 清理旧构建
# ============================================================================
log_info "Step 2/7: 清理旧构建产物..."

rm -rf "$BUILD_DIR"
rm -rf "$DIST_DIR"
mkdir -p "$BUILD_DIR"
mkdir -p "$DIST_DIR"

log_success "✓ 清理完成"

# ============================================================================
# 配置CMake
# ============================================================================
log_info "Step 3/7: 配置CMake..."

cd "$BUILD_DIR"

cmake .. \
    -DCMAKE_BUILD_TYPE="$BUILD_TYPE" \
    -DCMAKE_INSTALL_PREFIX="/usr/local" \
    -DBUILD_TESTS=OFF \
    -DBUILD_DOCS=OFF \
    -DCMAKE_EXPORT_COMPILE_COMMANDS=ON

if [ $? -ne 0 ]; then
    log_error "CMake配置失败"
    exit 1
fi

log_success "✓ CMake配置完成"

# ============================================================================
# 编译
# ============================================================================
log_info "Step 4/7: 编译项目..."

NPROC=$(nproc)
log_info "使用 $NPROC 个CPU核心进行并行编译"

make -j"$NPROC"

if [ $? -ne 0 ]; then
    log_error "编译失败"
    exit 1
fi

log_success "✓ 编译完成"

# ============================================================================
# 收集二进制文件
# ============================================================================
log_info "Step 5/7: 收集二进制文件..."

mkdir -p "$DIST_DIR/bin"
mkdir -p "$DIST_DIR/lib"
mkdir -p "$DIST_DIR/config"
mkdir -p "$DIST_DIR/plugins/channels"
mkdir -p "$DIST_DIR/plugins/memory"

# 复制可执行文件
if [ -f "$BUILD_DIR/hunterclaw-gateway" ]; then
    cp "$BUILD_DIR/hunterclaw-gateway" "$DIST_DIR/bin/"
    log_success "✓ hunterclaw-gateway"
fi

if [ -f "$BUILD_DIR/bin/hunterclaw" ]; then
    cp "$BUILD_DIR/bin/hunterclaw" "$DIST_DIR/bin/"
    log_success "✓ hunterclaw"
fi

if [ -f "$BUILD_DIR/bin/simple_react" ]; then
    cp "$BUILD_DIR/bin/simple_react" "$DIST_DIR/bin/"
    log_success "✓ simple_react"
fi

# 复制核心库
if [ -f "$BUILD_DIR/bin/libhunterclaw_core.so" ]; then
    cp "$BUILD_DIR/bin/libhunterclaw_core.so" "$DIST_DIR/lib/"
    log_success "✓ libhunterclaw_core.so"
fi

if [ -f "$BUILD_DIR/libhunterclaw_core.so" ]; then
    cp "$BUILD_DIR/libhunterclaw_core.so" "$DIST_DIR/lib/"
    log_success "✓ libhunterclaw_core.so"
fi

# 复制Channel插件
if [ -f "$BUILD_DIR/bin/libhunterclaw_channel_feishu.so" ]; then
    cp "$BUILD_DIR/bin/libhunterclaw_channel_feishu.so" "$DIST_DIR/plugins/channels/"
    log_success "✓ libhunterclaw_channel_feishu.so"
fi

# 复制配置文件
cp "$PROJECT_ROOT/config/production.yaml" "$DIST_DIR/config/config.yaml"
log_success "✓ production.yaml → config.yaml"

if [ -f "$PROJECT_ROOT/config.example.yaml" ]; then
    cp "$PROJECT_ROOT/config.example.yaml" "$DIST_DIR/config/config.example.yaml"
fi

if [ -f "$PROJECT_ROOT/examples/config.example.json" ]; then
    cp "$PROJECT_ROOT/examples/config.example.json" "$DIST_DIR/config/agents.example.json"
fi

# ============================================================================
# 创建部署脚本
# ============================================================================
log_info "Step 6/7: 创建部署脚本..."

cat > "$DIST_DIR/install.sh" << 'INSTALL_SCRIPT'
#!/bin/bash
# HunterClaw 安装脚本

set -euo pipefail

if [ "$EUID" -ne 0 ]; then
    echo "请使用 root 权限运行: sudo ./install.sh"
    exit 1
fi

echo "安装 HunterClaw 到系统..."

# 创建目录
mkdir -p /usr/local/bin
mkdir -p /usr/local/lib
mkdir -p /usr/lib/hunterclaw/plugins/channels
mkdir -p /usr/lib/hunterclaw/plugins/memory
mkdir -p /etc/hunterclaw
mkdir -p /var/lib/hunterclaw/{workspaces,memory,db,skills}
mkdir -p /var/log/hunterclaw
mkdir -p /var/backups/hunterclaw

# 安装二进制
install -m 755 bin/* /usr/local/bin/

# 安装库
install -m 644 lib/* /usr/local/lib/

# 安装插件
if [ -d plugins/channels ]; then
    install -m 644 plugins/channels/* /usr/lib/hunterclaw/plugins/channels/
fi

if [ -d plugins/memory ]; then
    install -m 644 plugins/memory/* /usr/lib/hunterclaw/plugins/memory/
fi

# 安装配置 (不覆盖已有配置)
if [ ! -f /etc/hunterclaw/config.yaml ]; then
    install -m 644 config/config.yaml /etc/hunterclaw/
fi

# 更新动态链接库缓存
ldconfig

echo "✓ 安装完成！"
echo ""
echo "下一步:"
echo "  1. 编辑配置: vim /etc/hunterclaw/config.yaml"
echo "  2. 设置环境变量: export OPENAI_API_KEY='your-key'"
echo "  3. 启动服务: hunterclaw-gateway --config /etc/hunterclaw/config.yaml"
INSTALL_SCRIPT

chmod +x "$DIST_DIR/install.sh"
log_success "✓ install.sh"

# 创建systemd服务文件
cat > "$DIST_DIR/hunterclaw.service" << 'SERVICE_FILE'
[Unit]
Description=HunterClaw AI Agent Gateway
After=network.target
Documentation=https://github.com/hunterclaw/hunterclaw

[Service]
Type=simple
User=hunterclaw
Group=hunterclaw
WorkingDirectory=/var/lib/hunterclaw
ExecStart=/usr/local/bin/hunterclaw-gateway --config /etc/hunterclaw/config.yaml
Restart=on-failure
RestartSec=5s

# 环境变量文件
EnvironmentFile=/etc/hunterclaw/env

# 日志
StandardOutput=journal
StandardError=journal
SyslogIdentifier=hunterclaw

# 安全加固
NoNewPrivileges=true
PrivateTmp=true
ProtectSystem=strict
ProtectHome=true
ReadWritePaths=/var/lib/hunterclaw /var/log/hunterclaw /var/backups/hunterclaw

# 资源限制
LimitNOFILE=65535
LimitNPROC=4096

[Install]
WantedBy=multi-user.target
SERVICE_FILE

log_success "✓ hunterclaw.service"

# 创建环境变量模板
cat > "$DIST_DIR/env.template" << 'ENV_TEMPLATE'
# HunterClaw 环境变量配置
# 复制到 /etc/hunterclaw/env 并填写实际值

# OpenAI API
OPENAI_API_KEY=sk-your-openai-api-key-here
OPENAI_API_BASE=https://api.openai.com/v1

# HunterClaw API Tokens (逗号分隔)
HUNTERCLAW_API_TOKENS=token1,token2,token3

# 飞书配置 (可选)
FEISHU_APP_ID=
FEISHU_APP_SECRET=
FEISHU_ENCRYPT_KEY=
FEISHU_VERIFICATION_TOKEN=

# 数据库配置 (可选)
DB_HOST=localhost
DB_USER=hunterclaw
DB_PASSWORD=

# Redis配置 (可选)
REDIS_HOST=localhost
REDIS_PASSWORD=

# 请求签名密钥
HUNTERCLAW_SIGNING_SECRET=your-random-secret-here
ENV_TEMPLATE

log_success "✓ env.template"

# 创建 README
cat > "$DIST_DIR/README.md" << 'README'
# HunterClaw 生产环境部署包

## 📦 包含内容

```
dist/
├── bin/                          # 二进制文件
│   ├── hunterclaw-gateway        # 网关服务器
│   ├── hunterclaw                # CLI工具
│   └── simple_react              # 示例程序
├── lib/                          # 核心库
│   └── libhunterclaw_core.so
├── plugins/                      # 插件
│   ├── channels/
│   │   └── libhunterclaw_channel_feishu.so
│   └── memory/
├── config/                       # 配置文件
│   ├── config.yaml               # 生产环境配置
│   ├── config.example.yaml       # 配置示例
│   └── agents.example.json       # Agent配置示例
├── install.sh                    # 安装脚本
├── hunterclaw.service            # systemd服务文件
├── env.template                  # 环境变量模板
└── README.md                     # 本文件
```

## 🚀 快速部署

### 1. 安装

```bash
# 解压部署包
tar -xzf hunterclaw-v0.1.0.tar.gz
cd hunterclaw-v0.1.0

# 运行安装脚本
sudo ./install.sh
```

### 2. 配置

```bash
# 编辑配置文件
sudo vim /etc/hunterclaw/config.yaml

# 配置环境变量
sudo cp env.template /etc/hunterclaw/env
sudo vim /etc/hunterclaw/env
```

### 3. 创建服务用户

```bash
sudo useradd -r -s /bin/false hunterclaw
sudo chown -R hunterclaw:hunterclaw /var/lib/hunterclaw
sudo chown -R hunterclaw:hunterclaw /var/log/hunterclaw
```

### 4. 安装systemd服务

```bash
sudo cp hunterclaw.service /etc/systemd/system/
sudo systemctl daemon-reload
sudo systemctl enable hunterclaw
sudo systemctl start hunterclaw
```

### 5. 验证

```bash
# 查看服务状态
sudo systemctl status hunterclaw

# 查看日志
sudo journalctl -u hunterclaw -f

# 健康检查
curl http://localhost:8081/health
```

## 🔧 配置说明

### 必需环境变量

- `OPENAI_API_KEY`: OpenAI API密钥
- `HUNTERCLAW_API_TOKENS`: API访问令牌

### TLS证书

生产环境必须配置TLS证书:

```bash
# 生成自签名证书 (测试用)
sudo openssl req -x509 -nodes -days 365 -newkey rsa:2048 \
  -keyout /etc/hunterclaw/ssl/server.key \
  -out /etc/hunterclaw/ssl/server.crt

# 使用Let's Encrypt (推荐)
sudo certbot certonly --standalone -d api.example.com
sudo cp /etc/letsencrypt/live/api.example.com/fullchain.pem /etc/hunterclaw/ssl/server.crt
sudo cp /etc/letsencrypt/live/api.example.com/privkey.pem /etc/hunterclaw/ssl/server.key
```

## 📊 监控

- **健康检查**: http://localhost:8081/health
- **Prometheus指标**: http://localhost:9090/metrics

## 🔒 安全建议

1. 始终启用TLS
2. 配置强API Token
3. 启用请求签名验证
4. 配置防火墙规则
5. 定期更新依赖
6. 启用审计日志
7. 配置Sandbox网络策略

## 📝 日志

- **应用日志**: `/var/log/hunterclaw/hunterclaw.log`
- **审计日志**: `/var/log/hunterclaw/audit.log`
- **systemd日志**: `journalctl -u hunterclaw`

## 🆘 故障排查

### 服务无法启动

```bash
# 检查配置文件语法
hunterclaw-gateway --config /etc/hunterclaw/config.yaml --validate

# 查看详细日志
sudo journalctl -u hunterclaw -n 100 --no-pager

# 检查端口占用
sudo netstat -tlnp | grep 8080
```

### 依赖问题

```bash
# 检查动态链接库
ldd /usr/local/bin/hunterclaw-gateway

# 更新库缓存
sudo ldconfig
```

## 📚 更多文档

- [官方文档](https://docs.hunterclaw.io)
- [API参考](https://api.hunterclaw.io/docs)
- [GitHub](https://github.com/hunterclaw/hunterclaw)

## 📄 许可证

Apache License 2.0
README

log_success "✓ README.md"

# ============================================================================
# 打包
# ============================================================================
log_info "Step 7/7: 创建发布包..."

cd "$PROJECT_ROOT"
TARBALL="hunterclaw-v${VERSION}-$(uname -m).tar.gz"

tar -czf "$TARBALL" -C dist .

if [ $? -eq 0 ]; then
    TARBALL_SIZE=$(du -h "$TARBALL" | cut -f1)
    log_success "✓ 发布包创建成功: $TARBALL ($TARBALL_SIZE)"
else
    log_error "发布包创建失败"
    exit 1
fi

# ============================================================================
# 生成校验和
# ============================================================================
log_info "生成SHA256校验和..."
sha256sum "$TARBALL" > "${TARBALL}.sha256"
log_success "✓ ${TARBALL}.sha256"

# ============================================================================
# 构建总结
# ============================================================================
echo ""
log_success "==================================================================="
log_success "构建完成！"
log_success "==================================================================="
echo ""
log_info "发布包:"
log_info "  - $TARBALL ($TARBALL_SIZE)"
log_info "  - ${TARBALL}.sha256"
echo ""
log_info "发布目录: $DIST_DIR"
log_info "  - bin/         : 可执行文件"
log_info "  - lib/         : 共享库"
log_info "  - plugins/     : 插件"
log_info "  - config/      : 配置文件"
log_info "  - install.sh   : 安装脚本"
echo ""
log_info "下一步:"
log_info "  1. 测试构建: cd $DIST_DIR && ./bin/simple_react"
log_info "  2. 部署到服务器: scp $TARBALL user@server:/tmp/"
log_info "  3. 服务器上解压并安装: tar -xzf $TARBALL && sudo ./install.sh"
echo ""
log_success "==================================================================="
