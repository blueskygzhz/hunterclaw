# HunterClaw ↔ OpenClaw Memory 快速对照表

## 文件对应关系

| OpenClaw (TypeScript) | HunterClaw (C++) | 状态 | 说明 |
|----------------------|------------------|------|------|
| `types.ts` | `types_aligned.h` | ✅ 完成 | 核心类型定义 |
| `manager.ts` | `memory_index_manager.h` | ✅ 完成 | 主管理器 |
| `search-manager.ts` | `search_manager_factory.h` | ✅ 完成 | 工厂和Fallback |
| `embeddings.ts` | `embedding_provider_aligned.h` | ✅ 完成 | Provider接口 |
| `embeddings-openai.ts` | `embedding_provider_aligned.h` | ✅ 完成 | OpenAI实现 |
| `embeddings-ollama.ts` | `embedding_provider_aligned.h` | ✅ 完成 | Ollama实现 |
| `embeddings-gemini.ts` | `embedding_provider_aligned.h` | ✅ 完成 | Gemini实现 |
| `embeddings-voyage.ts` | `embedding_provider_aligned.h` | ✅ 完成 | Voyage实现 |
| `embeddings-mistral.ts` | `embedding_provider_aligned.h` | ✅ 完成 | Mistral实现 |
| `sqlite.ts` | `sqlite_storage_aligned.h` | ✅ 完成 | SQLite存储 |
| `memory-schema.ts` | `sqlite_storage_aligned.h` | ✅ 完成 | Schema定义 |
| `hybrid.ts` | `search_algorithms.h` | ✅ 完成 | 混合检索 |
| `mmr.ts` | `search_algorithms.h` | ✅ 完成 | MMR重排 |
| `temporal-decay.ts` | `search_algorithms.h` | ✅ 完成 | 时间衰减 |
| `manager-search.ts` | `memory_index_manager.h` | ✅ 完成 | 搜索方法 |
| `manager-embedding-ops.ts` | `memory_index_manager.h` | ✅ 完成 | Embedding操作 |
| `manager-sync-ops.ts` | `memory_index_manager.h` | ✅ 完成 | 同步操作 |
| `backend-config.ts` | `search_manager_factory.h` | ✅ 完成 | 后端配置 |
| `qmd-manager.ts` | `qmd_manager.h` | ⏳ TODO | QMD后端 |
| `batch-*.ts` | Provider classes | 🔄 部分 | Batch API |

## 类对应关系

| OpenClaw类 | HunterClaw类 | 文件位置 |
|-----------|-------------|---------|
| `MemorySearchManager` (interface) | `MemorySearchManager` (abstract) | `types_aligned.h` |
| `MemoryIndexManager` | `MemoryIndexManager` | `memory_index_manager.h` |
| `FallbackMemoryManager` | `FallbackMemoryManager` | `search_manager_factory.h` |
| `EmbeddingProvider` (interface) | `EmbeddingProvider` (abstract) | `embedding_provider_aligned.h` |
| `OpenAiEmbeddingClient` | `OpenAiEmbeddingClient` | `embedding_provider_aligned.h` |
| `OllamaEmbeddingClient` | `OllamaEmbeddingClient` | `embedding_provider_aligned.h` |
| `GeminiEmbeddingClient` | `GeminiEmbeddingClient` | `embedding_provider_aligned.h` |
| `VoyageEmbeddingClient` | `VoyageEmbeddingClient` | `embedding_provider_aligned.h` |
| `MistralEmbeddingClient` | `MistralEmbeddingClient` | `embedding_provider_aligned.h` |
| `RemoteEmbeddingProvider` | `RemoteEmbeddingProvider` | `embedding_provider_aligned.h` |
| `SQLiteVectorStorage` | `SQLiteVectorStorage` | `sqlite_storage_aligned.h` |
| `HybridSearchEngine` | `HybridSearchEngine` | `search_algorithms.h` |
| `MMRReranker` | `MMRReranker` | `search_algorithms.h` |
| `TemporalDecayFilter` | `TemporalDecayFilter` | `search_algorithms.h` |
| `SearchPipeline` | `SearchPipeline` | `search_algorithms.h` |
| `QmdMemoryManager` | `QmdMemoryManager` | `qmd_manager.h` (TODO) |

## 方法对应关系

### MemorySearchManager接口

| OpenClaw方法 | HunterClaw方法 | 说明 |
|-------------|---------------|------|
| `search(query, opts?)` | `search(query, opts={})` | 搜索内存 |
| `readFile(params)` | `read_file(params)` | 读取文件 |
| `status()` | `status()` | 获取状态 |
| `sync(params?)` | `sync(params={})` | 同步索引 |
| `probeEmbeddingAvailability()` | `probe_embedding_availability()` | 探测embedding |
| `probeVectorAvailability()` | `probe_vector_availability()` | 探测向量搜索 |
| `close()` | `close()` | 关闭管理器 |

### MemoryIndexManager

