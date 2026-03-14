# 📋 HunterClaw Bazel 构建系统文件索引

## 核心配置文件

| 文件 | 大小 | 说明 |
|------|------|------|
| `WORKSPACE` | 2.9K | Bazel 工作空间配置，定义外部依赖 (GoogleTest, Boost, nlohmann/json, yaml-cpp) |
| `.bazelrc` | 1.8K | 编译器选项和构建配置 (Debug/Release/FastBuild 模式) |
| `.bazelignore` | - | 指定 Bazel 忽略的目录 (build/, cmake/, docs/ 等) |
| `BUILD` | 961B | 根目录构建文件，定义顶层目标和别名 |
| `verify_bazel_build.sh` | 2.2K | 自动化构建验证脚本 (可执行) |

## 构建文件 (BUILD)

| 文件 | 说明 |
|------|------|
| `./BUILD` | 根目录：定义顶层别名和导出头文件 |
| `./src/BUILD` | 源代码：定义 hunterclaw_core 库和 hunterclaw-gateway 可执行文件 |
| `./tests/BUILD` | 测试：定义 test_process_integration 测试目标 |
| `./third_party/json.BUILD` | 第三方：nlohmann/json 库的构建配置 |
| `./third_party/yaml-cpp.BUILD` | 第三方：yaml-cpp 库的构建配置 |

## 文档文件

| 文件 | 行数 | 说明 |
|------|------|------|
| `BUILD.bazel.md` | ~300 | 📖 **完整使用指南** - Bazel 安装、构建命令、配置说明、常见问题 |
| `BAZEL_MIGRATION_REPORT.md` | ~400 | 📋 **迁移报告** - 详细的迁移过程、文件映射、特性对比 |
| `BAZEL_QUICKREF.md` | ~250 | ⚡ **快速参考** - 常用命令速查表、CMake vs Bazel 对比 |
| `CMAKE_TO_BAZEL_COMPLETE.md` | ~200 | ✅ **完成总结** - 迁移状态、快速开始、故障排查 |

## 文件树结构

```
HunterClaw/
├── WORKSPACE                    # ← Bazel 工作空间
├── .bazelrc                     # ← Bazel 配置
├── .bazelignore                 # ← 忽略列表
├── BUILD                        # ← 根构建文件
├── verify_bazel_build.sh        # ← 验证脚本 (可执行)
│
├── src/
│   ├── BUILD                    # ← 源代码构建
│   ├── core/                    # C++ 源文件
│   ├── gateway/
│   ├── tools/
│   └── ...
│
├── tests/
│   ├── BUILD                    # ← 测试构建
│   └── test_process_integration.cc
│
├── third_party/
│   ├── json.BUILD               # ← nlohmann/json
│   └── yaml-cpp.BUILD           # ← yaml-cpp
│
├── include/
│   └── hunterclaw/              # 头文件
│
├── external/
│   ├── json/                    # nlohmann/json 源码
│   ├── yaml-cpp/                # yaml-cpp 源码
│   └── subprocess.h             # subprocess 头文件
│
└── 文档/
    ├── BUILD.bazel.md           # ← 使用指南
    ├── BAZEL_MIGRATION_REPORT.md
    ├── BAZEL_QUICKREF.md
    └── CMAKE_TO_BAZEL_COMPLETE.md
```

## 构建目标清单

### 库目标
```bash
//src:hunterclaw_core          # 核心共享库 (.so)
//:hunterclaw_core              # 别名
```

### 可执行文件目标
```bash
//src:hunterclaw-gateway        # 网关服务器
//:hunterclaw-gateway           # 别名
```

### 测试目标
```bash
//tests:test_process_integration  # 进程集成测试
//:test_process_integration       # 别名
```

### 依赖目标
```bash
//:hunterclaw_headers          # 所有头文件
//:subprocess                  # subprocess.h
@nlohmann_json//:json         # nlohmann/json 库
@yaml_cpp//:yaml-cpp          # yaml-cpp 库
@com_google_googletest//:gtest  # GoogleTest
```

## 快速命令参考

### 验证安装
```bash
./verify_bazel_build.sh
```

### 构建
```bash
bazel build //...                        # 构建所有
bazel build //src:hunterclaw_core       # 构建核心库
bazel build //src:hunterclaw-gateway    # 构建网关
```

### 运行
```bash
bazel run //src:hunterclaw-gateway      # 运行网关服务器
```

### 测试
```bash
bazel test //...                         # 运行所有测试
bazel test //tests:test_process_integration  # 运行特定测试
```

### 清理
```bash
bazel clean                              # 清理构建
bazel clean --expunge                    # 完全清理
```

## 配置模式

| 模式 | 命令 | 说明 |
|------|------|------|
| Debug | `--config=debug` | 调试符号, -O0, -DDEBUG |
| Release | `--config=release` | 优化 -O3, -DNDEBUG (默认) |
| FastBuild | `--config=fastbuild` | 快速开发模式 |

## 系统依赖

### 必需
- Bazel 或 Bazelisk
- GCC/Clang (C++17 支持)
- libssl-dev
- libcurl4-openssl-dev
- uuid-dev
- libjsoncpp-dev

### 可选
- libboost-system-dev
- ccache (编译缓存)

## 文件用途速查

| 你想... | 查看这个文件 |
|---------|-------------|
| 快速开始使用 Bazel | `CMAKE_TO_BAZEL_COMPLETE.md` |
| 学习所有 Bazel 功能 | `BUILD.bazel.md` |
| 查找常用命令 | `BAZEL_QUICKREF.md` |
| 了解迁移详情 | `BAZEL_MIGRATION_REPORT.md` |
| 验证构建是否正常 | 运行 `./verify_bazel_build.sh` |
| 添加新的源文件 | 编辑 `src/BUILD` |
| 添加新的测试 | 编辑 `tests/BUILD` |
| 修改编译选项 | 编辑 `.bazelrc` |
| 添加外部依赖 | 编辑 `WORKSPACE` |
| 查看所有文件 | 本文件 (INDEX) |

## 与 CMake 的共存

- ✅ CMake 文件保持不变 (`CMakeLists.txt`)
- ✅ 两个构建系统可以共存
- ✅ 可以根据需要选择使用哪个
- ✅ 推荐新项目使用 Bazel

## 总文件数统计

| 类型 | 数量 |
|------|------|
| 核心配置 | 5 个 |
| BUILD 文件 | 5 个 |
| 文档文件 | 4 个 |
| 总计 | 14 个 |

## 总代码量统计

| 类型 | 代码量 |
|------|--------|
| 配置文件 | ~200 行 |
| BUILD 文件 | ~300 行 |
| 文档 | ~1200 行 |
| 脚本 | ~100 行 |
| 总计 | ~1800 行 |

## 下载位置

所有文件位于：`/data/workspace/QQMail/HunterClaw/`

## 版本信息

- **创建日期**: 2026-03-14
- **Bazel 目标版本**: 6.0+
- **C++ 标准**: C++17
- **构建系统**: Bazel + CMake (共存)

---

📌 **提示**: 这是一个索引文件，帮助你快速定位到所需的文档和文件。

🚀 **开始使用**: 运行 `./verify_bazel_build.sh` 进行快速验证！
