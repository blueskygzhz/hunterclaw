#include <optional>
#include "hunterclaw/memory/manager_embedding_ops.h"
#include "hunterclaw/memory/sqlite_storage.h"
#include "hunterclaw/memory/embedding_provider.h"
#include "hunterclaw/memory/chunking.h"
#include <algorithm>
#include <sstream>
#include <cmath>
#include <thread>
#include <chrono>
#include <random>
#include <regex>
#include <fstream>
#include <set>
#include <cstring>

// 严格对齐 OpenClaw src/memory/manager-embedding-ops.ts
// 实现完整的批量 embedding、缓存和重试逻辑

namespace hunterclaw {
namespace memory {

// ============================================================================
// Utility Functions
// ============================================================================

// Vector to Blob (对齐 manager-embedding-ops.ts:44-45)
static std::vector<uint8_t> vector_to_blob(const std::vector<float>& embedding) {
    std::vector<uint8_t> blob(embedding.size() * sizeof(float));
    std::memcpy(blob.data(), embedding.data(), blob.size());
    return blob;
}

// ============================================================================
// MemoryManagerEmbeddingOps Implementation
// (alias: ManagerEmbeddingOps = MemoryManagerEmbeddingOps)
// ============================================================================

// ============================================================================
// Embedding Batching (对齐 manager-embedding-ops.ts:55-83)
// ============================================================================

std::vector<std::vector<MemoryChunk>>
ManagerEmbeddingOps::build_embedding_batches(const std::vector<MemoryChunk>& chunks) {
    std::vector<std::vector<MemoryChunk>> batches;
    std::vector<MemoryChunk> current;
    size_t current_tokens = 0;

    for (const auto& chunk : chunks) {
        size_t estimate = estimate_utf8_bytes(chunk.text);
        bool would_exceed = !current.empty() && 
                          (current_tokens + estimate > EMBEDDING_BATCH_MAX_TOKENS);
        
        if (would_exceed) {
            batches.push_back(current);
            current.clear();
            current_tokens = 0;
        }
        
        // 单个chunk就超大
        if (current.empty() && estimate > EMBEDDING_BATCH_MAX_TOKENS) {
            batches.push_back({chunk});
            continue;
        }
        
        current.push_back(chunk);
        current_tokens += estimate;
    }

    if (!current.empty()) {
        batches.push_back(current);
    }
    
    return batches;
}

// ============================================================================
// Embedding Cache (对齐 manager-embedding-ops.ts:85-183)
// ============================================================================

std::map<std::string, std::vector<float>>
ManagerEmbeddingOps::load_embedding_cache(const std::vector<std::string>& hashes) {
    std::map<std::string, std::vector<float>> out;

    if (!cache_.enabled || !provider_ || !db_) {
        return out;
    }
    if (hashes.empty()) {
        return out;
    }

    // 去重
    std::vector<std::string> unique;
    std::set<std::string> seen;
    for (const auto& hash : hashes) {
        if (hash.empty() || seen.count(hash)) {
            continue;
        }
        seen.insert(hash);
        unique.push_back(hash);
    }

    if (unique.empty()) {
        return out;
    }

    // 对齐 manager-embedding-ops.ts:loadEmbeddingCache
    // SELECT hash, embedding FROM embedding_cache
    //  WHERE provider = ? AND model = ? AND provider_key = ? AND hash IN (...)
    const std::string provider_id  = provider_->get_id();
    const std::string model        = provider_->get_model();
    const std::string provider_key = compute_provider_key();

    constexpr int batch_size = 400;
    for (size_t start = 0; start < unique.size(); start += batch_size) {
        size_t end = std::min(start + batch_size, unique.size());
        std::vector<std::string> batch(unique.begin() + start, unique.begin() + end);

        // 构建 placeholders
        std::string placeholders;
        for (size_t i = 0; i < batch.size(); ++i) {
            if (i > 0) placeholders += ", ";
            placeholders += "?";
        }

        std::string sql =
            "SELECT hash, embedding FROM embedding_cache"
            " WHERE provider = ? AND model = ? AND provider_key = ?"
            " AND hash IN (" + placeholders + ")";

        auto stmt_res = db_->prepare_statement(sql);
        if (stmt_res.is_err()) continue;
        sqlite3_stmt* stmt = stmt_res.get();

        int idx = 1;
        sqlite3_bind_text(stmt, idx++, provider_id.c_str(),  -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, idx++, model.c_str(),        -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, idx++, provider_key.c_str(), -1, SQLITE_TRANSIENT);
        for (const auto& h : batch) {
            sqlite3_bind_text(stmt, idx++, h.c_str(), -1, SQLITE_TRANSIENT);
        }

        while (sqlite3_step(stmt) == SQLITE_ROW) {
            const char* hash_val = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
            const char* emb_text = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
            if (!hash_val || !emb_text) continue;

            // 对齐 openclaw: parseEmbedding(row.embedding) — JSON array TEXT
            std::string emb_str(emb_text);
            std::vector<float> embedding;
            if (!emb_str.empty() && emb_str[0] == '[') {
                std::string s = emb_str.substr(1, emb_str.size() - 2);
                std::istringstream iss(s);
                std::string tok;
                while (std::getline(iss, tok, ',')) {
                    try { embedding.push_back(std::stof(tok)); } catch (...) {}
                }
            }
            if (!embedding.empty()) {
                out[std::string(hash_val)] = std::move(embedding);
            }
        }
        sqlite3_finalize(stmt);
    }

    return out;
}

void ManagerEmbeddingOps::upsert_embedding_cache(
    const std::vector<EmbeddingCacheEntry>& entries
) {
    if (!cache_.enabled || !provider_ || !db_) {
        return;
    }
    if (entries.empty()) {
        return;
    }

    // 对齐 manager-embedding-ops.ts:upsertEmbeddingCache
    // INSERT INTO embedding_cache (provider, model, provider_key, hash, embedding, dims, updated_at)
    //  VALUES (?, ?, ?, ?, ?, ?, ?)
    //  ON CONFLICT(...) DO UPDATE SET embedding=..., dims=..., updated_at=...
    const int64_t now = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()
    ).count();

