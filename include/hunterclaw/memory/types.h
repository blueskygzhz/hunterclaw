#pragma once

#include <string>
#include <vector>
#include <optional>
#include <map>
#include <set>
#include <functional>
#include <cstdint>

namespace hunterclaw {
namespace memory {

// 严格对齐 OpenClaw src/memory/types.ts

// ============================================================================
// MemorySource (types.ts:1)
// ============================================================================
enum class MemorySource {
    Memory,    // "memory"
    Sessions   // "sessions"
};

inline std::string to_string(MemorySource source) {
    return source == MemorySource::Memory ? "memory" : "sessions";
}

// ============================================================================
// MemorySearchResult (types.ts:3-11)
// ============================================================================
struct MemorySearchResult {
    std::string path;                        // 文件路径
    int start_line;                          // 起始行号
    int end_line;                            // 结束行号
    double score;                            // 相似度分数
    std::string snippet;                     // 代码片段
    MemorySource source;                     // 来源
    std::optional<std::string> citation;     // 引用格式
};

// ============================================================================
// MemoryEmbeddingProbeResult (types.ts:13-16)
// ============================================================================
struct MemoryEmbeddingProbeResult {
    bool ok;
    std::optional<std::string> error;
};

// ============================================================================
// MemorySyncProgressUpdate (types.ts:18-22)
// ============================================================================
struct MemorySyncProgressUpdate {
    int completed;
    int total;
    std::optional<std::string> label;
};

// ============================================================================
// MemoryProviderStatus (types.ts:24-59)
// ============================================================================
struct MemoryProviderStatus {
    // Backend类型
    std::string backend;  // "builtin" | "qmd"
    
    // Provider信息
    std::string provider;
    std::optional<std::string> model;
    std::optional<std::string> requested_provider;
    
    // 统计信息
    std::optional<int> files;
    std::optional<int> chunks;
    std::optional<bool> dirty;
    
    // 路径信息
    std::optional<std::string> workspace_dir;
    std::optional<std::string> db_path;
    std::optional<std::vector<std::string>> extra_paths;
    
    // 源统计
    std::optional<std::vector<MemorySource>> sources;
    
    struct SourceCount {
        MemorySource source;
        int files;
        int chunks;
    };
    std::optional<std::vector<SourceCount>> source_counts;
    
    // 缓存状态
    struct CacheStatus {
        bool enabled;
        std::optional<int> entries;
        std::optional<int> max_entries;
    };
    std::optional<CacheStatus> cache;
    
    // FTS状态
    struct FtsStatus {
        bool enabled;
        bool available;
        std::optional<std::string> error;
    };
    std::optional<FtsStatus> fts;
    
    // Fallback信息
    struct FallbackInfo {
        std::string from;
        std::optional<std::string> reason;
    };
    std::optional<FallbackInfo> fallback;
    
    // Vector状态
    struct VectorStatus {
        bool enabled;
        std::optional<bool> available;
        std::optional<std::string> extension_path;
        std::optional<std::string> load_error;
        std::optional<int> dims;
    };
    std::optional<VectorStatus> vector;
    
    // Batch状态
    struct BatchStatus {
        bool enabled;
        int failures;
        int limit;
        bool wait;
        int concurrency;
        int poll_interval_ms;
        int timeout_ms;
        std::optional<std::string> last_error;
        std::optional<std::string> last_provider;
    };
    std::optional<BatchStatus> batch;
    
    // 自定义字段
    std::optional<std::map<std::string, std::string>> custom;
};

// ============================================================================
// SearchOptions
// ============================================================================
struct SearchOptions {
    std::optional<int> max_results;
    std::optional<double> min_score;
    std::optional<std::string> session_key;
};

// ============================================================================
// ReadFileParams
// ============================================================================
struct ReadFileParams {
    std::string rel_path;
    std::optional<int> from;
    std::optional<int> lines;
};

struct ReadFileResult {
    std::string text;
    std::string path;
};

// ============================================================================
// SyncParams
// ============================================================================
struct SyncParams {
    std::optional<std::string> reason;
    std::optional<bool> force;
    std::optional<std::vector<std::string>> session_files;
    std::function<void(const MemorySyncProgressUpdate&)> progress;
};

// ============================================================================
// MemorySearchManager Interface (types.ts:61-81)
// ============================================================================
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
    
