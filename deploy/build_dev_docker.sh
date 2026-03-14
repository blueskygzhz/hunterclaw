#!/bin/bash

# ============================================================================
# HunterClaw 开发环境 Docker 构建和发布脚本
# ============================================================================

set -e

# 颜色定义
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m' # No Color

# 配置
IMAGE_NAME="hunterclaw/hunterclaw"
DEV_TAG="0.1.0-dev"
DOCKER_REGISTRY="${DOCKER_REGISTRY:-docker.io}"  # 默认 Docker Hub
FULL_IMAGE="${DOCKER_REGISTRY}/${IMAGE_NAME}:${DEV_TAG}"

# ============================================================================
# 辅助函数
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

print_separator() {
    echo "════════════════════════════════════════════════════════════════"
}

# ============================================================================
# 检查依赖
# ============================================================================
check_dependencies() {
    log_info "检查依赖..."
    
    if ! command -v docker &> /dev/null; then
        log_error "Docker 未安装"
        exit 1
    fi
    
    if ! command -v docker-compose &> /dev/null; then
        log_warn "docker-compose 未安装，将跳过 compose 相关操作"
    fi
    
    log_success "依赖检查通过"
}

# ============================================================================
# 构建开发镜像
# ============================================================================
build_dev_image() {
    print_separator
    log_info "开始构建开发环境镜像..."
    print_separator
    
    # 记录构建开始时间
    BUILD_START=$(date +%s)
    
    # 构建镜像
    docker build \
        -f Dockerfile.dev \
        -t "${IMAGE_NAME}:${DEV_TAG}" \
        -t "${IMAGE_NAME}:dev-latest" \
        --build-arg BUILD_DATE="$(date -u +'%Y-%m-%dT%H:%M:%SZ')" \
        --build-arg VCS_REF="$(git rev-parse --short HEAD 2>/dev/null || echo 'unknown')" \
        . || {
            log_error "镜像构建失败"
            exit 1
        }
    
    # 计算构建时间
    BUILD_END=$(date +%s)
    BUILD_TIME=$((BUILD_END - BUILD_START))
    
    print_separator
    log_success "镜像构建完成 (耗时: ${BUILD_TIME}s)"
    print_separator
    
    # 显示镜像信息
    log_info "镜像信息:"
    docker images | grep -E "REPOSITORY|${IMAGE_NAME}" | grep -E "REPOSITORY|dev"
}

# ============================================================================
# 测试镜像
# ============================================================================
test_image() {
    print_separator
    log_info "测试镜像..."
    print_separator
    
    # 测试编译器版本
    log_info "测试 GCC 版本:"
    docker run --rm "${IMAGE_NAME}:${DEV_TAG}" gcc --version | head -1
    
    log_info "测试 CMake 版本:"
    docker run --rm "${IMAGE_NAME}:${DEV_TAG}" cmake --version | head -1
    
    log_info "测试 Git 版本:"
    docker run --rm "${IMAGE_NAME}:${DEV_TAG}" git --version
    
    log_success "镜像测试通过"
}

# ============================================================================
# 推送到镜像仓库
# ============================================================================
push_image() {
    print_separator
    log_info "推送镜像到仓库..."
    print_separator
    
    # 检查是否已登录
    if ! docker info 2>&1 | grep -q "Username"; then
        log_warn "未登录到 Docker 仓库"
        log_info "请先执行: docker login ${DOCKER_REGISTRY}"
        read -p "是否现在登录? (y/n) " -n 1 -r
        echo
        if [[ $REPLY =~ ^[Yy]$ ]]; then
            docker login "${DOCKER_REGISTRY}"
        else
            log_error "取消推送"
            exit 1
        fi
    fi
    
    # 打标签
    log_info "打标签: ${FULL_IMAGE}"
    docker tag "${IMAGE_NAME}:${DEV_TAG}" "${FULL_IMAGE}"
    docker tag "${IMAGE_NAME}:${DEV_TAG}" "${DOCKER_REGISTRY}/${IMAGE_NAME}:dev-latest"
    
    # 推送镜像
    log_info "推送镜像: ${FULL_IMAGE}"
    docker push "${FULL_IMAGE}" || {
        log_error "镜像推送失败"
        exit 1
    }
    
    log_info "推送标签: dev-latest"
    docker push "${DOCKER_REGISTRY}/${IMAGE_NAME}:dev-latest" || {
        log_error "latest 标签推送失败"
        exit 1
    }
    
    log_success "镜像推送完成"
}

