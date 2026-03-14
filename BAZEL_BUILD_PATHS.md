# Bazel 构建输出路径说明

## 标准输出路径

### 符号链接（推荐使用）
```bash
bazel-bin/src/hunterclaw-gateway       # 网关程序
bazel-bin/src/libhunterclaw_core.so    # 核心库
```

### 实际物理路径
```bash
bazel-out/k8-fastbuild/bin/src/hunterclaw-gateway
bazel-out/k8-fastbuild/bin/src/libhunterclaw_core.so
```

## 构建命令

```bash
# 构建所有目标
bazel build //src:hunterclaw-gateway //src:hunterclaw_core

# 构建并运行
bazel run //src:hunterclaw-gateway

# 查看输出
ls -lh bazel-bin/src/
```

## 输出路径说明

| 路径 | 说明 |
|------|------|
| `bazel-bin/` | 构建产物符号链接（推荐使用） |
| `bazel-out/k8-fastbuild/bin/` | 快速构建模式的实际输出 |
| `bazel-out/k8-opt/bin/` | 优化构建模式的输出 |
| `bazel-HunterClaw/` | 执行根目录 |
| `bazel-testlogs/` | 测试日志 |

## 当前环境

- **系统**: GLIBC 2.17
- **工具链**: devtoolset-11 (GCC 11.2.1)
- **Bazel 版本**: 3.7.2
- **构建状态**: 配置就绪，需调整头文件路径

## CMake 构建产物（当前可用）

```
build/hunterclaw-gateway         (178K)
build/libhunterclaw_core.so      (5.6M)
```