| OpenClaw方法 | HunterClaw方法 | 位置 |
|-------------|---------------|------|
| `static async get(params)` | `static get(params)` | `memory_index_manager.h:48` |
| `search(query, opts)` | `search(query, opts)` | `memory_index_manager.h:60` |
| `searchVector(embedding, opts)` | `search_vector(embedding, opts)` | 私有方法 |
| `searchKeyword(query, opts)` | `search_keyword(query, opts)` | 私有方法 |
| `embedQuery(text)` | `embed_query(text)` | 私有方法 |
| `embedBatch(texts)` | `embed_batch(texts)` | 私有方法 |
| `syncMemoryFiles(params)` | `sync_memory_files(params)` | 私有方法 |
| `syncSessionFiles(params)` | `sync_session_files(params)` | 私有方法 |

### SQLiteVectorStorage

| OpenClaw方法 | HunterClaw方法 | 说明 |
|-------------|---------------|------|
| `addChunk(...)` | `add_chunk(...)` | 添加chunk |
| `vectorSearch(...)` | `vector_search(...)` | 向量搜索 |
| `ftsSearch(...)` | `fts_search(...)` | 全文搜索 |
| `getCachedEmbedding(...)` | `get_cached_embedding(...)` | 查询缓存 |
| `cacheEmbedding(...)` | `cache_embedding(...)` | 缓存embedding |
| `getStats()` | `get_stats()` | 获取统计 |

### SearchAlgorithms

| OpenClaw函数 | HunterClaw方法 | 类 |
|-------------|---------------|-----|
| `buildFtsQuery(query)` | `build_fts_query(query)` | `HybridSearchEngine` |
| `mergeHybridResults(...)` | `merge_results(...)` | `HybridSearchEngine` |
| `bm25RankToScore(rank)` | `bm25_rank_to_score(rank)` | `HybridSearchEngine` |
| `mmrRerank(...)` | `rerank(...)` | `MMRReranker` |
| `cosineSimilarity(a, b)` | `cosine_similarity(a, b)` | `MMRReranker` |
| `applyTemporalDecay(...)` | `apply_decay(...)` | `TemporalDecayFilter` |

## 类型对应关系

| OpenClaw类型 | HunterClaw类型 | 文件 |
|-------------|---------------|------|
| `MemorySource` | `enum class MemorySource` | `types_aligned.h:17` |
| `MemorySearchResult` | `struct MemorySearchResult` | `types_aligned.h:28` |
| `MemoryEmbeddingProbeResult` | `struct MemoryEmbeddingProbeResult` | `types_aligned.h:41` |
| `MemorySyncProgressUpdate` | `struct MemorySyncProgressUpdate` | `types_aligned.h:48` |
| `MemoryProviderStatus` | `struct MemoryProviderStatus` | `types_aligned.h:57` |
| `SearchOptions` | `struct SearchOptions` | `types_aligned.h:125` |
| `ReadFileParams` | `struct ReadFileParams` | `types_aligned.h:133` |
| `SyncParams` | `struct SyncParams` | `types_aligned.h:144` |
| `HybridConfig` | `struct HybridConfig` | `types_aligned.h:195` |
| `ResolvedMemorySearchConfig` | `struct ResolvedMemorySearchConfig` | `types_aligned.h:213` |

## 配置默认值对照

| 配置项 | OpenClaw默认值 | HunterClaw默认值 | 位置 |
|-------|--------------|----------------|------|
| `maxResults` | 6 | `max_results = 6` | `types_aligned.h:223` |
| `minScore` | 0.35 | `min_score = 0.35` | `types_aligned.h:224` |
| `hybrid.vectorWeight` | 0.7 | `vector_weight = 0.7` | `types_aligned.h:198` |
| `hybrid.textWeight` | 0.3 | `text_weight = 0.3` | `types_aligned.h:199` |
| `hybrid.mmr.lambda` | 0.7 | `lambda = 0.7` | `types_aligned.h:203` |
| `hybrid.temporalDecay.halfLifeDays` | 30.0 | `half_life_days = 30.0` | `types_aligned.h:208` |
| `chunking.tokens` | 400 | `tokens = 400` | `types_aligned.h:229` |
| `chunking.overlap` | 80 | `overlap = 80` | `types_aligned.h:230` |
| `batch.concurrency` | 5 | `concurrency = 5` | `types_aligned.h:238` |
| `batch.pollIntervalMs` | 1000 | `poll_interval_ms = 1000` | `types_aligned.h:239` |
| `batch.timeoutMs` | 600000 | `timeout_ms = 600000` | `types_aligned.h:240` |

## Schema对照

### chunks_vec表

| OpenClaw字段 | HunterClaw字段 | 类型 | 说明 |
|-------------|---------------|------|------|
| `id` | `id` | TEXT PRIMARY KEY | Chunk ID |
| `path` | `path` | TEXT NOT NULL | 文件路径 |
| `source` | `source` | TEXT NOT NULL | "memory" or "sessions" |
| `model` | `model` | TEXT NOT NULL | Embedding模型 |
| `start_line` | `start_line` | INTEGER NOT NULL | 起始行 |
| `end_line` | `end_line` | INTEGER NOT NULL | 结束行 |
| `content` | `content` | TEXT NOT NULL | 内容 |
| `embedding` | `embedding` | BLOB | Embedding向量 |
| `created_at` | `created_at` | INTEGER NOT NULL | 时间戳 |