# ============================================================================
# 启动开发环境
# ============================================================================
start_dev_env() {
    print_separator
    log_info "启动开发环境..."
    print_separator
    
    if command -v docker-compose &> /dev/null; then
        docker-compose -f docker-compose.dev.yaml up -d
        
        log_success "开发环境已启动"
        echo ""
        log_info "连接到开发容器:"
        echo "  docker exec -it hunterclaw-dev bash"
        echo ""
        log_info "查看日志:"
        echo "  docker-compose -f docker-compose.dev.yaml logs -f"
    else
        log_warn "docker-compose 未安装，手动启动容器..."
        docker run -d \
            --name hunterclaw-dev \
            -p 8080:8080 -p 8081:8081 -p 9090:9090 \
            -v "$(pwd):/workspace:cached" \
            "${IMAGE_NAME}:${DEV_TAG}"
        
        log_success "开发容器已启动: hunterclaw-dev"
        echo ""
        log_info "连接到开发容器:"
        echo "  docker exec -it hunterclaw-dev bash"
    fi
}

# ============================================================================
# 清理
# ============================================================================
cleanup() {
    print_separator
    log_info "清理资源..."
    print_separator
    
    log_info "停止并删除开发容器..."
    docker-compose -f docker-compose.dev.yaml down 2>/dev/null || docker stop hunterclaw-dev 2>/dev/null || true
    docker rm hunterclaw-dev 2>/dev/null || true
    
    log_info "删除旧镜像..."
    docker images | grep "${IMAGE_NAME}" | grep "dev" | awk '{print $3}' | xargs -r docker rmi -f || true
    
    log_success "清理完成"
}

# ============================================================================
# 显示帮助
# ============================================================================
show_help() {
    cat << EOF
HunterClaw 开发环境 Docker 管理脚本

用法: $0 [命令] [选项]

命令:
    build       构建开发镜像
    test        测试镜像
    push        推送镜像到仓库
    start       启动开发环境
    stop        停止开发环境
    clean       清理资源
    all         执行: build + test + push
    dev         执行: build + start (快速开发)

选项:
    -r, --registry REGISTRY    指定镜像仓库 (默认: docker.io)
    -t, --tag TAG             指定镜像标签 (默认: 0.1.0-dev)
    -h, --help                显示此帮助信息

示例:
    $0 build                  # 构建开发镜像
    $0 dev                    # 构建并启动开发环境
    $0 all                    # 构建、测试并推送
    $0 push -r ghcr.io        # 推送到 GitHub Container Registry

环境变量:
    DOCKER_REGISTRY          镜像仓库地址 (默认: docker.io)

EOF
}

# ============================================================================
# 主函数
# ============================================================================
main() {
    # 解析参数
    COMMAND=""
    while [[ $# -gt 0 ]]; do
        case $1 in
            build|test|push|start|stop|clean|all|dev)
                COMMAND=$1
                shift
                ;;
            -r|--registry)
                DOCKER_REGISTRY="$2"
                FULL_IMAGE="${DOCKER_REGISTRY}/${IMAGE_NAME}:${DEV_TAG}"
                shift 2
                ;;
            -t|--tag)
                DEV_TAG="$2"
                FULL_IMAGE="${DOCKER_REGISTRY}/${IMAGE_NAME}:${DEV_TAG}"
                shift 2
                ;;
            -h|--help)
                show_help
                exit 0
                ;;
            *)
                log_error "未知参数: $1"
                show_help
                exit 1
                ;;
        esac
    done
    
    # 如果没有指定命令，显示帮助
    if [ -z "$COMMAND" ]; then
        show_help
        exit 1
    fi
    
    # 检查依赖
    check_dependencies
    
    # 执行命令
    case $COMMAND in
        build)
            build_dev_image
            ;;
        test)
            test_image
            ;;
        push)
            push_image
            ;;
        start)
            start_dev_env
            ;;
        stop)
            log_info "停止开发环境..."
            docker-compose -f docker-compose.dev.yaml down 2>/dev/null || docker stop hunterclaw-dev 2>/dev/null || true
            log_success "开发环境已停止"
            ;;
        clean)
            cleanup
            ;;
        all)
            build_dev_image
            test_image
            push_image
            ;;
        dev)
            build_dev_image
            start_dev_env
            ;;
    esac
    
    print_separator
    log_success "操作完成!"
    print_separator
}

# 运行主函数
main "$@"
