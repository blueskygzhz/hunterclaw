#!/bin/bash

# ============================================================================
# HunterClaw 开发环境一键部署脚本
# ============================================================================
# 自动检测环境并选择合适的部署方式
# ============================================================================

set -e

# 颜色定义
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
CYAN='\033[0;36m'
NC='\033[0m'

log_info() { echo -e "${BLUE}[INFO]${NC} $1"; }
log_success() { echo -e "${GREEN}[✓]${NC} $1"; }
log_warn() { echo -e "${YELLOW}[!]${NC} $1"; }
log_error() { echo -e "${RED}[✗]${NC} $1"; }
log_step() { echo -e "${CYAN}[→]${NC} $1"; }

print_banner() {
    cat << 'EOF'
╔═══════════════════════════════════════════════════════════╗
║                                                           ║
║        🦅 HunterClaw Development Environment 🦅           ║
║                                                           ║
║              快速部署开发环境                              ║
║                                                           ║
╚═══════════════════════════════════════════════════════════╝
EOF
}

print_separator() {
    echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
}

# 检查 Docker 是否可用
check_docker() {
    if command -v docker &> /dev/null && docker ps &> /dev/null 2>&1; then
        return 0
    else
        return 1
    fi
}

# 显示菜单
show_menu() {
    print_banner
    echo ""
    print_separator
    log_info "请选择部署方式:"
    print_separator
    echo ""
    echo "  1) 🐳 使用 Docker 构建开发镜像 (推荐)"
    echo "  2) 📦 打包项目 (用于在其他机器上部署)"
    echo "  3) 🚀 启动开发环境 (需要已构建镜像)"
    echo "  4) 🔨 本地编译 (不使用 Docker)"
    echo "  5) 📊 查看状态"
    echo "  6) 🧹 清理环境"
    echo "  0) 退出"
    echo ""
    print_separator
}

# Docker 构建
docker_build() {
    print_separator
    log_step "检查 Docker 环境..."
    print_separator
    
    if ! check_docker; then
        log_error "Docker 不可用或未安装"
        log_info "请先安装 Docker: https://docs.docker.com/get-docker/"
        return 1
    fi
    
    log_success "Docker 环境正常"
    
    print_separator
    log_step "开始构建开发镜像..."
    print_separator
    
    ./deploy/build_dev_docker.sh build
    
    print_separator
    log_success "镜像构建完成！"
    print_separator
    
    echo ""
    log_info "下一步操作:"
    echo "  • 启动开发环境: ./deploy/quickstart.sh (选项 3)"
    echo "  • 推送到仓库: ./deploy/build_dev_docker.sh push"
    echo ""
}