    const std::string provider_id  = provider_->get_id();
    const std::string model        = provider_->get_model();
    const std::string provider_key = compute_provider_key();

    const std::string sql =
        "INSERT INTO embedding_cache"
        " (provider, model, provider_key, hash, embedding, dims, updated_at)"
        " VALUES (?, ?, ?, ?, ?, ?, ?)"
        " ON CONFLICT(provider, model, provider_key, hash) DO UPDATE SET"
        "   embedding=excluded.embedding,"
        "   dims=excluded.dims,"
        "   updated_at=excluded.updated_at";

    auto stmt_res = db_->prepare_statement(sql);
    if (stmt_res.is_err()) return;
    sqlite3_stmt* stmt = stmt_res.get();

    for (const auto& entry : entries) {
        const auto& embedding = entry.embedding;

        // 对齐 openclaw: JSON.stringify(embedding)
        std::ostringstream emb_json;
        emb_json << "[";
        for (size_t i = 0; i < embedding.size(); ++i) {
            if (i > 0) emb_json << ",";
            emb_json << embedding[i];
        }
        emb_json << "]";
        std::string emb_str = emb_json.str();

        sqlite3_reset(stmt);
        sqlite3_bind_text(stmt, 1, provider_id.c_str(),  -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 2, model.c_str(),        -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 3, provider_key.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 4, entry.hash.c_str(),   -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 5, emb_str.c_str(),      -1, SQLITE_TRANSIENT);
        sqlite3_bind_int (stmt, 6, static_cast<int>(embedding.size()));
        sqlite3_bind_int64(stmt, 7, now);
        sqlite3_step(stmt);
    }
    sqlite3_finalize(stmt);
}

void ManagerEmbeddingOps::prune_embedding_cache_if_needed() {
    if (!cache_.enabled || !db_) {
        return;
    }

    const int max = cache_.max_entries.value_or(0);
    if (max <= 0) {
        return;
    }

    // 对齐 manager-embedding-ops.ts:pruneEmbeddingCacheIfNeeded
    // SELECT COUNT(*) as c FROM embedding_cache
    auto stmt_res = db_->prepare_statement("SELECT COUNT(*) as c FROM embedding_cache");
    if (stmt_res.is_err()) return;
    sqlite3_stmt* stmt = stmt_res.get();
    int count = 0;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        count = sqlite3_column_int(stmt, 0);
    }
    sqlite3_finalize(stmt);

    if (count <= max) {
        return;
    }

    const int excess = count - max;
    // DELETE FROM embedding_cache WHERE rowid IN (
    //   SELECT rowid FROM embedding_cache ORDER BY updated_at ASC LIMIT ?
    // )
    const std::string sql =
        "DELETE FROM embedding_cache"
        " WHERE rowid IN ("
        "   SELECT rowid FROM embedding_cache"
        "   ORDER BY updated_at ASC"
        "   LIMIT ?"
        " )";

