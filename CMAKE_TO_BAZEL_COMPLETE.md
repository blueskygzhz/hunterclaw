# ✅ HunterClaw CMake → Bazel 迁移完成

## 🎉 迁移状态：完成

已成功将 HunterClaw 项目从 CMake 构建系统迁移到 Bazel！

## 📁 已创建的文件

### 核心配置文件 (5 个)
- ✅ `WORKSPACE` - Bazel 工作空间配置
- ✅ `.bazelrc` - 编译选项和构建配置
- ✅ `.bazelignore` - 忽略的目录列表
- ✅ `BUILD` - 根目录构建文件
- ✅ `verify_bazel_build.sh` - 自动化验证脚本 (可执行)

### 构建文件 (4 个)
- ✅ `src/BUILD` - 源代码构建配置
- ✅ `tests/BUILD` - 测试构建配置
- ✅ `third_party/json.BUILD` - nlohmann/json 构建配置
- ✅ `third_party/yaml-cpp.BUILD` - yaml-cpp 构建配置

### 文档文件 (3 个)
- ✅ `BUILD.bazel.md` - 完整使用指南 (2500+ 字)
- ✅ `BAZEL_MIGRATION_REPORT.md` - 详细迁移报告
- ✅ `BAZEL_QUICKREF.md` - 快速参考卡片

## 🚀 快速开始

### 1. 安装 Bazel (如果还没有)
```bash
# 推荐使用 Bazelisk
npm install -g @bazel/bazelisk

# 或使用 apt (Ubuntu)
sudo apt install bazel
```

### 2. 安装系统依赖
```bash
sudo apt-get install -y \
    libssl-dev \
    libcurl4-openssl-dev \
    uuid-dev \
    libjsoncpp-dev \
    libboost-system-dev
```

### 3. 验证构建
```bash
cd /data/workspace/QQMail/HunterClaw
./verify_bazel_build.sh
```

### 4. 开始使用
```bash
# 构建所有目标
bazel build //...

# 运行网关服务器
bazel run //src:hunterclaw-gateway

# 运行测试
bazel test //...
```

## 📊 构建目标

| 目标 | 类型 | 说明 |
|------|------|------|
| `//src:hunterclaw_core` | 共享库 | 核心功能库 |
| `//src:hunterclaw-gateway` | 可执行文件 | HTTP 网关服务器 |
| `//tests:test_process_integration` | 测试 | 进程集成测试 |

## 🔧 配置模式

```bash
# Debug 模式 (带调试符号, -O0)
bazel build --config=debug //src:hunterclaw-gateway

# Release 模式 (优化 -O3, 默认)
bazel build --config=release //src:hunterclaw-gateway

# FastBuild 模式 (快速开发)
bazel build --config=fastbuild //src:hunterclaw-gateway
```

## 📈 Bazel 优势

### 性能
- ⚡ **增量编译**: 只重新编译修改的文件
- ⚡ **并行构建**: 自动最大化并行度
- ⚡ **智能缓存**: 本地 + 远程缓存支持

### 可靠性
- 🔒 **确定性构建**: 相同输入 → 相同输出
- 🔒 **沙箱隔离**: 防止构建污染
- 🔒 **精确依赖**: 准确的依赖跟踪

### 可扩展性
- 📈 **大规模项目**: 适合单体仓库
- 📈 **多语言支持**: C++, Python, Java 等
- 📈 **分布式构建**: 支持远程执行

## 🔄 CMake vs Bazel

| 操作 | CMake | Bazel |
|------|-------|-------|
| 配置项目 | `cmake ..` | 自动完成 |
| 构建全部 | `make` | `bazel build //...` |
| 构建目标 | `make target` | `bazel build //src:target` |
| 运行程序 | `./binary` | `bazel run //src:target` |
| 运行测试 | `make test` | `bazel test //...` |
| 清理构建 | `make clean` | `bazel clean` |

## 📝 常用命令

```bash
# 构建
bazel build //...                        # 构建所有
bazel build //src:hunterclaw_core       # 构建核心库
bazel build //src:hunterclaw-gateway    # 构建网关

# 运行
bazel run //src:hunterclaw-gateway      # 运行网关服务器

# 测试
bazel test //...                         # 运行所有测试
bazel test //... --test_output=all       # 显示详细输出

# 查询
bazel query //...                        # 列出所有目标
bazel query "deps(//src:hunterclaw-gateway)"  # 查看依赖

# 清理
bazel clean                              # 清理构建
bazel clean --expunge                    # 完全清理
```

## 📚 文档参考

| 文档 | 内容 |
|------|------|
| `BUILD.bazel.md` | 完整使用指南和最佳实践 |
| `BAZEL_MIGRATION_REPORT.md` | 详细的迁移报告和说明 |
| `BAZEL_QUICKREF.md` | 命令速查和快速参考 |
| 此文件 | 迁移完成总结 |

## ✨ 特性亮点

1. **完全兼容**: 保留了 CMake 配置，两者可共存
2. **自动化脚本**: `verify_bazel_build.sh` 一键验证
3. **详尽文档**: 3 份完整文档覆盖所有场景
4. **清晰结构**: 模块化的 BUILD 文件组织
5. **优化配置**: 预设 Debug/Release/FastBuild 模式

## 🎯 下一步

### 立即可用
- [x] 使用 Bazel 构建项目
- [x] 运行测试验证功能
- [x] 查看文档学习用法

### 后续可选
- [ ] 配置远程缓存提升性能
- [ ] 设置 CI/CD 集成 Bazel
- [ ] 添加更多测试目标
- [ ] 探索分布式构建

## ⚠️ 注意事项

1. **系统依赖**: 需要手动安装 OpenSSL、libcurl 等系统库
2. **Bazel 安装**: 首次使用需要安装 Bazel 或 Bazelisk
3. **CMake 保留**: CMake 文件保持不变，可以继续使用
4. **插件目标**: Feishu/Telegram 插件暂时注释，待实现完成后启用

## 🆘 故障排查

### Bazel 未安装
```bash
npm install -g @bazel/bazelisk
# 或
sudo apt install bazel
```

### 系统依赖缺失
```bash
sudo apt-get install -y libssl-dev libcurl4-openssl-dev uuid-dev libjsoncpp-dev
```

### 构建错误
```bash
bazel clean --expunge
bazel build //... --verbose_failures
```

### 查看详细日志
```bash
bazel build //... -s --sandbox_debug
```

## 📞 支持

- 📖 查看 `BUILD.bazel.md` 获取完整指南
- 📋 查看 `BAZEL_QUICKREF.md` 获取命令速查
- 🌐 访问 [Bazel 官方文档](https://bazel.build/)
- 🔧 运行 `./verify_bazel_build.sh` 自动验证

---

**状态**: ✅ 迁移完成，可以开始使用！  
**构建系统**: CMake (保留) + Bazel (新增)  
**推荐使用**: Bazel (更快、更可靠)

🎉 **Happy Building with Bazel!** 🎉