# 打包项目
package_project() {
    print_separator
    log_step "打包项目..."
    print_separator
    
    ./deploy/package_dev_env.sh
    
    echo ""
    log_info "打包文件位置: dist/"
    ls -lh dist/*.tar.gz 2>/dev/null | tail -1
    echo ""
}

# 启动开发环境
start_dev_env() {
    print_separator
    log_step "启动开发环境..."
    print_separator
    
    if ! check_docker; then
        log_error "Docker 不可用"
        return 1
    fi
    
    # 检查镜像是否存在
    if ! docker images | grep -q "hunterclaw.*dev"; then
        log_warn "未找到开发镜像"
        log_info "请先构建镜像 (选项 1)"
        return 1
    fi
    
    ./deploy/build_dev_docker.sh start
    
    print_separator
    log_success "开发环境已启动！"
    print_separator
    
    echo ""
    log_info "访问方式:"
    echo "  • 进入容器: docker exec -it hunterclaw-dev bash"
    echo "  • Gateway: http://localhost:8080"
    echo "  • Health: http://localhost:8081/health"
    echo "  • Metrics: http://localhost:9090"
    echo ""
    
    read -p "是否现在进入容器? (y/n) " -n 1 -r
    echo
    if [[ $REPLY =~ ^[Yy]$ ]]; then
        docker exec -it hunterclaw-dev bash
    fi
}

# 本地编译
local_build() {
    print_separator
    log_step "本地编译..."
    print_separator
    
    if [ ! -f "deploy/build_minimal.sh" ]; then
        log_error "构建脚本不存在"
        return 1
    fi
    
    ./deploy/build_minimal.sh
    
    print_separator
    log_success "编译完成！"
    print_separator
    
    echo ""
    log_info "生成的文件:"
    ls -lh build/hunterclaw-gateway build/*.so 2>/dev/null | awk '{printf "  %s  %s\n", $9, $5}'
    echo ""
}

# 查看状态
show_status() {
    print_separator
    log_step "系统状态"
    print_separator
    echo ""
    
    # Docker 状态
    if check_docker; then
        log_success "Docker: 已安装且运行中"
        
        # 检查镜像
        if docker images | grep -q "hunterclaw.*dev"; then
            log_success "开发镜像: 已构建"
            docker images | grep "hunterclaw.*dev" | head -1
        else
            log_warn "开发镜像: 未构建"
        fi
        
        # 检查容器
        echo ""
        if docker ps -a | grep -q "hunterclaw-dev"; then
            log_info "容器状态:"
            docker ps -a | grep -E "CONTAINER|hunterclaw-dev"
        else
            log_warn "容器: 未创建"
        fi
    else
        log_warn "Docker: 未安装或未运行"
    fi
    
    echo ""
    
    # 本地编译状态
    if [ -d "build" ] && [ -f "build/hunterclaw-gateway" ]; then
        log_success "本地编译: 已完成"
        ls -lh build/hunterclaw-gateway | awk '{printf "  Gateway: %s (%s)\n", $9, $5}'
    else
        log_warn "本地编译: 未完成"
    fi
    
    echo ""
    
    # 打包文件
    if [ -d "dist" ] && ls dist/*.tar.gz &>/dev/null; then
        log_success "打包文件: 已生成"
        ls -lh dist/*.tar.gz | tail -1 | awk '{printf "  %s (%s)\n", $9, $5}'
    else
        log_warn "打包文件: 未生成"
    fi
    
    echo ""
}

# 清理环境
cleanup() {
    print_separator
    log_step "清理环境..."
    print_separator
    
    echo ""
    log_warn "此操作将清理以下内容:"
    echo "  • Docker 容器和卷"
    echo "  • 本地编译产物"
    echo "  • 打包文件"
    echo ""
    
    read -p "确认清理? (y/n) " -n 1 -r
    echo
    
    if [[ ! $REPLY =~ ^[Yy]$ ]]; then
        log_info "取消清理"
        return 0
    fi
    
    # 清理 Docker
    if check_docker; then
        log_info "停止并删除容器..."
        docker-compose -f docker-compose.dev.yaml down -v 2>/dev/null || true
        docker stop hunterclaw-dev 2>/dev/null || true
        docker rm hunterclaw-dev 2>/dev/null || true
        
        read -p "是否删除镜像? (y/n) " -n 1 -r
        echo
        if [[ $REPLY =~ ^[Yy]$ ]]; then
            log_info "删除镜像..."
            docker images | grep "hunterclaw.*dev" | awk '{print $3}' | xargs -r docker rmi -f || true
        fi
    fi
    
    # 清理本地编译
    log_info "清理编译产物..."
    rm -rf build/ bin/ lib/
    
    # 清理打包文件
    log_info "清理打包文件..."
    rm -rf dist/
    
    log_success "清理完成！"
}

# 主函数
main() {
    cd "$(dirname "${BASH_SOURCE[0]}")/.."
    
    while true; do
        show_menu
        read -p "请选择 [0-6]: " choice
        echo ""
        
        case $choice in
            1)
                docker_build
                ;;
            2)
                package_project
                ;;
            3)
                start_dev_env
                ;;
            4)
                local_build
                ;;
            5)
                show_status
                ;;
            6)
                cleanup
                ;;
            0)
                log_info "退出"
                exit 0
                ;;
            *)
                log_error "无效选择"
                ;;
        esac
        
        echo ""
        read -p "按 Enter 继续..." dummy
        clear
    done
}

# 如果有参数，直接执行命令
if [ $# -gt 0 ]; then
    case $1 in
        build)
            docker_build
            ;;
        package)
            package_project
            ;;
        start)
            start_dev_env
            ;;
        compile)
            local_build
            ;;
        status)
            show_status
            ;;
        clean)
            cleanup
            ;;
        *)
            log_error "未知命令: $1"
            echo "可用命令: build, package, start, compile, status, clean"
            exit 1
            ;;
    esac
else
    main
fi