    auto del_res = db_->prepare_statement(sql);
    if (del_res.is_err()) return;
    sqlite3_stmt* del_stmt = del_res.get();
    sqlite3_bind_int(del_stmt, 1, excess);
    sqlite3_step(del_stmt);
    sqlite3_finalize(del_stmt);
}

// ============================================================================
// Core Embedding Logic (对齐 manager-embedding-ops.ts:185-226)
// ============================================================================

core::Result<std::vector<std::vector<float>>>
ManagerEmbeddingOps::embed_chunks_in_batches(const std::vector<MemoryChunk>& chunks) {
    if (chunks.empty()) {
        return core::Result<std::vector<std::vector<float>>>::Ok(std::vector<std::vector<float>>{});
    }

    // 收集缓存 (对齐:189)
    auto cached_result = collect_cached_embeddings(chunks);
    auto embeddings = std::move(cached_result.embeddings);
    const auto& missing = cached_result.missing;

    if (missing.empty()) {
        return core::Result<std::vector<std::vector<float>>>::Ok(embeddings);
    }

    // 提取 missing chunks
    std::vector<MemoryChunk> missing_chunks;
    missing_chunks.reserve(missing.size());
    for (const auto& m : missing) {
        missing_chunks.push_back(m.chunk);
    }

    // 分批处理 (对齐:196)
    auto batches = build_embedding_batches(missing_chunks);
    std::vector<EmbeddingCacheEntry> to_cache;
    
    if (!provider_) {
        return core::Result<std::vector<std::vector<float>>>::Err("Cannot embed batch in FTS-only mode (no embedding provider)");
    }

    size_t cursor = 0;
    for (const auto& batch : batches) {
        // 提取文本
        std::vector<std::string> texts;
        texts.reserve(batch.size());
        for (const auto& chunk : batch) {
            texts.push_back(chunk.text);
        }

        // 调用 provider batch embed (对齐:211-213)
        auto batch_result = embed_batch_with_retry(texts);
        if (!batch_result.is_ok()) {
            return batch_result;
        }
        auto batch_embeddings = batch_result.get();

        // 填充结果 (对齐:214-222)
        for (size_t i = 0; i < batch.size(); ++i) {
            const auto& item = missing[cursor + i];
            const auto& embedding = (i < batch_embeddings.size()) 
                                  ? batch_embeddings[i] 
                                  : std::vector<float>{};
            
            embeddings[item.index] = embedding;
            to_cache.push_back({item.chunk.hash, embedding});
        }
        cursor += batch.size();
    }

    // 更新缓存 (对齐:224)
    upsert_embedding_cache(to_cache);
}

// ============================================================================
// Collect Cached Embeddings (对齐 manager-embedding-ops.ts:288-307)
// ============================================================================

ManagerEmbeddingOps::CachedEmbeddingsResult
ManagerEmbeddingOps::collect_cached_embeddings(const std::vector<MemoryChunk>& chunks) {
    std::vector<std::string> hashes;
    hashes.reserve(chunks.size());
    for (const auto& chunk : chunks) {
        hashes.push_back(chunk.hash);
    }
    
    auto cached = load_embedding_cache(hashes);
    
    std::vector<std::vector<float>> embeddings(chunks.size());
    std::vector<CachedEmbeddingsResult::MissingEntry> missing;

    for (size_t i = 0; i < chunks.size(); ++i) {
        const auto& chunk = chunks[i];
        auto it = cached.find(chunk.hash);
        
        if (it != cached.end() && !it->second.empty()) {
            embeddings[i] = it->second;
        } else {
            missing.push_back({static_cast<int>(i), chunk});
        }
    }

    return {std::move(embeddings), std::move(missing)};
}

// ============================================================================
// Provider Key (对齐 manager-embedding-ops.ts:228-266)
// ============================================================================