### chunks_fts表 (FTS5)

| OpenClaw字段 | HunterClaw字段 | 索引 | 说明 |
|-------------|---------------|------|------|
| `id` | `id` | UNINDEXED | Chunk ID |
| `path` | `path` | indexed | 文件路径 |
| `source` | `source` | indexed | 源类型 |
| `model` | `model` | indexed | 模型 |
| `start_line` | `start_line` | UNINDEXED | 起始行 |
| `end_line` | `end_line` | UNINDEXED | 结束行 |
| `content` | `content` | indexed | 全文索引 |

### embedding_cache表

| OpenClaw字段 | HunterClaw字段 | 类型 | 说明 |
|-------------|---------------|------|------|
| `content_hash` | `content_hash` | TEXT | 内容hash |
| `model` | `model` | TEXT | 模型名 |
| `embedding` | `embedding` | BLOB | Embedding |
| `created_at` | `created_at` | INTEGER | 时间戳 |
| PRIMARY KEY | (content_hash, model) | - | 复合主键 |

## 算法公式对照

### RRF (Reciprocal Rank Fusion)
```
OpenClaw:  score = 1 / (k + rank), k=60
HunterClaw: score = 1 / (k + rank), k=60.0
```

### MMR (Maximal Marginal Relevance)
```
OpenClaw:  score = λ * relevance - (1-λ) * max_similarity
HunterClaw: score = lambda * relevance - (1-lambda) * max_similarity
```

### 时间衰减 (Temporal Decay)
```
OpenClaw:  factor = 2^(-days / half_life)
HunterClaw: factor = 2^(-days / half_life)
```

### 余弦相似度
```
OpenClaw:  cos(a,b) = dot(a,b) / (norm(a) * norm(b))
HunterClaw: cos(a,b) = dot(a,b) / (norm(a) * norm(b))
```

## 命名约定对照

| OpenClaw (TypeScript) | HunterClaw (C++) | 示例 |
|----------------------|------------------|------|
| camelCase | snake_case | `maxResults` → `max_results` |
| interface | abstract class | `interface Manager` → `class Manager` |
| async/await | Result<T> | `async func()` → `Result<T> func()` |
| Promise<T> | Result<T> | `Promise<string>` → `Result<string>` |
| undefined/null | std::optional<T> | `string \| undefined` → `std::optional<string>` |
| Array<T> | std::vector<T> | `Array<Result>` → `std::vector<Result>` |
| Record<K,V> | std::map<K,V> | `Record<string,number>` → `std::map<string,int>` |
| Set<T> | std::set<T> | `Set<string>` → `std::set<string>` |

## 错误处理对照

| OpenClaw | HunterClaw | 说明 |
|---------|-----------|------|
| `throw new Error(msg)` | `return Result<T>::Err(msg)` | 错误返回 |
| `try/catch` | `if (result.is_err())` | 错误检查 |
| `Promise.reject(err)` | `Result<T>::Err(err)` | 异步错误 |
| `undefined` | `std::nullopt` | 空值 |

## Provider配置对照

### OpenAI
```typescript
// OpenClaw
{
  apiKey: string,
  model: "text-embedding-3-small",
  baseUrl: "https://api.openai.com/v1",
  dimensions: 1536
}
```

```cpp
// HunterClaw
OpenAiEmbeddingClient::Config {
  api_key: string,
  model: "text-embedding-3-small",
  base_url: "https://api.openai.com/v1",
  dimensions: 1536
}
```

### Ollama
```typescript
// OpenClaw
{
  model: "nomic-embed-text",
  baseUrl: "http://localhost:11434",
  dimensions: 768
}
```

```cpp
// HunterClaw
OllamaEmbeddingClient::Config {
  model: "nomic-embed-text",
  base_url: "http://localhost:11434",
  dimensions: 768
}
```

## 快速查找指南

需要实现某个OpenClaw功能?使用这个查找表:

| 功能 | 查看文件 |
|-----|---------|
| 类型定义 | `types_aligned.h` |
| 搜索接口 | `types_aligned.h` (MemorySearchManager) |
| 主管理器 | `memory_index_manager.h` |
| Provider | `embedding_provider_aligned.h` |
| 存储层 | `sqlite_storage_aligned.h` |
| 搜索算法 | `search_algorithms.h` |
| 工厂方法 | `search_manager_factory.h` |
| 对齐计划 | `MEMORY_ALIGNMENT_PLAN.md` |
| 实现状态 | `MEMORY_ALIGNMENT_STATUS.md` |

## 更新日志

- 2026-03-14: 完成核心接口定义 (types, provider, storage, algorithms, manager)
- 2026-03-14: 创建对齐计划和状态文档
- 2026-03-14: 创建快速对照表
