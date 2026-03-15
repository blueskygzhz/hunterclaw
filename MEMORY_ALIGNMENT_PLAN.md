# HunterClaw Memory Module - OpenClaw Alignment Plan

## 概述
本文档定义了 HunterClaw memory 模块与 OpenClaw memory 模块的严格对齐方案。

## OpenClaw Memory 架构分析

### 核心组件 (src/memory/)
1. **manager.ts** (MemoryIndexManager) - 核心内存管理器
2. **search-manager.ts** - 搜索管理器工厂和Fallback机制
3. **types.ts** - 类型定义
4. **sqlite.ts** - SQLite存储层
5. **embeddings.ts** - Embedding Provider接口和实现
6. **hybrid.ts** - 混合检索 (Vector + BM25)
7. **mmr.ts** - MMR多样性重排
8. **temporal-decay.ts** - 时间衰减
9. **qmd-manager.ts** - QMD外部后端支持
10. **manager-embedding-ops.ts** - Embedding操作
11. **manager-search.ts** - 搜索操作实现
12. **manager-sync-ops.ts** - 同步操作

### Embedding Providers
- embeddings-openai.ts
- embeddings-ollama.ts
- embeddings-gemini.ts
- embeddings-voyage.ts
- embeddings-mistral.ts
- embeddings-remote-*.ts (远程embedding服务)
- node-llama.ts (本地模型支持)

### Batch Processing
- batch-runner.ts
- batch-openai.ts
- batch-gemini.ts
- batch-voyage.ts
- batch-upload.ts
- batch-output.ts

## HunterClaw C++ 对齐架构

### 目录结构
```
include/hunterclaw/memory/
  - types.h                      # 类型定义 (对齐 types.ts)
  - search_manager.h             # 搜索管理器接口 (对齐 search-manager.ts)
  - memory_manager.h             # 核心管理器 (对齐 manager.ts)
  - storage.h                    # 存储层接口
  - sqlite_storage.h             # SQLite实现 (对齐 sqlite.ts)
  - embedding_provider.h         # Provider接口 (对齐 embeddings.ts)
  - hybrid_search.h              # 混合检索 (对齐 hybrid.ts)
  - mmr.h                        # MMR算法 (对齐 mmr.ts)
  - temporal_decay.h             # 时间衰减 (对齐 temporal-decay.ts)
  - qmd_manager.h                # QMD后端 (对齐 qmd-manager.ts)
  - batch_processor.h            # 批处理框架
  
  providers/
    - openai_provider.h
    - ollama_provider.h
    - gemini_provider.h
    - voyage_provider.h
    - mistral_provider.h
    - remote_provider.h
    
src/memory/
  - [对应的.cc实现文件]
```

### 核心类型对齐

#### MemorySearchResult (types.ts → types.h)
```cpp
struct MemorySearchResult {
    std::string path;           // 文件路径
    int start_line;             // 起始行
    int end_line;               // 结束行
    double score;               // 相似度分数
    std::string snippet;        // 代码片段
    MemorySource source;        // "memory" | "sessions"
    std::optional<std::string> citation;
};
```

#### MemorySearchManager (types.ts:61-81 → search_manager.h)
```cpp
class MemorySearchManager {
public:
    virtual ~MemorySearchManager() = default;
    
    // 搜索内存
    virtual std::vector<MemorySearchResult> search(
        const std::string& query,
        const SearchOptions& opts = {}
    ) = 0;
    
    // 读取文件
    virtual ReadFileResult read_file(const ReadFileParams& params) = 0;
    
    // 获取状态
    virtual MemoryProviderStatus status() = 0;
    
    // 同步索引
    virtual void sync(const SyncParams& params = {}) = 0;
    
    // 探测embedding可用性
    virtual MemoryEmbeddingProbeResult probe_embedding_availability() = 0;
    
    // 探测向量搜索可用性
    virtual bool probe_vector_availability() = 0;
    
    // 关闭
    virtual void close() = 0;
};
```