std::string ManagerEmbeddingOps::compute_provider_key() {
    // FTS-only mode (对齐:229-232)
    if (!provider_) {
        return hash_text(R"({"provider":"none","model":"fts-only"})");
    }

    const std::string id = provider_->get_id();
    const std::string model = provider_->get_model();

    // OpenAI (对齐:233-245)
    if (id == "openai") {
        std::ostringstream oss;
        oss << R"({"provider":"openai",)"
<< R"("baseUrl":")" << settings_.openai.base_url << R"(",)"
            << R"("model":")" << model << R"(",)"
            << R"("headers":[]})";  // 简化: 实际应该包含headers
        return hash_text(oss.str());
    }

    // Gemini (对齐:246-263)
    if (id == "gemini") {
        std::ostringstream oss;
        oss << R"({"provider":"gemini",)"
            << R"("baseUrl":")" << settings_.gemini.base_url << R"(",)"
            << R"("model":")" << model << R"(",)"
            << R"("outputDimensionality":)" << settings_.gemini.output_dimensionality << ","
            << R"("headers":[]})";
        return hash_text(oss.str());
    }

    // Default (对齐:265)
    std::ostringstream oss;
    oss << R"({"provider":")" << id << R"(",)"
        << R"("model":")" << model << R"("})";
    return hash_text(oss.str());
}

// ============================================================================
// Retry & Timeout (对齐 manager-embedding-ops.ts:524-647)
// ============================================================================

core::Result<std::vector<std::vector<float>>>
ManagerEmbeddingOps::embed_batch_with_retry(const std::vector<std::string>& texts) {
    if (texts.empty()) {
        return core::Result<std::vector<std::vector<float>>>::Ok(std::vector<std::vector<float>>{});
    }
    if (!provider_) {
        return core::Result<std::vector<std::vector<float>>>::Err("Cannot embed batch in FTS-only mode (no embedding provider)");
    }

    int attempt = 0;
    int delay_ms = EMBEDDING_RETRY_BASE_DELAY_MS;

    while (true) {
        try {
            // 对齐 manager-embedding-ops.ts:535-545
            const int timeout_ms = resolve_embedding_timeout("batch");
            
            // Log (简化)
            // log.debug("memory embeddings: batch start", ...);

            // 调用 provider (简化: 实际应该加timeout)
            auto result = provider_->embed_batch(texts);
            if (!result.is_ok()) {
                throw std::runtime_error(result.error);
            }
            
            return result;
            
        } catch (const std::exception& err) {
            std::string message = err.what();
            
            // 对齐 manager-embedding-ops.ts:547-554
            if (!is_retryable_embedding_error(message) || 
                attempt >= EMBEDDING_RETRY_MAX_ATTEMPTS) {
                return core::Result<std::vector<std::vector<float>>>::Err(message);
            }
            
            wait_for_embedding_retry(delay_ms, "retrying");
            delay_ms *= 2;
            ++attempt;
        }
    }
}

core::Result<std::vector<float>>
ManagerEmbeddingOps::embed_query_with_timeout(const std::string& text) {
    if (!provider_) {
        return core::Result<std::vector<float>>::Err("Cannot embed query in FTS-only mode (no embedding provider)");
    }

    // 对齐 manager-embedding-ops.ts:615-626
    const int timeout_ms = resolve_embedding_timeout("query");
    
    // Log (简化)
    // log.debug("memory embeddings: query start", ...);

    // 调用 provider (简化: 实际应该加timeout)
    return provider_->embed_query(text);
}

// ============================================================================
// Utilities
// ============================================================================

bool ManagerEmbeddingOps::is_retryable_embedding_error(const std::string& message) const {
    // 对齐 manager-embedding-ops.ts:601-605
    std::regex pattern(
        R"((rate[_ ]limit|too many requests|429|resource has been exhausted|5\d\d|cloudflare|tokens per day))",
        std::regex::icase
    );
    return std::regex_search(message, pattern);
}

bool ManagerEmbeddingOps::is_batch_timeout_error(const std::string& message) const {
    // 对齐 manager-embedding-ops.ts:698-700
    std::regex pattern(R"(timed out|timeout)", std::regex::icase);
    return std::regex_search(message, pattern);
}

bool ManagerEmbeddingOps::is_structured_input_too_large_error(const std::string& message) const {
    // 对齐 manager-embedding-ops.ts:797-801
    std::regex pattern(
        R"((413|payload too large|request too large|input too large|too many tokens|input limit|request size))",
        std::regex::icase
    );
    return std::regex_search(message, pattern);
}

int ManagerEmbeddingOps::resolve_embedding_timeout(const std::string& kind) const {
    // 对齐 manager-embedding-ops.ts:607-613
    const bool is_local = provider_ && provider_->get_id() == "local";
    
    if (kind == "query") {
        return is_local ? EMBEDDING_QUERY_TIMEOUT_LOCAL_MS 
                       : EMBEDDING_QUERY_TIMEOUT_REMOTE_MS;
    }
    return is_local ? EMBEDDING_BATCH_TIMEOUT_LOCAL_MS 
                   : EMBEDDING_BATCH_TIMEOUT_REMOTE_MS;
}

