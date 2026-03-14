#!/bin/bash

# ============================================================================
# HunterClaw 开发环境打包脚本
# ============================================================================
# 将项目和 Docker 配置打包，用于在有 Docker 环境的机器上构建镜像
# ============================================================================

set -e

# 颜色定义
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m'

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

# 配置
PROJECT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
OUTPUT_DIR="${PROJECT_DIR}/dist"
PACKAGE_NAME="hunterclaw-dev-env-$(date +%Y%m%d-%H%M%S).tar.gz"
TEMP_DIR="${OUTPUT_DIR}/tmp"

log_info "HunterClaw 开发环境打包工具"
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"

# 创建输出目录
log_info "创建输出目录: ${OUTPUT_DIR}"
mkdir -p "${OUTPUT_DIR}"
mkdir -p "${TEMP_DIR}"

# 复制必要文件
log_info "复制项目文件..."

cd "${PROJECT_DIR}"

# 使用 tar 打包，排除不必要的文件
tar --exclude='./build' \
    --exclude='./dist' \
    --exclude='./.git' \
    --exclude='./output' \
    --exclude='*/__pycache__' \
    --exclude='*.pyc' \
    --exclude='*.o' \
    --exclude='*.so' \
    --exclude='*.a' \
    -czf "${OUTPUT_DIR}/${PACKAGE_NAME}" \
    .

log_success "项目已打包: ${OUTPUT_DIR}/${PACKAGE_NAME}"

# 创建部署说明
cat > "${OUTPUT_DIR}/DEPLOY.md" << 'EOF'
# HunterClaw 开发环境部署指南

## 📦 包内容

此压缩包包含：
- HunterClaw 完整源代码
- Dockerfile.dev (开发环境镜像定义)
- docker-compose.dev.yaml (开发环境编排配置)
- 构建和部署脚本
- 配置文件示例

## 🚀 快速部署

### 前置要求

确保目标机器已安装：
- Docker 20.10+
- Docker Compose 2.0+

### 部署步骤

1. **解压文件**
   ```bash
   tar -xzf hunterclaw-dev-env-*.tar.gz
   cd HunterClaw/
   ```

2. **配置环境变量** (可选)
   ```bash
   cp .env.example .env
   vim .env
   ```
   
   配置 OpenAI API Key 等必要信息：
   ```bash
   OPENAI_API_KEY=sk-xxx
   OPENAI_API_BASE=https://api.openai.com/v1
   ```

3. **构建开发镜像**
   ```bash
   ./deploy/build_dev_docker.sh build
   ```

4. **启动开发环境**
   ```bash
   # 方式1: 使用脚本
   ./deploy/build_dev_docker.sh dev
   
   # 方式2: 使用 docker-compose
   docker-compose -f docker-compose.dev.yaml up -d
   ```

5. **进入开发容器**
   ```bash
   docker exec -it hunterclaw-dev bash
   ```

6. **编译和运行**
   ```bash
   # 在容器内
   build    # 编译项目
   run      # 运行 Gateway
   ```

## 🐳 镜像发布

### 发布到 Docker Hub

```bash
# 登录 Docker Hub
docker login

# 构建、测试并推送
./deploy/build_dev_docker.sh all
```

### 发布到私有镜像仓库

```bash
# 示例：发布到腾讯云 CCR
./deploy/build_dev_docker.sh push -r ccr.ccs.tencentyun.com

# 示例：发布到 GitHub Container Registry
./deploy/build_dev_docker.sh push -r ghcr.io
```

## 📖 详细文档

完整的开发指南请查看：
- `DEVELOPMENT.md` - 开发环境使用指南
- `README.md` - 项目说明
- `deploy/` 目录下的脚本说明

## 🔧 故障排查

### Docker 未安装

**CentOS/RHEL:**
```bash
sudo yum install -y yum-utils
sudo yum-config-manager --add-repo https://download.docker.com/linux/centos/docker-ce.repo
sudo yum install -y docker-ce docker-ce-cli containerd.io docker-compose-plugin
sudo systemctl start docker
sudo systemctl enable docker
```

**Ubuntu/Debian:**
```bash
sudo apt-get update
sudo apt-get install -y docker.io docker-compose
sudo systemctl start docker
sudo systemctl enable docker
```

### 权限问题

将当前用户添加到 docker 组：
```bash
sudo usermod -aG docker $USER
newgrp docker
```

### 端口冲突

修改 `docker-compose.dev.yaml` 中的端口映射。

## 📞 技术支持

遇到问题？
- 查看项目 Issues
- 联系维护者

---
祝开发顺利！🎉
EOF

log_success "部署说明已生成: ${OUTPUT_DIR}/DEPLOY.md"

# 创建 .env.example 示例文件
cat > "${OUTPUT_DIR}/.env.example" << 'EOF'
# ============================================================================
# HunterClaw 开发环境配置
# ============================================================================

# OpenAI 配置
OPENAI_API_KEY=sk-your-api-key-here
OPENAI_API_BASE=https://api.openai.com/v1

# HunterClaw 配置
HUNTERCLAW_API_TOKENS=dev_token_12345
HUNTERCLAW_SIGNING_SECRET=dev_secret_change_me

# 飞书配置 (可选)
FEISHU_APP_ID=
FEISHU_APP_SECRET=
FEISHU_ENCRYPT_KEY=
FEISHU_VERIFICATION_TOKEN=

# 数据库配置 (生产环境)
DB_PASSWORD=change_me_in_production
REDIS_PASSWORD=

# Grafana 配置
GRAFANA_PASSWORD=admin
EOF

log_success "环境变量示例已生成: ${OUTPUT_DIR}/.env.example"

# 显示摘要
echo ""
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
log_success "打包完成！"
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
echo ""
log_info "输出文件:"
ls -lh "${OUTPUT_DIR}" | grep -v "^d" | awk '{printf "  %s  %s\n", $9, $5}'
echo ""
log_info "下一步:"
echo "  1. 将 ${OUTPUT_DIR}/${PACKAGE_NAME} 上传到有 Docker 的机器"
echo "  2. 解压: tar -xzf ${PACKAGE_NAME}"
echo "  3. 按照 DEPLOY.md 的说明进行部署"
echo ""
log_info "或者直接在本地构建 (如果已安装 Docker):"
echo "  cd ${PROJECT_DIR}"
echo "  ./deploy/build_dev_docker.sh build"
echo ""
