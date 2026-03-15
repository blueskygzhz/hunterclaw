# HunterClaw Memory Module - OpenClaw Alignment

本目录包含严格对齐 OpenClaw memory 模块的 C++ 实现。

## 📁 文件结构

```
include/hunterclaw/memory/
├── types_aligned.h                    # 核心类型定义 (对齐 types.ts)
├── embedding_provider_aligned.h       # Provider接口 (对齐 embeddings*.ts)
├── sqlite_storage_aligned.h           # 存储层 (对齐 sqlite.ts)
├── search_algorithms.h                # 搜索算法 (对齐 hybrid.ts/mmr.ts/temporal-decay.ts)
├── memory_index_manager.h             # 主管理器 (对齐 manager.ts)
└── search_manager_factory.h           # 工厂方法 (对齐 search-manager.ts)

docs/
├── MEMORY_ALIGNMENT_PLAN.md           # 详细对齐计划
├── MEMORY_ALIGNMENT_STATUS.md         # 实现状态跟踪
├── MEMORY_REFERENCE_MAP.md            # 快速对照表
└── MEMORY_ALIGNMENT_SUMMARY.md        # 完成总结
```

## 🎯 对齐目标

**100%对齐 OpenClaw Memory 模块**,包括:
- ✅ API接口完全兼容
- ✅ 数据结构完全一致
- ✅ 算法语义精确对齐
- ✅ 配置默认值一致
- ✅ Schema完全兼容

## 📊 当前状态

| 阶段 | 状态 | 完成度 |
|-----|------|-------|
| Phase 1: 接口定义 | ✅ 完成 | 100% |
| Phase 2: 核心实现 | ⏳ 待完成 | 0% |
| Phase 3: 测试验证 | ⏳ 待完成 | 0% |
| **总体进度** | **🔄 进行中** | **29%** |

## 🚀 快速开始

### 查看对齐情况
```bash
# 查看详细对齐计划
cat MEMORY_ALIGNMENT_PLAN.md

# 查看实现状态
cat MEMORY_ALIGNMENT_STATUS.md

# 查看快速对照表 (OpenClaw ↔ HunterClaw)
cat MEMORY_REFERENCE_MAP.md

# 查看完成总结
cat MEMORY_ALIGNMENT_SUMMARY.md
```

### 理解架构
1. **从类型开始**: 阅读 `types_aligned.h` 了解核心数据结构
2. **看接口定义**: 查看 `MemorySearchManager` 接口 (在 `types_aligned.h`)
3. **看主管理器**: 阅读 `memory_index_manager.h` 了解核心逻辑
4. **看Provider**: 查看 `embedding_provider_aligned.h` 了解Provider架构
5. **看存储层**: 阅读 `sqlite_storage_aligned.h` 了解数据持久化
6. **看搜索算法**: 查看 `search_algorithms.h` 了解检索和排序

### 查找对应关系
需要知道某个 OpenClaw 类/方法在 HunterClaw 中的对应实现?

```bash
# 使用快速对照表
grep "OpenClaw类名或方法名" MEMORY_REFERENCE_MAP.md
```

## 📖 核心概念

### MemorySearchManager (搜索管理器接口)
所有memory后端的统一接口,包括:
- `search()` - 搜索内存
- `read_file()` - 读取文件
- `status()` - 获取状态
- `sync()` - 同步索引
- `probe_*()` - 探测可用性

### MemoryIndexManager (主管理器)
Builtin后端的核心实现,负责:
- Provider管理 (OpenAI/Ollama/Gemini等)
- 索引管理 (sqlite-vec + FTS5)
- 搜索实现 (Hybrid/MMR/Decay)
- 同步操作 (文件监控和索引更新)

### EmbeddingProvider (Embedding提供者)
抽象接口,支持多种Provider:
- OpenAI (含Batch API)
- Ollama (本地)
- Gemini (含Batch API)
- Voyage (含Batch API)
- Mistral
- Remote

### SQLiteVectorStorage (存储层)
基于SQLite的向量存储:
- `chunks_vec` - 向量索引 (sqlite-vec)
- `chunks_fts` - 全文索引 (FTS5)
- `embedding_cache` - Embedding缓存