void ManagerEmbeddingOps::wait_for_embedding_retry(int delay_ms, const std::string& action) {
    // 对齐 manager-embedding-ops.ts:592-599
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_real_distribution<> dis(1.0, 1.2);
    
    const int wait_ms = std::min(
        EMBEDDING_RETRY_MAX_DELAY_MS,
        static_cast<int>(std::round(delay_ms * dis(gen)))
    );
    
    // log.warn("memory embeddings rate limited; {} in {}ms", action, wait_ms);
    std::this_thread::sleep_for(std::chrono::milliseconds(wait_ms));
}

int ManagerEmbeddingOps::get_index_concurrency() {
    // 对齐 manager-embedding-ops.ts:756-758
    return settings_.batch.enabled ? settings_.batch.concurrency 
                                : EMBEDDING_INDEX_CONCURRENCY;
}

core::Result<bool> ManagerEmbeddingOps::ensure_vector_ready(size_t dimensions) {
    // 对齐 manager-embedding-ops.ts:ensureVectorReady
    if (!settings_.vector.enabled || !db_) {
        return core::Result<bool>::Ok(false);
    }
    if (dimensions == 0) {
        return core::Result<bool>::Ok(false);
    }
    // chunks_vec 表在 ensure_schema 中已创建（如果 vec 扩展可用）
    // 这里只需确认 vec 扩展已加载
    return core::Result<bool>::Ok(true);
}

// ============================================================================
// Batch Failure Management (对齐 manager-embedding-ops.ts:649-754)
// ============================================================================

void ManagerEmbeddingOps::reset_batch_failure_count() {
    // 对齐 manager-embedding-ops.ts:663-672
    std::lock_guard<std::mutex> lock(batch_failure_mutex_);
    
    if (batch_failure_count_ > 0) {
        // log.debug("memory embeddings: batch recovered; resetting failure count");
    }
    
    batch_failure_count_ = 0;
    batch_failure_last_error_.clear();
    batch_failure_last_provider_.clear();
}

ManagerEmbeddingOps::BatchFailureResult
ManagerEmbeddingOps::record_batch_failure(
    const std::string& provider,
    const std::string& message,
    int attempts,
    bool force_disable
) {
    // 对齐 manager-embedding-ops.ts:674-696
    std::lock_guard<std::mutex> lock(batch_failure_mutex_);
    
    if (!settings_.batch.enabled) {
        return {true, batch_failure_count_};
    }
    
    const int increment = force_disable ? BATCH_FAILURE_LIMIT 
                                       : std::max(1, attempts);
    batch_failure_count_ += increment;
    batch_failure_last_error_ = message;
    batch_failure_last_provider_ = provider;
    
    const bool disabled = force_disable || 
                         (batch_failure_count_ >= BATCH_FAILURE_LIMIT);
    
    if (disabled) {
        settings_.batch.enabled = false;
    }
    
    return {disabled, batch_failure_count_};
}

// ============================================================================
// File Indexing (对齐 manager-embedding-ops.ts:803-924)
// ============================================================================