#### MemoryIndexManager (manager.ts:61 → memory_manager.h)
```cpp
class MemoryIndexManager : public MemorySearchManager {
public:
    static std::shared_ptr<MemoryIndexManager> get(const ManagerParams& params);
    
    // 实现 MemorySearchManager 接口
    std::vector<MemorySearchResult> search(...) override;
    ReadFileResult read_file(...) override;
    MemoryProviderStatus status() override;
    void sync(...) override;
    MemoryEmbeddingProbeResult probe_embedding_availability() override;
    bool probe_vector_availability() override;
    void close() override;
    
private:
    // OpenClaw manager.ts:62-133 的对应成员变量
    std::string cache_key_;
    Config cfg_;
    std::string agent_id_;
    std::string workspace_dir_;
    ResolvedMemorySearchConfig settings_;
    std::shared_ptr<EmbeddingProvider> provider_;
    std::string requested_provider_;  // "openai"|"local"|"gemini"|...
    
    // Provider clients
    std::unique_ptr<OpenAiEmbeddingClient> openai_;
    std::unique_ptr<GeminiEmbeddingClient> gemini_;
    std::unique_ptr<VoyageEmbeddingClient> voyage_;
    std::unique_ptr<MistralEmbeddingClient> mistral_;
    std::unique_ptr<OllamaEmbeddingClient> ollama_;
    
    // Batch配置
    struct {
        bool enabled;
        bool wait;
        int concurrency;
        int poll_interval_ms;
        int timeout_ms;
    } batch_;
    
    int batch_failure_count_ = 0;
    std::string batch_failure_last_error_;
    
    // 数据库
    sqlite3* db_;
    
    // 源集合
    std::set<MemorySource> sources_;
    std::string provider_key_;
    
    // 缓存配置
    struct {
        bool enabled;
        std::optional<int> max_entries;
    } cache_;
    
    // 向量配置
    struct {
        bool enabled;
        std::optional<bool> available;
        std::optional<std::string> extension_path;
        std::optional<std::string> load_error;
        std::optional<int> dims;
    } vector_;
    
    // FTS配置
    struct {
        bool enabled;
        bool available;
        std::optional<std::string> load_error;
    } fts_;
    
    // 状态标志
    bool closed_ = false;
    bool dirty_ = false;
    bool sessions_dirty_ = false;
    std::set<std::string> sessions_dirty_files_;
    
    // Helper methods (对齐 manager.ts 的私有方法)
    void ensure_provider();
    void ensure_vector_ready();
    void setup_watchers();
    void handle_file_change(const std::string& path);
    // ... 等等
};
```

### 搜索流程对齐

#### 混合检索 (hybrid.ts → hybrid_search.h/cc)
```cpp
class HybridSearchEngine {
public:
    // BM25文本搜索 (buildFtsQuery)
    std::vector<SearchResult> keyword_search(
        const std::string& query,
        int limit
    );
    
    // 向量搜索 (searchVector in manager-search.ts)
    std::vector<SearchResult> vector_search(
        const std::vector<float>& query_embedding,
        int limit,
        double min_score
    );
    
    // 混合结果合并 (mergeHybridResults)
    std::vector<SearchResult> merge_results(
        const std::vector<SearchResult>& vector_results,
        const std::vector<SearchResult>& keyword_results,
        const HybridConfig& config
    );
    
private:
    // RRF排名融合
    double reciprocal_rank_fusion(
        const std::vector<SearchResult>& results,
        int rank,
        double k = 60.0
    );
};
```

#### MMR算法 (mmr.ts → mmr.h/cc)
```cpp
class MMRReranker {
public:
    // 最大边际相关性重排
    std::vector<SearchResult> rerank(
        const std::vector<SearchResult>& candidates,
        const std::vector<float>& query_embedding,
        const MMRConfig& config
    );
    
private:
    // 计算相似度
    double compute_similarity(
        const std::vector<float>& a,
        const std::vector<float>& b
    );
    
    // 计算多样性惩罚
    double compute_diversity_penalty(
        const SearchResult& candidate,
        const std::vector<SearchResult>& selected
    );
};
```

#### 时间衰减 (temporal-decay.ts → temporal_decay.h/cc)
```cpp
class TemporalDecayFilter {
public:
    // 应用时间衰减
    std::vector<SearchResult> apply_decay(
        const std::vector<SearchResult>& results,
        const TemporalDecayConfig& config
    );
    
private:
    // 计算衰减因子
    double compute_decay_factor(
        int64_t timestamp,
        double half_life_days
    );
};
```