### 搜索算法
- **Hybrid Search**: RRF合并向量和全文搜索
- **MMR**: 多样性重排
- **Temporal Decay**: 时间衰减

## 🔍 示例用法 (接口)

```cpp
#include "hunterclaw/memory/search_manager_factory.h"

using namespace hunterclaw::memory;

// 获取MemorySearchManager
GetManagerParams params;
params.agent_id = "default";
params.workspace_dir = "/path/to/workspace";
params.settings.provider = "openai";
params.settings.model = "text-embedding-3-small";
params.settings.api_key = "sk-...";

auto result = get_memory_search_manager(params);
if (result.manager) {
    auto manager = result.manager;
    
    // 搜索
    SearchOptions opts;
    opts.max_results = 6;
    opts.min_score = 0.35;
    
    auto results = manager->search("query string", opts);
    
    // 获取状态
    auto status = manager->status();
    
    // 同步索引
    manager->sync();
    
    // 关闭
    manager->close();
}
```

## 📐 配置默认值

与OpenClaw完全一致:
```cpp
max_results = 6              // 最大结果数
min_score = 0.35             // 最小分数阈值
vector_weight = 0.7          // 向量搜索权重
text_weight = 0.3            // 全文搜索权重
mmr.lambda = 0.7             // MMR相关性vs多样性
temporal_decay.half_life_days = 30.0  // 时间衰减半衰期
chunking.tokens = 400        // 分块大小
chunking.overlap = 80        // 分块重叠
```

## 🧪 测试计划

### 单元测试 (TODO)
- `sqlite_storage_test.cc` - 存储层测试
- `hybrid_search_test.cc` - 混合检索测试
- `mmr_test.cc` - MMR算法测试
- `temporal_decay_test.cc` - 时间衰减测试
- `memory_index_manager_test.cc` - 管理器测试

### 集成测试 (TODO)
- 端到端搜索流程
- Provider切换和Fallback
- 性能基准测试

## 🛠️ 下一步工作

### 优先级 P0 (核心功能)
1. [ ] `sqlite_storage_aligned.cc` - 实现存储层
2. [ ] `search_algorithms.cc` - 实现搜索算法
3. [ ] `memory_index_manager.cc` - 实现主管理器
4. [ ] `search_manager_factory.cc` - 实现工厂方法

### 优先级 P1 (Provider)
5. [ ] `providers/openai_provider.cc`
6. [ ] `providers/ollama_provider.cc`
7. [ ] `providers/gemini_provider.cc`

### 优先级 P2 (高级特性)
8. [ ] `qmd_manager.cc` - QMD外部后端
9. [ ] `batch_processor.cc` - 批处理框架

详见 `MEMORY_ALIGNMENT_STATUS.md`

## 📚 参考文档

- **MEMORY_ALIGNMENT_PLAN.md** - 详细的对齐计划和架构设计
- **MEMORY_ALIGNMENT_STATUS.md** - 实现状态和进度跟踪
- **MEMORY_REFERENCE_MAP.md** - OpenClaw ↔ HunterClaw 快速对照表
- **MEMORY_ALIGNMENT_SUMMARY.md** - 阶段性完成总结

## 🤝 贡献指南

### 实现新功能时
1. 查看 `MEMORY_REFERENCE_MAP.md` 找到对应的OpenClaw实现
2. 阅读OpenClaw源码理解逻辑
3. 参考头文件中的接口定义
4. 实现时保持语义对齐
5. 添加单元测试

### 代码风格
- 遵循C++17标准
- 使用 `core::Result<T>` 处理错误
- 使用 `std::optional<T>` 表示可选值
- 方法名使用 `snake_case`
- 类名使用 `PascalCase`
- 成员变量使用 `snake_case_` (带下划线后缀)

## 📞 联系方式

有问题?查看:
1. 头文件中的详细注释
2. 四份对齐文档
3. OpenClaw源码: `/data/workspace/QQMail/openclaw-analysis/src/memory/`

---

**更新日期**: 2026-03-14  
**版本**: Phase 1 Complete  
**状态**: Interface Definition 100% ✅
