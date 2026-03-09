#!/bin/bash
# ============================================================================
# HunterClaw 最小化生产构建脚本
# ============================================================================
# 只编译核心ReAct功能，零外部依赖
# ============================================================================

set -euo pipefail

RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m'

log_info() { echo -e "${BLUE}[INFO]${NC} $1"; }
log_success() { echo -e "${GREEN}[SUCCESS]${NC} $1"; }
log_error() { echo -e "${RED}[ERROR]${NC} $1"; }

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
DIST_DIR="${PROJECT_ROOT}/dist"
VERSION="0.1.0"

log_info "==================================================================="
log_info "HunterClaw 最小化生产构建 (零依赖)"
log_info "==================================================================="

# 检查编译器
if ! command -v g++ &> /dev/null; then
    log_error "未找到 g++"
    exit 1
fi

# 启用devtoolset-11
if [ -f /opt/rh/devtoolset-11/enable ]; then
    source /opt/rh/devtoolset-11/enable
    log_success "✓ 已启用 devtoolset-11"
fi

# 清理
log_info "清理旧构建..."
rm -rf "$DIST_DIR"
mkdir -p "$DIST_DIR"/{bin,config,scripts}

# 编译核心二进制
log_info "编译核心二进制..."

cd "$PROJECT_ROOT"

# 方案1: 使用已编译的二进制
if [ -f "bin/hunterclaw" ] && [ -f "bin/libhunterclaw_core.so" ]; then
    log_info "使用已编译的二进制..."
    cp bin/hunterclaw "$DIST_DIR/bin/"
    cp bin/libhunterclaw_core.so "$DIST_DIR/bin/"
    
    if [ -f "bin/simple_react" ]; then
        cp bin/simple_react "$DIST_DIR/bin/"
    fi
    
    if [ -f "bin/libhunterclaw_channel_feishu.so" ]; then
        cp bin/libhunterclaw_channel_feishu.so "$DIST_DIR/bin/"
    fi
    
    log_success "✓ 二进制复制完成"
else
    log_error "未找到已编译的二进制文件"
    log_info "请先运行: ./build.sh"
    exit 1
fi

# 复制配置
log_info "复制配置文件..."
cp config/production.yaml "$DIST_DIR/config/config.yaml"
cp config.example.yaml "$DIST_DIR/config/config.example.yaml"
if [ -f examples/config.example.json ]; then
    cp examples/config.example.json "$DIST_DIR/config/agents.example.json"
fi

# 创建启动脚本
log_info "创建启动脚本..."

cat > "$DIST_DIR/scripts/start.sh" << 'START_SCRIPT'
#!/bin/bash
# HunterClaw 启动脚本

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
APP_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"

export LD_LIBRARY_PATH="$APP_ROOT/bin:$LD_LIBRARY_PATH"

# 检查环境变量
if [ -z "${OPENAI_API_KEY:-}" ]; then
    echo "错误: 未设置 OPENAI_API_KEY 环境变量"
    echo "使用方法: export OPENAI_API_KEY='your-key'"
    exit 1
fi

# 启动
cd "$APP_ROOT"
exec "$APP_ROOT/bin/hunterclaw" "$@"
START_SCRIPT

chmod +x "$DIST_DIR/scripts/start.sh"

# 创建简易安装脚本
cat > "$DIST_DIR/install.sh" << 'INSTALL_SCRIPT'
#!/bin/bash
set -euo pipefail

if [ "$EUID" -ne 0 ]; then
    echo "请使用 root 权限运行: sudo ./install.sh"
    exit 1
fi

echo "安装 HunterClaw..."

mkdir -p /opt/hunterclaw
cp -r bin config scripts /opt/hunterclaw/

mkdir -p /var/lib/hunterclaw/{workspaces,memory,db}
mkdir -p /var/log/hunterclaw

cat > /usr/local/bin/hunterclaw << 'WRAPPER'
#!/bin/bash
export LD_LIBRARY_PATH="/opt/hunterclaw/bin:$LD_LIBRARY_PATH"
exec /opt/hunterclaw/bin/hunterclaw "$@"
WRAPPER

chmod +x /usr/local/bin/hunterclaw

echo "✓ 安装完成!"
echo ""
echo "使用方法:"
echo "  export OPENAI_API_KEY='your-key'"
echo "  hunterclaw"
INSTALL_SCRIPT

chmod +x "$DIST_DIR/install.sh"

# 创建README
cat > "$DIST_DIR/README.md" << 'README'
# HunterClaw 生产部署包

## 📦 内容

```
dist/
├── bin/
│   ├── hunterclaw                      # 主程序
│   ├── libhunterclaw_core.so           # 核心库
│   ├── simple_react                    # 示例程序
│   └── libhunterclaw_channel_feishu.so # 飞书插件
├── config/
│   ├── config.yaml                     # 生产配置
│   └── config.example.yaml             # 配置示例
├── scripts/
│   └── start.sh                        # 启动脚本
├── install.sh                          # 安装脚本
└── README.md
```

## 🚀 快速开始

### 本地运行

```bash
# 设置环境变量
export OPENAI_API_KEY='sk-your-key-here'

# 设置库路径
export LD_LIBRARY_PATH="$PWD/bin:$LD_LIBRARY_PATH"

# 运行
./bin/hunterclaw
```

### 使用启动脚本

```bash
export OPENAI_API_KEY='sk-your-key-here'
./scripts/start.sh
```

### 系统安装

```bash
sudo ./install.sh
export OPENAI_API_KEY='sk-your-key-here'
hunterclaw
```

## 📝 配置

编辑 `config/config.yaml` 自定义配置。

## 🔧 环境变量

必需:
- `OPENAI_API_KEY`: OpenAI API密钥

可选:
- `OPENAI_API_BASE`: API端点
- `LD_LIBRARY_PATH`: 库路径 (自动设置)

## 📊 测试

运行示例:
```bash
export LD_LIBRARY_PATH="$PWD/bin:$LD_LIBRARY_PATH"
./bin/simple_react
```

## 📚 文档

详细文档: https://github.com/hunterclaw/hunterclaw
README

# 打包
log_info "创建发布包..."
cd "$PROJECT_ROOT"
TARBALL="hunterclaw-minimal-v${VERSION}-$(uname -m).tar.gz"
tar -czf "$TARBALL" -C dist .

TARBALL_SIZE=$(du -h "$TARBALL" | cut -f1)
sha256sum "$TARBALL" > "${TARBALL}.sha256"

log_success "==================================================================="
log_success "构建完成！"
log_success "==================================================================="
echo ""
log_info "发布包: $TARBALL ($TARBALL_SIZE)"
log_info "校验和: ${TARBALL}.sha256"
echo ""
log_info "快速测试:"
log_info "  cd $DIST_DIR"
log_info "  export OPENAI_API_KEY='your-key'"
log_info "  export LD_LIBRARY_PATH=\"\$PWD/bin:\$LD_LIBRARY_PATH\""
log_info "  ./bin/hunterclaw"
echo ""
log_success "==================================================================="