### Embedding Provider架构

#### 基础接口 (embeddings.ts → embedding_provider.h)
```cpp
class EmbeddingProvider {
public:
    virtual ~EmbeddingProvider() = default;
    
    // 单个查询embedding
    virtual std::vector<float> embed_query(const std::string& text) = 0;
    
    // 批量embedding
    virtual std::vector<std::vector<float>> embed_batch(
        const std::vector<std::string>& texts
    ) = 0;
    
    // Provider信息
    virtual std::string id() const = 0;
    virtual std::string model() const = 0;
    virtual int dimensions() const = 0;
    
    // 探测可用性
    virtual bool probe_availability() = 0;
};

// Provider工厂 (createEmbeddingProvider)
class EmbeddingProviderFactory {
public:
    static std::shared_ptr<EmbeddingProvider> create(
        const std::string& provider_type,
        const nlohmann::json& config
    );
};
```

#### OpenAI Provider (embeddings-openai.ts → providers/openai_provider.h)
```cpp
class OpenAiEmbeddingClient : public EmbeddingProvider {
public:
    OpenAiEmbeddingClient(
        const std::string& api_key,
        const std::string& model = "text-embedding-3-small",
        const std::string& base_url = "https://api.openai.com/v1"
    );
    
    std::vector<float> embed_query(const std::string& text) override;
    std::vector<std::vector<float>> embed_batch(...) override;
    
    // Batch API支持
    std::string create_batch_job(const std::vector<std::string>& texts);
    BatchStatus check_batch_status(const std::string& job_id);
    std::vector<std::vector<float>> retrieve_batch_results(const std::string& job_id);
    
private:
    std::string api_key_;
    std::string model_;
    std::string base_url_;
    std::unique_ptr<httplib::Client> http_client_;
};
```

### SQLite存储对齐

#### Schema (memory-schema.ts → schema.sql)
```sql
-- 对齐 OpenClaw 的schema定义
CREATE TABLE IF NOT EXISTS chunks_vec (
    id TEXT PRIMARY KEY,
    path TEXT NOT NULL,
    source TEXT NOT NULL,
    model TEXT NOT NULL,
    start_line INTEGER NOT NULL,
    end_line INTEGER NOT NULL,
    content TEXT NOT NULL,
    embedding BLOB,
    created_at INTEGER NOT NULL
);

CREATE TABLE IF NOT EXISTS chunks_fts (
    id TEXT PRIMARY KEY,
    path TEXT NOT NULL,
    source TEXT NOT NULL,
    model TEXT NOT NULL,
    start_line INTEGER,
    end_line INTEGER,
    content TEXT NOT NULL
) USING fts5(content, tokenize='porter');

CREATE TABLE IF NOT EXISTS embedding_cache (
    content_hash TEXT PRIMARY KEY,
    embedding BLOB NOT NULL,
    model TEXT NOT NULL,
    created_at INTEGER NOT NULL
);
```

#### SQLite存储类 (sqlite.ts → sqlite_storage.h)
```cpp
class SQLiteVectorStorage : public MemoryStorage {
public:
    static std::shared_ptr<SQLiteVectorStorage> create(
        const std::string& db_path,
        int dimensions = 1536
    );
    
    // 添加chunk
    std::string add_chunk(
        const std::string& path,
        const std::string& content,
        const std::vector<float>& embedding,
        const ChunkMetadata& metadata
    ) override;
    
    // 向量搜索 (使用sqlite-vec)
    std::vector<SearchResult> vector_search(
        const std::vector<float>& query_embedding,
        int limit,
        double min_score
    ) override;
    
    // FTS全文搜索 (使用FTS5)
    std::vector<SearchResult> fts_search(
        const std::string& query,
        int limit
    ) override;
    
    // 缓存操作
    std::optional<std::vector<float>> get_cached_embedding(
        const std::string& content_hash,
        const std::string& model
    );
    
    void cache_embedding(
        const std::string& content_hash,
        const std::vector<float>& embedding,
        const std::string& model
    );
    
private:
    sqlite3* db_;
    int dimensions_;
    bool fts_available_;
    bool vec_available_;
    
    void ensure_schema();
    void load_sqlite_vec_extension();
    bool try_enable_fts5();
    std::string serialize_embedding(const std::vector<float>& emb);
    std::vector<float> deserialize_embedding(const std::string& blob);
};
```

