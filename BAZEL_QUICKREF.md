# 🚀 HunterClaw Bazel 快速参考

## 安装 Bazel

```bash
# 方法 1: 使用 apt (Ubuntu/Debian)
sudo apt install bazel

# 方法 2: 使用 Bazelisk (推荐)
npm install -g @bazel/bazelisk

# 方法 3: 手动下载
wget https://github.com/bazelbuild/bazelisk/releases/latest/download/bazelisk-linux-amd64
chmod +x bazelisk-linux-amd64
sudo mv bazelisk-linux-amd64 /usr/local/bin/bazel
```

## 系统依赖

```bash
sudo apt-get install -y \
    libssl-dev \
    libcurl4-openssl-dev \
    uuid-dev \
    libjsoncpp-dev \
    libboost-system-dev
```

## 常用命令速查

### 构建
```bash
bazel build //...                        # 构建所有目标
bazel build //src:hunterclaw_core       # 构建核心库
bazel build //src:hunterclaw-gateway    # 构建网关服务器
bazel build //:hunterclaw-gateway        # 使用别名构建
```

### 运行
```bash
bazel run //src:hunterclaw-gateway      # 运行网关服务器
bazel run //:hunterclaw-gateway          # 使用别名运行
```

### 测试
```bash
bazel test //...                         # 运行所有测试
bazel test //tests:test_process_integration  # 运行特定测试
bazel test //... --test_output=all       # 显示详细输出
```

### 清理
```bash
bazel clean                              # 清理构建
bazel clean --expunge                    # 完全清理
bazel shutdown                           # 关闭 Bazel 服务器
```

### 查询
```bash
bazel query //...                        # 列出所有目标
bazel query "deps(//src:hunterclaw-gateway)"  # 查看依赖
bazel query --output=graph //src:hunterclaw_core > deps.dot  # 生成依赖图
```

## 构建配置

```bash
# Debug 模式
bazel build --config=debug //src:hunterclaw-gateway

# Release 模式 (默认)
bazel build --config=release //src:hunterclaw-gateway

# Fast Build 模式
bazel build --config=fastbuild //src:hunterclaw-gateway
```

## 并行构建

```bash
bazel build --jobs=8 //...              # 使用 8 个并行任务
```

## 详细日志

```bash
bazel build //... --verbose_failures    # 显示详细错误
bazel build //... --sandbox_debug       # 调试沙箱
bazel build //... -s                    # 显示所有命令
```

## 项目目标速查

| 目标 | 说明 |
|------|------|
| `//src:hunterclaw_core` | 核心共享库 |
| `//src:hunterclaw-gateway` | 网关服务器可执行文件 |
| `//tests:test_process_integration` | 进程集成测试 |
| `//:hunterclaw-gateway` | 网关服务器别名 |
| `//:hunterclaw_core` | 核心库别名 |

## 文件结构

```
HunterClaw/
├── WORKSPACE          # 工作空间配置 (外部依赖)
├── .bazelrc          # 编译选项配置
├── BUILD             # 根构建文件
├── src/BUILD         # 源代码构建文件
├── tests/BUILD       # 测试构建文件
└── third_party/      # 第三方库 BUILD 文件
    ├── json.BUILD
    └── yaml-cpp.BUILD
```

## CMake vs Bazel 命令对比

| 操作 | CMake | Bazel |
|------|-------|-------|
| 配置 | `cmake ..` | 自动 |
| 构建 | `make` | `bazel build //...` |
| 构建特定目标 | `make target` | `bazel build //src:target` |
| 运行 | `./binary` | `bazel run //src:target` |
| 测试 | `make test` | `bazel test //...` |
| 清理 | `make clean` | `bazel clean` |
| 安装 | `make install` | N/A (直接运行) |

## 环境变量

```bash
# 设置 Bazel 缓存位置
export TEST_TMPDIR=/path/to/cache

# 设置 Bazel 输出基础目录
export BAZEL_OUTPUT_BASE=/path/to/output

# 启用编译缓存
export CC="ccache gcc"
export CXX="ccache g++"
```

## 性能优化

```bash
# 使用本地缓存
bazel build --disk_cache=~/.cache/bazel //...

# 使用远程缓存 (如果有)
bazel build --remote_cache=http://cache-server:port //...

# 启用编译数据库生成
bazel run @hedron_compile_commands//:refresh_all
```

## 常见问题快速解决

### Q: 找不到头文件
```bash
# 检查 includes 路径
bazel query --output=build //src:hunterclaw_core | grep includes
```

### Q: 链接错误
```bash
# 查看链接命令
bazel build //src:hunterclaw-gateway -s 2>&1 | grep "^SUBCOMMAND:"
```

### Q: 缓存问题
```bash
# 清除缓存重建
bazel clean --expunge
bazel shutdown
bazel build //...
```

### Q: 依赖问题
```bash
# 查看依赖树
bazel query --output=graph //src:hunterclaw-gateway | dot -Tpng -o deps.png
```

## 一键验证

```bash
# 运行验证脚本
./verify_bazel_build.sh
```

## 更多信息

- 📖 完整文档: `BUILD.bazel.md`
- 📋 迁移报告: `BAZEL_MIGRATION_REPORT.md`
- 🌐 Bazel 官方文档: https://bazel.build/
- 📚 C++ 规则: https://bazel.build/reference/be/c-cpp

---

💡 **提示**: 使用 `bazel help` 查看更多命令帮助
