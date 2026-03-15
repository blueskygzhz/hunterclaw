#include "hunterclaw/memory/memory_index_manager.h"
#include "hunterclaw/memory/search_algorithms.h"
#include <chrono>
#include <algorithm>
#include <sstream>

namespace hunterclaw {
namespace memory {

namespace {

// Get current time in milliseconds
int64_t now_ms() {
    auto now = std::chrono::system_clock::now();
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch());
    return ms.count();
}

// Generate chunk ID
std::string generate_chunk_id(const std::string& path, int start_line, int end_line, MemorySource source) {
    std::ostringstream oss;
    oss << to_string(source) << ":" << path << ":" << start_line << ":" << end_line;
    return oss.str();
}

} // anonymous namespace

// ============================================================================
// MemoryIndexManager Implementation
// ============================================================================

MemoryIndexManager::MemoryIndexManager(const MemoryIndexManagerConfig& config)
    : config_(config),
      storage_(config.db_path),
      provider_(nullptr),
      closed_(false),
      dirty_(false) {
    
    // Initialize storage
    auto open_res = storage_.open();
    if (!open_res.is_ok()) {
        throw std::runtime_error("Failed to open storage: " + open_res.error_message());
    }
    
    auto schema_res = storage_.ensure_schema();
    if (!schema_res.is_ok()) {
        throw std::runtime_error("Failed to ensure schema: " + schema_res.error_message());
    }
    
    // Load vector extension if configured
    if (config.vector_extension_path.has_value()) {
        auto vec_res = storage_.load_vector_extension(config.vector_extension_path.value());
        if (vec_res.is_ok() && vec_res.value()) {
            vector_available_ = true;
        }
    }
    
    // Initialize embedding provider
    if (config.embedding_provider_options.has_value()) {
        auto provider_res = create_embedding_provider(config.embedding_provider_options.value());
        if (provider_res.is_ok() && provider_res.value().provider) {
            provider_ = std::move(provider_res.value().provider);
        }
    }
}

MemoryIndexManager::~MemoryIndexManager() {
    close();
}

void MemoryIndexManager::close() {
    std::lock_guard<std::mutex> lock(mutex_);
    
    if (closed_) {
        return;
    }
    
    closed_ = true;
    storage_.close();
}

std::string MemoryIndexManager::compute_provider_key() const {
    if (!provider_) {
        return "no-provider";
    }
    return provider_->id() + ":" + provider_->model();
}

Result<std::vector<MemorySearchResult>> MemoryIndexManager::search(
    const std::string& query,
    const SearchOptions& opts) {
    
    std::lock_guard<std::mutex> lock(mutex_);
    
    if (closed_) {
        return Result<std::vector<MemorySearchResult>>::error("Manager is closed");
    }
    
    std::string cleaned = query;
    // Trim whitespace
    cleaned.erase(0, cleaned.find_first_not_of(" \t\n\r"));
    cleaned.erase(cleaned.find_last_not_of(" \t\n\r") + 1);
    
    if (cleaned.empty()) {
        return Result<std::vector<MemorySearchResult>>::ok_value({});
    }
    
    const int max_results = opts.max_results.value_or(config_.max_results);
    const double min_score = opts.min_score.value_or(config_.min_score);
    const int candidates = std::min(200, std::max(1, max_results * 3));
    
    // FTS-only mode if no provider
    if (!provider_) {
        if (!config_.fts_enabled) {
            return Result<std::vector<MemorySearchResult>>::error("No provider and FTS disabled");
        }
        
        std::string fts_query = build_fts_query(cleaned);
        if (fts_query.empty()) {
            return Result<std::vector<MemorySearchResult>>::ok_value({});
        }
        
        auto keyword_res = storage_.search_keyword(fts_query, candidates);
        if (!keyword_res.is_ok()) {
            return Result<std::vector<MemorySearchResult>>::error(keyword_res.error_message());
        }
        
        std::vector<MemorySearchResult> results;
        for (const auto& kr : keyword_res.value()) {
            if (kr.text_score < min_score) continue;
            if (results.size() >= static_cast<size_t>(max_results)) break;
            
            MemorySearchResult result;
            result.path = kr.path;
            result.start_line = kr.start_line;
            result.end_line = kr.end_line;
            result.score = kr.text_score;
            result.snippet = kr.snippet;
            result.source = kr.source;
            
            results.push_back(std::move(result));
        }
        
        return Result<std::vector<MemorySearchResult>>::ok_value(results);
    }
    
    // Hybrid search mode
    
    // 1. Get query embedding
    auto query_emb_res = provider_->embed_query(cleaned);
    if (!query_emb_res.is_ok()) {
        return Result<std::vector<MemorySearchResult>>::error("Failed to embed query: " + query_emb_res.error_message());
    }
    
    auto query_embedding = query_emb_res.value();
    bool has_vector = false;
    for (float v : query_embedding) {
        if (v != 0.0f) {
            has_vector = true;
            break;
        }
    }
    
    // 2. Vector search
    std::vector<VectorSearchResult> vector_results;
    if (has_vector && vector_available_) {
        auto vec_res = storage_.search_vector(query_embedding, candidates, 0.0);
        if (vec_res.is_ok()) {
            vector_results = vec_res.value();
        }
    }
    
    // 3. Keyword search
    std::vector<KeywordSearchResult> keyword_results;
    if (config_.fts_enabled) {
        std::string fts_query = build_fts_query(cleaned);
        if (!fts_query.empty()) {
            auto kw_res = storage_.search_keyword(fts_query, candidates);
            if (kw_res.is_ok()) {
                keyword_results = kw_res.value();
            }
        }
    }
    
    // 4. Merge results with search pipeline
    SearchPipelineParams pipeline_params;
    pipeline_params.vector_results = vector_results;
    pipeline_params.keyword_results = keyword_results;
    pipeline_params.vector_weight = config_.vector_weight;
    pipeline_params.text_weight = config_.text_weight;
    pipeline_params.max_results = max_results;
    pipeline_params.min_score = min_score;
    pipeline_params.workspace_dir = config_.workspace_dir;
    pipeline_params.now_ms = now_ms();
    
    // MMR config
    pipeline_params.mmr_enabled = config_.mmr_enabled;
    pipeline_params.mmr_lambda = config_.mmr_lambda;
    pipeline_params.mmr_top_k = config_.mmr_top_k;
    
    // Temporal decay config
    pipeline_params.temporal_decay_enabled = config_.temporal_decay_enabled;
    pipeline_params.temporal_decay_half_life_days = config_.temporal_decay_half_life_days;
    pipeline_params.temporal_decay_min_score_multiplier = config_.temporal_decay_min_score_multiplier;
    
    // Embedding lookup for MMR
    pipeline_params.get_embedding_func = [this](const std::string& chunk_id) -> std::optional<std::vector<float>> {
        // TODO: Implement chunk ID to embedding lookup
        return std::nullopt;
    };
    
    auto final_results = SearchPipeline::execute(pipeline_params);
    
    return Result<std::vector<MemorySearchResult>>::ok_value(final_results);
}

Result<void> MemoryIndexManager::sync(const SyncOptions& opts) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    if (closed_) {
        return Result<void>::error("Manager is closed");
    }
    