### QMD后端对齐 (qmd-manager.ts → qmd_manager.h)

```cpp
class QmdMemoryManager : public MemorySearchManager {
public:
    static std::shared_ptr<QmdMemoryManager> create(const QmdParams& params);
    
    // 实现MemorySearchManager接口
    std::vector<MemorySearchResult> search(...) override;
    // ... 其他接口
    
private:
    // QMD进程管理
    std::unique_ptr<subprocess::Popen> qmd_process_;
    
    // IPC通信
    void send_command(const nlohmann::json& cmd);
    nlohmann::json receive_response();
    
    // QMD命令封装
    void qmd_search(const std::string& query, const SearchOptions& opts);
    void qmd_sync(const SyncParams& params);
};
```

### Batch处理对齐

#### Batch Runner (batch-runner.ts → batch_processor.h)
```cpp
class BatchProcessor {
public:
    // 创建批处理任务
    std::string create_batch(
        const std::vector<std::string>& texts,
        const std::string& provider
    );
    
    // 轮询批处理状态
    BatchStatus poll_batch(
        const std::string& batch_id,
        int poll_interval_ms,
        int timeout_ms
    );
    
    // 获取结果
    std::vector<std::vector<float>> get_batch_results(
        const std::string& batch_id
    );
    
private:
    std::map<std::string, std::shared_ptr<EmbeddingProvider>> providers_;
};
```

## 实现优先级

### Phase 1: 核心基础 (对齐90%)
1. ✅ types.h - 类型定义
2. ✅ embedding_provider.h - Provider接口
3. ✅ storage.h + sqlite_storage.h - 存储层
4. ✅ memory_manager.h - 核心管理器框架
5. ✅ search_manager.h - 搜索管理器接口

### Phase 2: 搜索算法 (对齐hybrid+mmr+temporal)
1. ✅ hybrid_search.h/cc - 混合检索
2. ✅ mmr.h/cc - MMR重排
3. ✅ temporal_decay.h/cc - 时间衰减
4. manager-search.cc - 搜索实现

### Phase 3: Provider实现
1. providers/openai_provider.h/cc
2. providers/ollama_provider.h/cc
3. providers/gemini_provider.h/cc
4. providers/voyage_provider.h/cc
5. providers/mistral_provider.h/cc

### Phase 4: 高级特性
1. batch_processor.h/cc - 批处理
2. qmd_manager.h/cc - QMD后端
3. manager-embedding-ops.cc - Embedding操作
4. manager-sync-ops.cc - 同步操作

### Phase 5: 测试和优化
1. 单元测试覆盖
2. 性能测试
3. 集成测试

## 关键对齐点

### 1. API兼容性
- 所有公共接口签名对齐OpenClaw TypeScript接口
- 参数命名保持一致 (snake_case vs camelCase)
- 返回类型语义对齐

### 2. 行为一致性
- 搜索算法结果一致
- 排序规则一致
- 错误处理一致
- Fallback机制一致

### 3. 配置对齐
- 配置参数名称一致
- 默认值一致
- 配置验证规则一致

### 4. 数据库Schema
- 表结构完全一致
- 索引策略一致
- 迁移策略一致

### 5. 性能特性
- 缓存策略对齐
- 批处理策略对齐
- 并发控制对齐

## 测试验证

### 单元测试
- 每个类对应OpenClaw的.test.ts文件
- 测试用例覆盖相同场景
- 边界条件测试

### 集成测试
- 端到端搜索流程
- Provider切换测试
- Fallback机制测试
- QMD后端测试

### 性能测试
- 搜索延迟对标
- 批处理吞吐量对标
- 内存使用对标

## 文档对齐
- API文档对齐
- 配置文档对齐
- 架构文档对齐
- 示例代码对齐