    // 同步索引 (可选)
    virtual void sync(const SyncParams& params = {}) {}
    
    // 探测embedding可用性
    virtual MemoryEmbeddingProbeResult probe_embedding_availability() = 0;
    
    // 探测向量搜索可用性
    virtual bool probe_vector_availability() = 0;
    
    // 关闭管理器 (可选)
    virtual void close() {}
};

// ============================================================================
// ChunkMetadata - 内部使用
// ============================================================================
struct ChunkMetadata {
    std::string path;
    MemorySource source;
    std::string model;
    int start_line;
    int end_line;
    int64_t timestamp;
};

// ============================================================================
// EmbeddingProviderResult (对齐 embeddings.ts)
// ============================================================================
struct EmbeddingProviderResult {
    std::string provider;
    std::string model;
    std::optional<std::string> fallback_from;
    std::optional<std::string> fallback_reason;
    std::optional<std::string> unavailable_reason;
};

// ============================================================================
// HybridConfig - 混合检索配置
// ============================================================================
struct HybridConfig {
    bool enabled = true;
    double vector_weight = 0.7;
    double text_weight = 0.3;
    int candidate_multiplier = 4;
    
    struct MMRConfig {
        bool enabled = true;
        double lambda = 0.7;  // 相关性 vs 多样性
    } mmr;
    
    struct TemporalDecayConfig {
        bool enabled = false;
        double half_life_days = 30.0;
    } temporal_decay;
};

// ============================================================================
// ResolvedMemorySearchConfig - 解析后的配置
// ============================================================================
struct ResolvedMemorySearchConfig {
    std::string provider;  // "openai" | "local" | "gemini" | "voyage" | "mistral" | "ollama" | "auto"
    std::optional<std::string> model;
    std::optional<std::string> api_key;
    std::optional<std::string> base_url;
    
    int max_results = 6;
    double min_score = 0.35;
    
    HybridConfig hybrid;
    
    struct ChunkingConfig {
        int tokens = 400;
        int overlap = 80;
    } chunking;
    
    struct CacheConfig {
        bool enabled = true;
        std::optional<int> max_entries;
    } cache;
    
    struct BatchConfig {
        bool enabled = false;
        bool wait = false;
        int concurrency = 5;
        int poll_interval_ms = 1000;
        int timeout_ms = 600000;  // 10分钟
    } batch;
    
    struct VectorConfig {
        bool enabled = true;
    } vector;
    
    struct FtsConfig {
        bool enabled = true;
    } fts;
    
    struct OpenAIConfig {
        std::string base_url = "https://api.openai.com/v1";
    } openai;
    
    struct GeminiConfig {
        std::string base_url = "https://generativelanguage.googleapis.com/v1beta";
        int output_dimensionality = 768;
    } gemini;
    
    // Sync configuration (对齐 manager-sync-ops.ts)
    struct SyncConfig {
        bool watch = true;
        int watch_debounce_ms = 5000;
        std::optional<int> interval_minutes;
        
        struct SessionsConfig {
            int delta_bytes = 0;
            int delta_messages = 0;
        };
        std::optional<SessionsConfig> sessions;
    } sync;
    
    // Store configuration
    struct StoreConfig {
        std::string path;
        VectorConfig vector;
    } store;
    
    // Multimodal configuration
    struct MultimodalConfig {
        bool enabled = false;
        std::set<std::string> modalities;
        int max_file_bytes = 10 * 1024 * 1024;  // 10MB
    } multimodal;
    
    // Extra paths for memory indexing
    std::vector<std::string> extra_paths;
};

// Type alias for convenience
using MemoryConfig = ResolvedMemorySearchConfig;

} // namespace memory
} // namespace hunterclaw
