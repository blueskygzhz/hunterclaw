#include <optional>
#include "hunterclaw/memory/memory_index_manager.h"
#include "hunterclaw/memory/chunking.h"
#include "hunterclaw/memory/session_files.h"
#include "hunterclaw/memory/manager_search.h"
#include "hunterclaw/memory/search_algorithms.h"
#include "hunterclaw/memory/query_expansion.h"
#include "hunterclaw/logging/subsystem_logger.h"
#include <filesystem>
#include <chrono>
#include <algorithm>
#include <sstream>
#include <fstream>
#include <set>

namespace fs = std::filesystem;

namespace hunterclaw {
namespace memory {

// ============================================================================
// 静态成员初始化
// ============================================================================
std::map<std::string, std::shared_ptr<MemoryIndexManager>> MemoryIndexManager::index_cache_;
std::map<std::string, std::shared_ptr<std::future<std::shared_ptr<MemoryIndexManager>>>> MemoryIndexManager::index_cache_pending_;
std::mutex MemoryIndexManager::cache_mutex_;

static auto log = logging::create_subsystem_logger("memory");

// 对齐 manager.ts: SNIPPET_MAX_CHARS = 700
static constexpr int SNIPPET_MAX_CHARS = 700;

// ============================================================================
// 工厂方法 (对齐 manager.ts:135-195 - static async get())
// ============================================================================

core::Result<std::shared_ptr<MemoryIndexManager>> MemoryIndexManager::get(const Params& params) {
    // 对齐 manager.ts:135-195:
    // 1. 计算 cache key
    // 2. 检查 INDEX_CACHE
    // 3. 检查 INDEX_CACHE_PENDING
    // 4. 创建新实例并缓存

    const std::string key = params.agent_id + ":" + params.workspace_dir;

    std::lock_guard<std::mutex> lock(cache_mutex_);

    // 检查已有缓存 (对齐 INDEX_CACHE.get(key))
    auto it = index_cache_.find(key);
    if (it != index_cache_.end() && it->second && !it->second->is_closed()) {
        return core::Result<std::shared_ptr<MemoryIndexManager>>::Ok(it->second);
    }

    // 创建新实例 (对齐 new MemoryIndexManager({...}))
    try {
        auto instance = std::shared_ptr<MemoryIndexManager>(new MemoryIndexManager(params));
        instance->cache_key_ = key;
        instance->initialize();
        index_cache_[key] = instance;
        log->info("[MemoryIndexManager] Created: agent={}, workspace={}",
                  params.agent_id, params.workspace_dir);
        return core::Result<std::shared_ptr<MemoryIndexManager>>::Ok(instance);
    } catch (const std::exception& e) {
        log->error("[MemoryIndexManager] Failed to create: {}", e.what());
        return core::Result<std::shared_ptr<MemoryIndexManager>>::Err(e.what());
    }
}

void MemoryIndexManager::close_all() {
    // 对齐 manager.ts:closeAllMemoryIndexManagers
    std::lock_guard<std::mutex> lock(cache_mutex_);
    for (auto& [key, manager] : index_cache_) {
        if (manager && !manager->is_closed()) {
            try {
                manager->close();
            } catch (const std::exception& e) {
                log->warn("[MemoryIndexManager] Failed to close: {}", e.what());
            }
        }
    }
    index_cache_.clear();
    index_cache_pending_.clear();
}

// ============================================================================
// 构造函数 (对齐 manager.ts:196-260 - private constructor)
// ============================================================================

MemoryIndexManager::MemoryIndexManager(const Params& params) {
    // 初始化基类成员 (MemoryManagerSyncOps 的 protected 成员)
    agent_id_ = params.agent_id;
    workspace_dir_ = params.workspace_dir;
    settings_ = params.settings;

    // 初始化 sources (对齐 manager.ts: new Set(params.settings.sources))
    // 默认包含 memory 和 sessions
    sources_.insert(MemorySource::Memory);
    sources_.insert(MemorySource::Sessions);

    // 初始化 batch 配置 (对齐 manager.ts:resolveBatchConfig)
    batch_.enabled = settings_.batch.enabled;
    batch_.wait = settings_.batch.wait;
    batch_.concurrency = settings_.batch.concurrency;
    batch_.poll_interval_ms = settings_.batch.poll_interval_ms;
    batch_.timeout_ms = settings_.batch.timeout_ms;

    // 初始化 cache 配置
    cache_.enabled = settings_.cache.enabled;
    cache_.max_entries = settings_.cache.max_entries;

    // 初始化 vector 配置 (对齐 manager.ts:vector)
    vector_.enabled = settings_.vector.enabled;
    vector_.available = std::nullopt;  // null until probed
    vector_.extension_path = std::nullopt;
    vector_.load_error = std::nullopt;
    vector_.dims = std::nullopt;

    // 初始化 fts 配置 (对齐 manager.ts:fts)
    fts_.enabled = settings_.hybrid.enabled;
    fts_.available = false;
    fts_.load_error = std::nullopt;

    // 初始化 dirty 状态 (对齐 manager.ts: this.dirty = sources.has("memory"))
    dirty_ = sources_.count(MemorySource::Memory) > 0;
    sessions_dirty_ = false;
}

MemoryIndexManager::~MemoryIndexManager() {
    if (!closed_) {
        close();
    }
}

// ============================================================================
// 初始化 (对齐 manager.ts constructor 中的初始化逻辑)
// ============================================================================

void MemoryIndexManager::initialize() {
    // 1. 确保 workspace 目录存在
    try {
        fs::create_directories(workspace_dir_);
    } catch (...) {}

    // 2. 打开数据库 (对齐 manager.ts: this.db = this.openDatabase())
    auto db_result = open_database();
    if (db_result.is_ok()) {
        db_ = db_result.get();
    } else {
        log->warn("[MemoryIndexManager] Failed to open database: {}", db_result.error);
    }

    // 3. 计算 provider key (对齐 manager.ts: this.providerKey = this.computeProviderKey())
    provider_key_ = compute_provider_key();

    // 4. 启动文件监视器 (对齐 manager.ts: this.ensureWatcher())
    start_memory_watcher();

    // 5. 启动 session 监视器 (对齐 manager.ts: this.ensureSessionListener())
    start_session_watcher();

    // 6. 读取 meta 信息 (对齐 manager.ts: const meta = this.readMeta())
    auto meta = load_index_meta();
    if (meta && meta->vector_dims) {
        vector_.dims = meta->vector_dims;
    }
}

// ============================================================================
// MemorySearchManager 接口实现
// ============================================================================

std::vector<MemorySearchResult> MemoryIndexManager::search(
    const std::string& query,
    const SearchOptions& opts) {

    if (closed_) {
        return {};
    }

    // 对齐 manager.ts:search():
    // 1. warmSession (触发 session-start sync)
    warm_session(opts.session_key);

    // 2. 如果 dirty，触发异步 sync
    if (settings_.sync.watch && (dirty_ || sessions_dirty_)) {
        // 异步触发，不阻塞搜索
        SyncParams sync_params;
        sync_params.reason = "search";
        sync(sync_params);
    }

    // 3. 清理 query
    std::string cleaned = query;
    // trim
    cleaned.erase(0, cleaned.find_first_not_of(" \t\n\r"));
    cleaned.erase(cleaned.find_last_not_of(" \t\n\r") + 1);
    if (cleaned.empty()) {
        return {};
    }

    // 4. 如果没有 provider，使用 FTS-only 模式
    if (!provider_) {
        if (!fts_.enabled || !fts_.available || !db_) {
            log->warn("[MemoryIndexManager] No provider and FTS unavailable");
            return {};
        }
        int max_results = opts.max_results.value_or(settings_.max_results);
        double min_score = opts.min_score.value_or(settings_.min_score);
        int candidate_limit = std::min(200,
            std::max(1, static_cast<int>(max_results * settings_.hybrid.candidate_multiplier)));

        // 对齐 manager.ts: extractKeywords(cleaned)
        // 提取关键词，对每个关键词分别搜索，合并去重
        auto keywords = extract_keywords(cleaned);
        std::vector<std::string> search_terms;
        if (!keywords.empty()) {
            search_terms = keywords;
        } else {
            search_terms.push_back(cleaned);
        }

        // 对齐 manager.ts: Promise.all(searchTerms.map(term => searchKeyword(term, candidates)))
        // 合并去重，保留每个 id 的最高分
        std::map<std::string, KeywordSearchResultWithScore> seen_ids;
        for (const auto& term : search_terms) {
            auto kr = search_keyword_impl(
                *db_, "chunks_fts",
                std::nullopt,  // FTS-only: no model filter
                term,
                candidate_limit,
                SNIPPET_MAX_CHARS,
                {}
            );
            if (kr.is_err()) continue;
            for (const auto& r : kr.get()) {
                auto it = seen_ids.find(r.id);
                if (it == seen_ids.end() || r.score > it->second.score) {
                    seen_ids[r.id] = r;
                }
            }
        }

        // 对齐 manager.ts: toSorted + filter + slice
        std::vector<KeywordSearchResultWithScore> merged_kw;
        merged_kw.reserve(seen_ids.size());
        for (auto& [id, r] : seen_ids) merged_kw.push_back(std::move(r));
        std::sort(merged_kw.begin(), merged_kw.end(),
                  [](const KeywordSearchResultWithScore& a, const KeywordSearchResultWithScore& b) {
                      return a.score > b.score;
                  });

        std::vector<MemorySearchResult> results;
        for (const auto& r : merged_kw) {
            if (r.score < min_score) continue;
            if (static_cast<int>(results.size()) >= max_results) break;
            MemorySearchResult res;
            res.path       = r.path;
            res.start_line = r.start_line;
            res.end_line   = r.end_line;
            res.score      = r.score;
            res.snippet    = r.snippet;
            res.source     = r.source;
            res.citation   = r.path + ":" + std::to_string(r.start_line) + "-" + std::to_string(r.end_line);
            results.push_back(std::move(res));
        }
        return results;
    }

    // 5. 混合搜索 (vector + keyword) (对齐 manager.ts:search 混合搜索逻辑)
    int max_results = opts.max_results.value_or(settings_.max_results);
    double min_score = opts.min_score.value_or(settings_.min_score);
    int candidate_limit = std::min(200,
        std::max(1, static_cast<int>(max_results * settings_.hybrid.candidate_multiplier)));

    // 5a. 获取 query embedding
    std::vector<float> query_vec;
    if (provider_) {
        auto emb_result = provider_->embed_query(cleaned);
        if (emb_result.is_ok()) {
            query_vec = emb_result.get();
        }
    }
    bool has_vector = !query_vec.empty() &&
                      std::any_of(query_vec.begin(), query_vec.end(), [](float v){ return v != 0.0f; });

    // 5b. 关键词搜索 (对齐 manager.ts: hybrid.enabled && fts.enabled && fts.available)
    std::vector<HybridKeywordResult> kw_results;
    if (settings_.hybrid.enabled && fts_.enabled && fts_.available && db_) {
        auto kr = search_keyword_impl(
            *db_, "chunks_fts",
            provider_key_,
            cleaned,
            candidate_limit,
            SNIPPET_MAX_CHARS,
            {}
        );
        if (kr.is_ok()) {
            for (const auto& r : kr.get()) {
                kw_results.push_back({r.id, r.path, r.start_line, r.end_line,
                                      r.source, r.snippet, r.text_score});
            }
        }
    }

    // 5c. 向量搜索 (对齐 manager.ts: hasVector && searchVector)
    std::vector<HybridVectorResult> vec_results;
    if (has_vector && vector_.enabled && db_) {
        auto vr = search_vector_impl(
            *db_, "chunks_vec",
            provider_key_.value_or(""),
            query_vec,
            candidate_limit,
            SNIPPET_MAX_CHARS,
            {}
        );
        if (vr.is_ok()) {
            for (const auto& r : vr.get()) {
                vec_results.push_back({r.id, r.path, r.start_line, r.end_line,
                                       r.source, r.snippet, r.score});
            }
        }
    }

    // 5d. 如果 FTS 不可用，降级为纯向量搜索 (对齐 manager.ts: !hybrid.enabled || !fts.enabled || !fts.available)
    if (!settings_.hybrid.enabled || !fts_.enabled || !fts_.available) {
        std::vector<MemorySearchResult> results;
        for (const auto& r : vec_results) {
            if (r.vector_score < min_score) continue;
            if (static_cast<int>(results.size()) >= max_results) break;
            MemorySearchResult res;
            res.path       = r.path;
            res.start_line = r.start_line;
            res.end_line   = r.end_line;
            res.score      = r.vector_score;
            res.snippet    = r.snippet;
            res.source     = r.source;
            res.citation   = r.path + ":" + std::to_string(r.start_line) + "-" + std::to_string(r.end_line);
            results.push_back(std::move(res));
        }
        return results;
    }

    // 5e. 合并结果 (对齐 mergeHybridResults)
    MMRConfig mmr_cfg;
    mmr_cfg.enabled = settings_.hybrid.mmr.enabled;
    mmr_cfg.lambda  = settings_.hybrid.mmr.lambda;

    TemporalDecayConfig td_cfg;
    td_cfg.enabled        = settings_.hybrid.temporal_decay.enabled;
    td_cfg.half_life_days = settings_.hybrid.temporal_decay.half_life_days;

    auto merged = merge_hybrid_results(
        vec_results, kw_results,
        settings_.hybrid.vector_weight,
        settings_.hybrid.text_weight,
        workspace_dir_,
        mmr_cfg, td_cfg, 0
    );

    // 5f. strict 过滤 (对齐 manager.ts: merged.filter(entry => entry.score >= minScore))
    std::vector<MemorySearchResult> strict_results;
    for (const auto& item : merged) {
        if (item.score < min_score) continue;
        if (static_cast<int>(strict_results.size()) >= max_results) break;
        MemorySearchResult res;
        res.path       = item.path;
        res.start_line = item.start_line;
        res.end_line   = item.end_line;
        res.score      = item.score;
        res.snippet    = item.snippet;
        res.source     = (item.source == "sessions") ? MemorySource::Sessions : MemorySource::Memory;
        res.citation   = item.path + ":" + std::to_string(item.start_line) + "-" + std::to_string(item.end_line);
        strict_results.push_back(std::move(res));
    }

    // 对齐 manager.ts: if (strict.length > 0 || keywordResults.length === 0) return strict
    if (!strict_results.empty() || kw_results.empty()) {
        return strict_results;
    }

    // 5g. relaxedMinScore 逻辑 (对齐 manager.ts:relaxedMinScore)
    // Hybrid defaults can produce keyword-only matches with max score equal to
    // textWeight (e.g. 0.3). If minScore is higher (e.g. 0.35), these exact
    // lexical hits get filtered out even when they are the only relevant results.
    double relaxed_min_score = std::min(min_score, settings_.hybrid.text_weight);

    // Build keyword result key set
    std::set<std::string> kw_keys;
    for (const auto& r : kw_results) {
        kw_keys.insert(r.source + ":" + r.path + ":" + std::to_string(r.start_line) + ":" + std::to_string(r.end_line));
    }

    std::vector<MemorySearchResult> relaxed_results;
    for (const auto& item : merged) {
        std::string key = item.source + ":" + item.path + ":" + std::to_string(item.start_line) + ":" + std::to_string(item.end_line);
        if (kw_keys.count(key) == 0) continue;
        if (item.score < relaxed_min_score) continue;
        if (static_cast<int>(relaxed_results.size()) >= max_results) break;
        MemorySearchResult res;
        res.path       = item.path;
        res.start_line = item.start_line;
        res.end_line   = item.end_line;
        res.score      = item.score;
        res.snippet    = item.snippet;
        res.source     = (item.source == "sessions") ? MemorySource::Sessions : MemorySource::Memory;
        res.citation   = item.path + ":" + std::to_string(item.start_line) + "-" + std::to_string(item.end_line);
        relaxed_results.push_back(std::move(res));
    }
    return relaxed_results;
}

ReadFileResult MemoryIndexManager::read_file(const ReadFileParams& params) {
    // 对齐 manager.ts:readFile()
    ReadFileResult result;
    result.path = params.rel_path;

    std::string raw_path = params.rel_path;
    // trim
    raw_path.erase(0, raw_path.find_first_not_of(" \t\n\r"));
    raw_path.erase(raw_path.find_last_not_of(" \t\n\r") + 1);

    if (raw_path.empty()) {
        return result;
    }

    // 构建绝对路径
    fs::path abs_path;
    if (fs::path(raw_path).is_absolute()) {
        abs_path = fs::path(raw_path);
    } else {
        abs_path = fs::path(workspace_dir_) / raw_path;
    }

    // 安全检查：只允许读取 memory 路径下的 .md 文件
    auto rel = fs::relative(abs_path, workspace_dir_).string();
    // normalize separators
    std::replace(rel.begin(), rel.end(), '\\', '/');

    if (!is_memory_path(rel) || !abs_path.string().ends_with(".md")) {
        return result;
    }

    // 读取文件
    try {
        if (!fs::exists(abs_path)) {
            return result;
        }
        std::ifstream file(abs_path);
        if (!file.is_open()) {
            return result;
        }
        std::string content((std::istreambuf_iterator<char>(file)),
                             std::istreambuf_iterator<char>());

        if (!params.from && !params.lines) {
            result.text = content;
            result.path = rel;
            return result;
        }

        // 按行切片 (对齐 manager.ts:readFile line slicing)
        std::vector<std::string> lines;
        std::istringstream ss(content);
        std::string line;
        while (std::getline(ss, line)) {
            lines.push_back(line);
        }

        int start = std::max(1, params.from.value_or(1));
        int count = std::max(1, params.lines.value_or(static_cast<int>(lines.size())));
        int begin_idx = start - 1;
        int end_idx = std::min(begin_idx + count, static_cast<int>(lines.size()));

        std::ostringstream out;
        for (int i = begin_idx; i < end_idx; ++i) {
            if (i > begin_idx) out << "\n";
            out << lines[i];
        }
        result.text = out.str();
        result.path = rel;
    } catch (const std::exception& e) {
        log->warn("[MemoryIndexManager] readFile failed: {}", e.what());
    }

    return result;
}

MemoryProviderStatus MemoryIndexManager::status() {
    // 对齐 manager.ts:status()
    MemoryProviderStatus s;
    s.backend = "builtin";
    s.workspace_dir = workspace_dir_;
    s.dirty = dirty_ || sessions_dirty_;

    // Provider 信息
    if (provider_) {
        s.provider = provider_->id();
        s.model = provider_->model();
    } else {
        s.provider = "none";
    }
    s.requested_provider = requested_provider_.empty()
        ? std::nullopt : std::make_optional(requested_provider_);

    // Sources
    std::vector<MemorySource> src_vec(sources_.begin(), sources_.end());
    s.sources = src_vec;

    // 从数据库获取统计信息
    if (db_) {
        auto stats = db_->get_stats();
        if (stats.is_ok()) {
            s.files  = stats.get().total_files;
            s.chunks = stats.get().total_chunks;
            // Cache entries
            if (cache_.enabled) {
                MemoryProviderStatus::CacheStatus cache;
                cache.enabled  = cache_.enabled;
                cache.entries  = stats.get().cache_entries;
                cache.max_entries = cache_.max_entries;
                s.cache = cache;
            }
            // Source counts
            auto sc = db_->get_source_counts();
            if (sc.is_ok()) {
                s.source_counts = sc.get();
            }
        }
    }

    // Vector 状态
    MemoryProviderStatus::VectorStatus vec;
    vec.enabled = vector_.enabled;
    vec.available = vector_.available;
    vec.extension_path = vector_.extension_path;
    vec.load_error = vector_.load_error;
    vec.dims = vector_.dims;
    s.vector = vec;

    // FTS 状态
    MemoryProviderStatus::FtsStatus fts;
    fts.enabled = fts_.enabled;
    fts.available = fts_.available;
    fts.error = fts_.load_error;
    s.fts = fts;

    // Cache 状态
    MemoryProviderStatus::CacheStatus cache;
    cache.enabled = cache_.enabled;
    cache.max_entries = cache_.max_entries;
    s.cache = cache;

    // Batch 状态
    MemoryProviderStatus::BatchStatus batch;
    batch.enabled = batch_.enabled;
    batch.failures = batch_failure_count_;
    batch.limit = BATCH_FAILURE_LIMIT;
    batch.wait = batch_.wait;
    batch.concurrency = batch_.concurrency;
    batch.poll_interval_ms = batch_.poll_interval_ms;
    batch.timeout_ms = batch_.timeout_ms;
    batch.last_error = batch_failure_last_error_;
    batch.last_provider = batch_failure_last_provider_;
    s.batch = batch;

    // Fallback 信息
    if (fallback_reason_) {
        MemoryProviderStatus::FallbackInfo fallback;
        fallback.from = fallback_from_.value_or("local");
        fallback.reason = fallback_reason_;
        s.fallback = fallback;
    }

    return s;
}

void MemoryIndexManager::sync(const SyncParams& params) {
    // 对齐 manager.ts:sync()
    if (closed_) {
        return;
    }

    // 如果有 session files，入队处理
    if (params.session_files && !params.session_files->empty()) {
        enqueue_targeted_session_sync(*params.session_files);
        return;
    }

    // 运行同步（带 readonly 恢复）
    run_sync_with_readonly_recovery(params);
}

MemoryEmbeddingProbeResult MemoryIndexManager::probe_embedding_availability() {
    // 对齐 manager.ts:probeEmbeddingAvailability()
    if (!provider_) {
        MemoryEmbeddingProbeResult r;
        r.ok = false;
        r.error = provider_unavailable_reason_.value_or(
            "No embedding provider available (FTS-only mode)"
        );
        return r;
    }

    // TODO: 实际测试 provider (embed ["ping"])
    MemoryEmbeddingProbeResult r;
    r.ok = true;
    return r;
}

bool MemoryIndexManager::probe_vector_availability() {
    // 对齐 manager.ts:probeVectorAvailability()
    if (!provider_) return false;
    if (!vector_.enabled) return false;
    auto result = ensure_vector_ready();
    return result.is_ok() && result.get();
}

void MemoryIndexManager::close() {
    // 对齐 manager.ts:close()
    if (closed_) {
        return;
    }
    closed_ = true;

    // 停止 watchers
    stop_memory_watcher();
    stop_session_watcher();

    // 关闭数据库
    if (db_) {
        // db_->close();
        db_.reset();
    }

    // 从缓存中移除
    std::lock_guard<std::mutex> lock(cache_mutex_);
    index_cache_.erase(cache_key_);

    log->info("[MemoryIndexManager] Closed: agent={}", agent_id_);
}

// ============================================================================
// Session Management
// ============================================================================

void MemoryIndexManager::warm_session(const std::optional<std::string>& session_key) {
    // 对齐 manager.ts:warmSession()
    if (!settings_.sync.watch) {
        return;
    }

    std::string key = session_key ? session_key->substr(0, session_key->find_first_of(" \t\n\r")) : "";
    if (!key.empty() && session_warm_.count(key)) {
        return;
    }

    // 触发异步 sync
    SyncParams sync_params;
    sync_params.reason = "session-start";
    sync(sync_params);

    if (!key.empty()) {
        session_warm_.insert(key);
    }
}

// ============================================================================
// 内部方法
// ============================================================================

core::Result<void> MemoryIndexManager::run_sync(const SyncParams& params) {
    // 对齐 manager.ts:runSync() → 调用 MemoryManagerSyncOps::run_sync
    return MemoryManagerSyncOps::run_sync(params);
}

void MemoryIndexManager::run_sync_with_readonly_recovery(const SyncParams& params) {
    // 对齐 manager.ts:runSyncWithReadonlyRecovery()
    try {
        auto r = MemoryManagerSyncOps::run_sync(params);
        if (!r.is_ok()) {
            log->warn("[MemoryIndexManager] Sync failed: {}", r.error);
        }
    } catch (const std::exception& e) {
        if (!is_readonly_db_error(e) || closed_) {
            log->warn("[MemoryIndexManager] Sync error: {}", e.what());
            return;
        }

        // Readonly recovery: 重新打开数据库
        readonly_recovery_attempts_++;
        readonly_recovery_last_error_ = e.what();
        log->warn("[MemoryIndexManager] Readonly DB detected, reopening: {}", e.what());

        db_.reset();
        try {
            db_ = open_database();
            vector_.available = std::nullopt;
            vector_.load_error = std::nullopt;
        } catch (...) {}

        try {
            auto r = MemoryManagerSyncOps::run_sync(params);
            if (r.is_ok()) {
                readonly_recovery_successes_++;
            } else {
                readonly_recovery_failures_++;
                log->warn("[MemoryIndexManager] Sync failed after recovery: {}", r.error);
            }
        } catch (const std::exception& retry_e) {
            readonly_recovery_failures_++;
            log->warn("[MemoryIndexManager] Sync failed after recovery: {}", retry_e.what());
        }
    }
}

void MemoryIndexManager::enqueue_targeted_session_sync(
    const std::vector<std::string>& session_files) {
    // 对齐 manager.ts:enqueueTargetedSessionSync()
    for (const auto& f : session_files) {
        std::string trimmed = f;
        trimmed.erase(0, trimmed.find_first_not_of(" \t\n\r"));
        trimmed.erase(trimmed.find_last_not_of(" \t\n\r") + 1);
        if (!trimmed.empty()) {
            queued_session_files_.insert(trimmed);
        }
    }
}

bool MemoryIndexManager::is_readonly_db_error(const std::exception& e) const {
    // 对齐 manager.ts:isReadonlyDbError()
    std::string msg = e.what();
    return msg.find("readonly") != std::string::npos ||
           msg.find("read-only") != std::string::npos ||
           msg.find("SQLITE_READONLY") != std::string::npos;
}

// ============================================================================
// MemoryManagerSyncOps 抽象方法实现
// ============================================================================

std::string MemoryIndexManager::compute_provider_key() {
    // 对齐 manager-embedding-ops.ts:computeProviderKey()
    if (!provider_) return "fts-only";
    return provider_->id() + ":" + provider_->model();
}

void MemoryIndexManager::do_sync(const SyncParams& params) {
    // 对齐 manager.ts:sync() → 调用 MemoryManagerSyncOps::run_sync
    auto result = run_sync(params);
    if (!result.is_ok()) {
        log->warn("[MemoryIndexManager] sync failed: {}", result.error);
    }
}

int MemoryIndexManager::get_index_concurrency() {
    return EMBEDDING_INDEX_CONCURRENCY;
}

void MemoryIndexManager::prune_embedding_cache_if_needed() {
    // 对齐 manager-embedding-ops.ts:pruneEmbeddingCacheIfNeeded()
    if (!cache_.enabled || !db_) return;
    // 保留最近 30 天的缓存
    auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    int64_t cutoff = now_ms - 30LL * 24 * 60 * 60 * 1000;
    db_->cleanup_old_cache(cutoff);
}

// ============================================================================
// 全局函数
// ============================================================================

void close_all_memory_index_managers() {
    MemoryIndexManager::close_all();
}

} // namespace memory
} // namespace hunterclaw