    // TODO: Implement file scanning and indexing
    // 1. Scan workspace_dir for relevant files
    // 2. Chunk files into segments
    // 3. Generate embeddings for each chunk
    // 4. Store chunks with embeddings in database
    
    dirty_ = false;
    
    return Result<void>::ok();
}

Result<MemoryProviderStatus> MemoryIndexManager::get_status() const {
    std::lock_guard<std::mutex> lock(mutex_);
    
    MemoryProviderStatus status;
    status.backend = "builtin";
    
    if (provider_) {
        status.provider = provider_->id();
        status.model = provider_->model();
    } else {
        status.provider = "none";
    }
    
    status.dirty = dirty_;
    status.workspace_dir = config_.workspace_dir;
    status.db_path = config_.db_path;
    
    // Get storage stats
    auto stats_res = storage_.get_stats();
    if (stats_res.is_ok()) {
        const auto& stats = stats_res.value();
        status.files = stats.total_files;
        status.chunks = stats.total_chunks;
        
        if (config_.cache_enabled) {
            MemoryProviderStatus::CacheStatus cache_status;
            cache_status.enabled = true;
            cache_status.entries = stats.cache_entries;
            cache_status.max_entries = config_.cache_max_entries;
            status.cache = cache_status;
        }
    }
    
    // FTS status
    MemoryProviderStatus::FtsStatus fts_status;
    fts_status.enabled = config_.fts_enabled;
    fts_status.available = config_.fts_enabled; // Assume available if enabled
    status.fts = fts_status;
    
    // Vector status
    MemoryProviderStatus::VectorStatus vector_status;
    vector_status.enabled = config_.vector_enabled;
    vector_status.available = vector_available_;
    if (config_.vector_extension_path.has_value()) {
        vector_status.extension_path = config_.vector_extension_path.value();
    }
    status.vector = vector_status;
    
    return Result<MemoryProviderStatus>::ok_value(status);
}

Result<void> MemoryIndexManager::add_file(const std::string& path, MemorySource source) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    if (closed_) {
        return Result<void>::error("Manager is closed");
    }
    
    // TODO: Implement file chunking and embedding
    // 1. Read file content
    // 2. Split into chunks
    // 3. Generate embeddings
    // 4. Store in database
    
    dirty_ = true;
    
    return Result<void>::ok();
}

Result<void> MemoryIndexManager::remove_file(const std::string& path, MemorySource source) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    if (closed_) {
        return Result<void>::error("Manager is closed");
    }
    
    return storage_.delete_chunks_by_path(path, source);
}

Result<void> MemoryIndexManager::clear_all() {
    std::lock_guard<std::mutex> lock(mutex_);
    
    if (closed_) {
        return Result<void>::error("Manager is closed");
    }
    
    // TODO: Implement full database clear
    dirty_ = false;
    
    return Result<void>::ok();
}

Result<MemoryEmbeddingProbeResult> MemoryIndexManager::probe_embedding_provider() const {
    std::lock_guard<std::mutex> lock(mutex_);
    
    if (!provider_) {
        MemoryEmbeddingProbeResult result;
        result.ok = false;
        result.error = "No embedding provider configured";
        return Result<MemoryEmbeddingProbeResult>::ok_value(result);
    }
    
    // Try to embed a test query
    auto test_res = provider_->embed_query("test");
    
    MemoryEmbeddingProbeResult result;
    result.ok = test_res.is_ok();
    if (!result.ok) {
        result.error = test_res.error_message();
    }
    
    return Result<MemoryEmbeddingProbeResult>::ok_value(result);
}

} // namespace memory
} // namespace hunterclaw