void ManagerEmbeddingOps::clear_indexed_file_data(
    const std::string& pathname,
    MemorySource source
) {
    if (!db_) return;
    const std::string source_str = (source == MemorySource::Memory) ? "memory" : "sessions";

    // 对齐 manager-embedding-ops.ts:clearIndexedFileData
    // Vector table
    if (settings_.vector.enabled) {
        const std::string sql_vec =
            "DELETE FROM chunks_vec WHERE id IN"
            " (SELECT id FROM chunks WHERE path = ? AND source = ?)";
        auto s = db_->prepare_statement(sql_vec);
        if (s.is_ok()) {
            sqlite3_bind_text(s.get(), 1, pathname.c_str(),   -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(s.get(), 2, source_str.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_step(s.get());
            sqlite3_finalize(s.get());
        }
    }

    // FTS table
    if (settings_.fts.enabled && fts_available_ && provider_) {
        const std::string sql_fts =
            "DELETE FROM chunks_fts WHERE path = ? AND source = ? AND model = ?";
        auto s = db_->prepare_statement(sql_fts);
        if (s.is_ok()) {
            sqlite3_bind_text(s.get(), 1, pathname.c_str(),              -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(s.get(), 2, source_str.c_str(),            -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(s.get(), 3, provider_->get_model().c_str(),-1, SQLITE_TRANSIENT);
            sqlite3_step(s.get());
            sqlite3_finalize(s.get());
        }
    }

    // Chunks table
    const std::string sql_chunks = "DELETE FROM chunks WHERE path = ? AND source = ?";
    auto s = db_->prepare_statement(sql_chunks);
    if (s.is_ok()) {
        sqlite3_bind_text(s.get(), 1, pathname.c_str(),   -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(s.get(), 2, source_str.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_step(s.get());
        sqlite3_finalize(s.get());
    }
}

void ManagerEmbeddingOps::upsert_file_record(
    const MemoryFileEntry& entry,
    MemorySource source
) {
    if (!db_) return;
    const std::string source_str = (source == MemorySource::Memory) ? "memory" : "sessions";

    // 对齐 manager-embedding-ops.ts:upsertFileRecord
    const std::string sql =
        "INSERT INTO files (path, source, hash, mtime, size) VALUES (?, ?, ?, ?, ?)"
        " ON CONFLICT(path) DO UPDATE SET"
        "   source=excluded.source,"
        "   hash=excluded.hash,"
        "   mtime=excluded.mtime,"
        "   size=excluded.size";

    auto s = db_->prepare_statement(sql);
    if (s.is_err()) return;
    sqlite3_bind_text (s.get(), 1, entry.path.c_str(),   -1, SQLITE_TRANSIENT);
    sqlite3_bind_text (s.get(), 2, source_str.c_str(),   -1, SQLITE_TRANSIENT);
    sqlite3_bind_text (s.get(), 3, entry.hash.c_str(),   -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(s.get(), 4, entry.mtime_ms);
    sqlite3_bind_int64(s.get(), 5, static_cast<int64_t>(entry.size));
    sqlite3_step(s.get());
    sqlite3_finalize(s.get());
}

void ManagerEmbeddingOps::delete_file_record(
    const std::string& pathname,
    MemorySource source
) {
    if (!db_) return;
    const std::string source_str = (source == MemorySource::Memory) ? "memory" : "sessions";

    // 对齐 manager-embedding-ops.ts:deleteFileRecord
    const std::string sql = "DELETE FROM files WHERE path = ? AND source = ?";
    auto s = db_->prepare_statement(sql);
    if (s.is_err()) return;
    sqlite3_bind_text(s.get(), 1, pathname.c_str(),   -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(s.get(), 2, source_str.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_step(s.get());
    sqlite3_finalize(s.get());
}

core::Result<void> ManagerEmbeddingOps::index_file(
    const MemoryFileEntry& entry,
    MemorySource source,
    const std::optional<std::string>& content
) {
    // 对齐 manager-embedding-ops.ts:indexFile

    // FTS-only mode: skip (对齐:807-814)
    if (!provider_ || !db_) {
        return core::Result<void>::Ok();
    }

    // 读取内容 (对齐: content ?? fs.readFile)
    std::string file_content;
    if (content.has_value()) {
        file_content = *content;
    } else {
        std::ifstream f(entry.abs_path);
        if (!f.is_open()) {
            return core::Result<void>::Err("Cannot open file: " + entry.abs_path);
        }
        file_content = std::string((std::istreambuf_iterator<char>(f)),
                                    std::istreambuf_iterator<char>());
    }

    // 分块 (对齐 openclaw internal.ts:chunkMarkdown)
    ChunkingConfig chunking_cfg;
    chunking_cfg.tokens  = settings_.chunking.tokens;
    chunking_cfg.overlap = settings_.chunking.overlap;
    auto chunks = chunk_markdown(file_content, chunking_cfg);

    // 过滤空 chunks
    chunks.erase(
        std::remove_if(chunks.begin(), chunks.end(),
            [](const MemoryChunk& c) {
                return c.text.find_first_not_of(" \t\n\r") == std::string::npos;
            }),
        chunks.end()
    );

    if (chunks.empty()) {
        clear_indexed_file_data(entry.path, source);
        upsert_file_record(entry, source);
        return core::Result<void>::Ok();
    }

    // Batch embedding (对齐:840-844)
    core::Result<std::vector<std::vector<float>>> embeddings_result;
    if (settings_.batch.enabled) {
        embeddings_result = embed_chunks_with_batch(chunks, entry, source);
    } else {
        embeddings_result = embed_chunks_in_batches(chunks);
    }

    if (!embeddings_result.is_ok()) {
        return core::Result<void>::Err(embeddings_result.error);
    }
    auto embeddings = embeddings_result.get();

    // Ensure vector ready (对齐:865-866)
    bool vector_ready = false;
    for (const auto& emb : embeddings) {
        if (!emb.empty()) {
            auto r = ensure_vector_ready(emb.size());
            vector_ready = r.is_ok() && r.get();
            break;
        }
    }

    const int64_t now = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()
    ).count();

    // 清理旧数据 (对齐:868)
    clear_indexed_file_data(entry.path, source);

    const std::string source_str = (source == MemorySource::Memory) ? "memory" : "sessions";

    // 对齐 openclaw: INSERT INTO chunks ... ON CONFLICT DO UPDATE
    const std::string sql_chunks =
        "INSERT INTO chunks"
        " (id, path, source, start_line, end_line, hash, model, text, embedding, updated_at)"
        " VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?)"
        " ON CONFLICT(id) DO UPDATE SET"
        "   hash=excluded.hash,"
        "   model=excluded.model,"
        "   text=excluded.text,"
        "   embedding=excluded.embedding,"
        "   updated_at=excluded.updated_at";

    auto stmt_chunks_res = db_->prepare_statement(sql_chunks);
    if (stmt_chunks_res.is_err()) {
        return core::Result<void>::Err("Failed to prepare chunks insert: " + stmt_chunks_res.error);
    }
    sqlite3_stmt* stmt_chunks = stmt_chunks_res.get();

    // 对齐 openclaw: INSERT INTO chunks_vec (id, embedding) VALUES (?, ?)
    sqlite3_stmt* stmt_vec = nullptr;
    if (vector_ready) {
        const std::string sql_vec =
            "INSERT INTO chunks_vec (id, embedding) VALUES (?, ?)";
        auto r = db_->prepare_statement(sql_vec);
        if (r.is_ok()) stmt_vec = r.get();
    }

    // 对齐 openclaw: INSERT INTO chunks_fts (text, id, path, source, model, start_line, end_line)
    sqlite3_stmt* stmt_fts = nullptr;
    if (settings_.fts.enabled && fts_available_) {
        const std::string sql_fts =
            "INSERT INTO chunks_fts"
            " (text, id, path, source, model, start_line, end_line)"
            " VALUES (?, ?, ?, ?, ?, ?, ?)";
        auto r = db_->prepare_statement(sql_fts);
        if (r.is_ok()) stmt_fts = r.get();
    }

    for (size_t i = 0; i < chunks.size(); ++i) {
        const auto& chunk     = chunks[i];
        const auto& embedding = (i < embeddings.size()) ? embeddings[i] : std::vector<float>{};

        // 对齐 openclaw: id = hashText(`${source}:${path}:${startLine}:${endLine}:${hash}:${model}`)
        std::ostringstream id_oss;
        id_oss << source_str << ":" << entry.path << ":"
               << chunk.start_line << ":" << chunk.end_line << ":"
               << chunk.hash << ":" << provider_->get_model();
        const std::string id = hash_text(id_oss.str());

        // 对齐 openclaw: JSON.stringify(embedding)
        std::ostringstream emb_json;
        emb_json << "[";
        for (size_t j = 0; j < embedding.size(); ++j) {
            if (j > 0) emb_json << ",";
            emb_json << embedding[j];
        }
        emb_json << "]";
        std::string emb_str = emb_json.str();

        // Insert into chunks
        sqlite3_reset(stmt_chunks);
        sqlite3_bind_text (stmt_chunks, 1, id.c_str(),                       -1, SQLITE_TRANSIENT);
        sqlite3_bind_text (stmt_chunks, 2, entry.path.c_str(),               -1, SQLITE_TRANSIENT);
        sqlite3_bind_text (stmt_chunks, 3, source_str.c_str(),               -1, SQLITE_TRANSIENT);
        sqlite3_bind_int  (stmt_chunks, 4, chunk.start_line);
        sqlite3_bind_int  (stmt_chunks, 5, chunk.end_line);
        sqlite3_bind_text (stmt_chunks, 6, chunk.hash.c_str(),               -1, SQLITE_TRANSIENT);
        sqlite3_bind_text (stmt_chunks, 7, provider_->get_model().c_str(),   -1, SQLITE_TRANSIENT);
        sqlite3_bind_text (stmt_chunks, 8, chunk.text.c_str(),               -1, SQLITE_TRANSIENT);
        sqlite3_bind_text (stmt_chunks, 9, emb_str.c_str(),                  -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(stmt_chunks, 10, now);
        sqlite3_step(stmt_chunks);

        // Insert into chunks_vec (对齐 openclaw: vectorToBlob)
        if (stmt_vec && !embedding.empty()) {
            // First delete existing (对齐 openclaw: db.prepare("DELETE FROM chunks_vec WHERE id = ?").run(id))
            auto del_res = db_->prepare_statement("DELETE FROM chunks_vec WHERE id = ?");
            if (del_res.is_ok()) {
                sqlite3_bind_text(del_res.get(), 1, id.c_str(), -1, SQLITE_TRANSIENT);
                sqlite3_step(del_res.get());
                sqlite3_finalize(del_res.get());
            }
            // Insert blob (Float32Array buffer)
            std::vector<uint8_t> blob(embedding.size() * sizeof(float));
            std::memcpy(blob.data(), embedding.data(), blob.size());
            sqlite3_reset(stmt_vec);
            sqlite3_bind_text(stmt_vec, 1, id.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_blob(stmt_vec, 2, blob.data(), static_cast<int>(blob.size()), SQLITE_TRANSIENT);
            sqlite3_step(stmt_vec);
        }

        // Insert into chunks_fts
        if (stmt_fts) {
            sqlite3_reset(stmt_fts);
            sqlite3_bind_text(stmt_fts, 1, chunk.text.c_str(),              -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(stmt_fts, 2, id.c_str(),                      -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(stmt_fts, 3, entry.path.c_str(),              -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(stmt_fts, 4, source_str.c_str(),              -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(stmt_fts, 5, provider_->get_model().c_str(),  -1, SQLITE_TRANSIENT);
            sqlite3_bind_int (stmt_fts, 6, chunk.start_line);
            sqlite3_bind_int (stmt_fts, 7, chunk.end_line);
            sqlite3_step(stmt_fts);
        }
    }

    sqlite3_finalize(stmt_chunks);
    if (stmt_vec) sqlite3_finalize(stmt_vec);
    if (stmt_fts) sqlite3_finalize(stmt_fts);

    // 更新文件记录 (对齐:923)
    upsert_file_record(entry, source);

    return core::Result<void>::Ok();
}

// ============================================================================
// Provider Batch API (简化实现 - 对齐 manager-embedding-ops.ts:442-522)
// ============================================================================

core::Result<std::vector<std::vector<float>>>
ManagerEmbeddingOps::embed_chunks_with_batch(
    const std::vector<MemoryChunk>& chunks,
    const MemoryFileEntry& entry,
    MemorySource source
) {
    // 对齐 manager-embedding-ops.ts:268-286
    if (!provider_) {
        return embed_chunks_in_batches(chunks);
    }
    
    const std::string id = provider_->get_id();
    
    if (id == "openai") {
        return embed_chunks_with_openai_batch(chunks, entry, source);
    }
    if (id == "gemini") {
        return embed_chunks_with_gemini_batch(chunks, entry, source);
    }
    if (id == "voyage") {
        return embed_chunks_with_voyage_batch(chunks, entry, source);
    }
    
    return embed_chunks_in_batches(chunks);
}

core::Result<std::vector<std::vector<float>>>
ManagerEmbeddingOps::embed_chunks_with_openai_batch(
    const std::vector<MemoryChunk>& chunks,
    const MemoryFileEntry& entry,
    MemorySource source
) {
    // 简化实现: 实际应该调用 OpenAI Batch API
    // 对齐 manager-embedding-ops.ts:465-491
    return embed_chunks_in_batches(chunks);
}

core::Result<std::vector<std::vector<float>>>
ManagerEmbeddingOps::embed_chunks_with_gemini_batch(
    const std::vector<MemoryChunk>& chunks,
    const MemoryFileEntry& entry,
    MemorySource source
) {
    // 简化实现: 实际应该调用 Gemini Batch API
    // 对齐 manager-embedding-ops.ts:493-522
    return embed_chunks_in_batches(chunks);
}

core::Result<std::vector<std::vector<float>>>
ManagerEmbeddingOps::embed_chunks_with_voyage_batch(
    const std::vector<MemoryChunk>& chunks,
    const MemoryFileEntry& entry,
    MemorySource source
) {
    // 简化实现: 实际应该调用 Voyage Batch API
    // 对齐 manager-embedding-ops.ts:442-463
    return embed_chunks_in_batches(chunks);
}

}  // namespace memory
}  // namespace hunterclaw
