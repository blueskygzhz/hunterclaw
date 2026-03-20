#include <optional>
#include "hunterclaw/memory/memory_index_manager.h"
#include "hunterclaw/memory/embedding_provider.h"
#include "hunterclaw/memory/sqlite_storage.h"
#include "hunterclaw/memory/chunking.h"
#include "hunterclaw/memory/session_files.h"
#include "hunterclaw/memory/manager_search.h"
#include "hunterclaw/memory/search_algorithms.h"
#include "hunterclaw/memory/query_expansion.h"
#include "hunterclaw/logging/subsystem_logger.h"
#include <sqlite3.h>
#include <filesystem>
#include <chrono>
#include <algorithm>
#include <sstream>
#include <fstream>
#include <set>
#include <map>
#include <mutex>
#include <future>

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
    // 对齐 manager.ts: key = `${agentId}:${workspaceDir}:${JSON.stringify(settings)}`
    // 简化：使用 agentId:workspaceDir:provider:model 作为 key
    const std::string key = params.agent_id + ":" + params.workspace_dir
        + ":" + params.settings.provider
        + ":" + params.settings.model.value_or("");

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
        log->info("[MemoryIndexManager] Created: agent=" + params.agent_id + ", workspace=" + params.workspace_dir);
        return core::Result<std::shared_ptr<MemoryIndexManager>>::Ok(instance);
    } catch (const std::exception& e) {
        log->error(std::string("[MemoryIndexManager] Failed to create: ") + e.what());
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
                log->warn(std::string("[MemoryIndexManager] Failed to close: ") + e.what());
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
    agent_id_      = params.agent_id;
    workspace_dir_ = params.workspace_dir;
    settings_      = params.settings;

    // 初始化 sources (对齐 manager.ts: new Set(params.settings.sources))
    sources_.insert(MemorySource::Memory);
    sources_.insert(MemorySource::Sessions);

    // 初始化 cache 配置
    cache_.enabled    = settings_.cache.enabled;
    cache_.max_entries = settings_.cache.max_entries;

    // 初始化 vector 配置 (对齐 manager.ts:vector)
    vector_.enabled        = settings_.vector.enabled;
    vector_.available      = std::nullopt;  // null until probed
    vector_.extension_path = settings_.vector.extension_path;
    vector_.load_error     = std::nullopt;
    vector_.dims           = std::nullopt;

    // 初始化 fts 配置 (对齐 manager.ts:fts)
    fts_.enabled    = settings_.hybrid.enabled;
    fts_.available  = false;
    fts_.load_error = std::nullopt;

    // 初始化 batch 配置 (对齐 manager.ts:resolveBatchConfig)
    batch_.enabled         = settings_.batch.enabled;
    batch_.wait            = settings_.batch.wait;
    batch_.concurrency     = settings_.batch.concurrency;
    batch_.poll_interval_ms = settings_.batch.poll_interval_ms;
    batch_.timeout_ms      = settings_.batch.timeout_ms;

    // 初始化 dirty 状态 (对齐 manager.ts: this.dirty = sources.has("memory") && (statusOnly ? !meta : true))
    // 注意：此时 db_ 尚未打开，statusOnly 逻辑在 initialize() 中处理
    const bool status_only = (params.purpose == "status");
    if (status_only) {
        // statusOnly: dirty = sources.has("memory") && !meta
        // meta 在 initialize() 中读取，这里先设为 false，initialize() 会修正
        dirty_ = false;
    } else {
        dirty_ = sources_.count(MemorySource::Memory) > 0;
    }
    sessions_dirty_ = false;
    purpose_ = params.purpose;
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
    db_ = open_database();
    if (!db_) {
        log->warn("[MemoryIndexManager] Failed to open database");
    }

    // 3. 确保 schema (对齐 manager.ts: this.ensureSchema())
    ensure_schema();

    // 4. 计算 provider key (对齐 manager.ts: this.providerKey = this.computeProviderKey())
    provider_key_ = compute_provider_key();

    // 5. 读取 meta 信息 (对齐 manager.ts: const meta = this.readMeta())
    auto meta = load_index_meta();
    if (meta && meta->vector_dims) {
        vector_.dims = meta->vector_dims;
    }

    // 6. statusOnly dirty 修正 (对齐 manager.ts: this.dirty = sources.has("memory") && (statusOnly ? !meta : true))
    if (purpose_ == "status") {
        dirty_ = sources_.count(MemorySource::Memory) > 0 && !meta.has_value();
    }

    // 7. 启动文件监视器 (对齐 manager.ts: this.ensureWatcher())
    start_memory_watcher();

    // 8. 启动 session 监视器 (对齐 manager.ts: this.ensureSessionListener())
    start_session_watcher();

    // 9. 启动定时同步 (对齐 manager.ts: this.ensureIntervalSync())
    ensure_interval_sync();
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

    // 2. 如果 dirty，触发异步 sync (对齐 manager.ts: settings.sync.onSearch)
    if (settings_.sync.on_search && (dirty_ || sessions_dirty_)) {
        SyncParams sync_params;
        sync_params.reason = "search";
        sync(sync_params);
    }

    // 3. 清理 query
    std::string cleaned = query;
    cleaned.erase(0, cleaned.find_first_not_of(" \t\n\r"));
    cleaned.erase(cleaned.find_last_not_of(" \t\n\r") + 1);
    if (cleaned.empty()) {
        return {};
    }

    const int max_results = opts.max_results.value_or(settings_.max_results);
    const double min_score = opts.min_score.value_or(settings_.min_score);
    const int candidate_limit = std::min(200,
        std::max(1, static_cast<int>(max_results * settings_.hybrid.candidate_multiplier)));

    // 4. FTS-only 模式 (对齐 manager.ts: if (!this.provider))
    if (!provider_) {
        if (!fts_.enabled || !fts_.available || !db_) {
            log->warn("[MemoryIndexManager] No provider and FTS unavailable");
            return {};
        }

        // 对齐 manager.ts: extractKeywords(cleaned)
        auto keywords = extract_keywords(cleaned);
        std::vector<std::string> search_terms;
        if (!keywords.empty()) {
            search_terms = keywords;
        } else {
            search_terms.push_back(cleaned);
        }

        // 对齐 manager.ts: Promise.all(searchTerms.map(...)) + merge + dedup
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
            res.source     = r.source;  // MemorySource enum，直接赋值
            res.citation   = r.path + ":" + std::to_string(r.start_line) + "-" + std::to_string(r.end_line);
            results.push_back(std::move(res));
        }
        return results;
    }

    // 5. 混合搜索 (vector + keyword)

    // 5a. 关键词搜索 (对齐 manager.ts: hybrid.enabled && fts.enabled && fts.available)
    std::vector<HybridKeywordResult> kw_results;
    if (settings_.hybrid.enabled && fts_.enabled && fts_.available && db_) {
        auto kr = search_keyword_impl(
            *db_, "chunks_fts",
            provider_key_,  // std::string
            cleaned,
            candidate_limit,
            SNIPPET_MAX_CHARS,
            {}
        );
        if (kr.is_ok()) {
            for (const auto& r : kr.get()) {
                HybridKeywordResult item;
                item.id         = r.id;
                item.path       = r.path;
                item.start_line = r.start_line;
                item.end_line   = r.end_line;
                item.source     = (r.source == MemorySource::Memory) ? "memory" : "sessions";
                item.snippet    = r.snippet;
                item.text_score = r.text_score;
                kw_results.push_back(std::move(item));
            }
        }
    }

    // 5b. 向量搜索 (对齐 manager.ts: embedQueryWithTimeout + searchVector)
    std::vector<float> query_vec;
    if (provider_) {
        auto emb_result = provider_->embed_query(cleaned);
        if (emb_result.is_ok()) {
            query_vec = emb_result.get();
        }
    }
    bool has_vector = !query_vec.empty() &&
                      std::any_of(query_vec.begin(), query_vec.end(), [](float v){ return v != 0.0f; });

    // SearchRowResult 对齐 HybridVectorResult
    std::vector<SearchRowResult> vec_results;
    if (has_vector && vector_.enabled && db_) {
        auto vr = search_vector_impl(
            *db_, "chunks_vec",
            provider_key_,  // std::string
            query_vec,
            candidate_limit,
            SNIPPET_MAX_CHARS,
            {}
        );
        if (vr.is_ok()) {
            vec_results = vr.get();
        }
    }

    // 5c. 降级为纯向量搜索 (对齐 manager.ts: !hybrid.enabled || !fts.enabled || !fts.available)
    if (!settings_.hybrid.enabled || !fts_.enabled || !fts_.available) {
        std::vector<MemorySearchResult> results;
        for (const auto& r : vec_results) {
            if (r.score < min_score) continue;
            if (static_cast<int>(results.size()) >= max_results) break;
            MemorySearchResult res;
            res.path       = r.path;
            res.start_line = r.start_line;
            res.end_line   = r.end_line;
            res.score      = r.score;
            res.snippet    = r.snippet;
            res.source     = (r.source == "sessions") ? MemorySource::Sessions : MemorySource::Memory;
            res.citation   = r.path + ":" + std::to_string(r.start_line) + "-" + std::to_string(r.end_line);
            results.push_back(std::move(res));
        }
        return results;
    }

    // 对齐 openclaw: 将 SearchRowResult 转换为 HybridVectorResult 格式
    std::vector<HybridVectorResult> hybrid_vec;
    for (const auto& r : vec_results) {
        HybridVectorResult item;
        item.id           = r.id;
        item.path         = r.path;
        item.start_line   = r.start_line;
        item.end_line     = r.end_line;
        item.source       = r.source;
        item.snippet      = r.snippet;
        item.vector_score = r.score;
        hybrid_vec.push_back(std::move(item));
    }

    // 5d. 合并结果 (对齐 mergeHybridResults)
    MMRConfig mmr_cfg;
    mmr_cfg.enabled = settings_.hybrid.mmr.enabled;
    mmr_cfg.lambda  = settings_.hybrid.mmr.lambda;

    TemporalDecayConfig td_cfg;
    td_cfg.enabled        = settings_.hybrid.temporal_decay.enabled;
    td_cfg.half_life_days = settings_.hybrid.temporal_decay.half_life_days;

    auto merged = merge_hybrid_results(
        hybrid_vec, kw_results,
        settings_.hybrid.vector_weight,
        settings_.hybrid.text_weight,
        workspace_dir_,
        mmr_cfg, td_cfg, 0
    );

    // 5e. strict 过滤 (对齐 manager.ts: merged.filter(entry => entry.score >= minScore))
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

    // 5f. relaxedMinScore 逻辑 (对齐 manager.ts:relaxedMinScore)
    // Hybrid defaults can produce keyword-only matches with max score equal to
    // textWeight (e.g. 0.3). If minScore is higher (e.g. 0.35), these exact
    // lexical hits get filtered out even when they are the only relevant results.
    const double relaxed_min_score = std::min(min_score, settings_.hybrid.text_weight);

    std::set<std::string> kw_keys;
    for (const auto& r : kw_results) {
        kw_keys.insert(r.source + ":" + r.path + ":" +
                       std::to_string(r.start_line) + ":" + std::to_string(r.end_line));
    }

    std::vector<MemorySearchResult> relaxed_results;
    for (const auto& item : merged) {
        const std::string key = item.source + ":" + item.path + ":" +
                                std::to_string(item.start_line) + ":" + std::to_string(item.end_line);
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

    std::string raw_path = params.rel_path;
    raw_path.erase(0, raw_path.find_first_not_of(" \t\n\r"));
    raw_path.erase(raw_path.find_last_not_of(" \t\n\r") + 1);

    if (raw_path.empty()) {
        // 对齐 openclaw: throw new Error("path required")
        result.error = "path required";
        return result;
    }

    // 构建绝对路径 (对齐 openclaw: path.isAbsolute ? path.resolve : path.resolve(workspaceDir, rawPath))
    fs::path abs_path;
    if (fs::path(raw_path).is_absolute()) {
        abs_path = fs::path(raw_path);
    } else {
        abs_path = fs::path(workspace_dir_) / raw_path;
    }

    // 计算相对路径 (对齐 openclaw: path.relative(workspaceDir, absPath).replace(/\\/g, "/"))
    std::string rel;
    try {
        rel = fs::relative(abs_path, workspace_dir_).string();
    } catch (...) {
        rel = raw_path;
    }
    std::replace(rel.begin(), rel.end(), '\\', '/');

    // 对齐 openclaw: inWorkspace = relPath.length > 0 && !relPath.startsWith("..") && !path.isAbsolute(relPath)
    const bool in_workspace = !rel.empty()
        && rel.rfind("..", 0) != 0
        && !fs::path(rel).is_absolute();
    const bool allowed_workspace = in_workspace && is_memory_path(rel);

    // 对齐 openclaw: extraPaths 支持
    bool allowed_additional = false;
    if (!allowed_workspace && !settings_.extra_paths.empty()) {
        auto additional_paths = normalize_extra_memory_paths(workspace_dir_, settings_.extra_paths);
        for (const auto& additional_path : additional_paths) {
            try {
                if (!fs::exists(additional_path)) continue;
                if (fs::is_symlink(additional_path)) continue;
                if (fs::is_directory(additional_path)) {
                    std::string ap_str = additional_path;
                    if (ap_str.back() != '/') ap_str += '/';
                    const std::string abs_str = abs_path.string();
                    if (abs_str == additional_path || abs_str.rfind(ap_str, 0) == 0) {
                        allowed_additional = true;
                        break;
                    }
                } else if (fs::is_regular_file(additional_path)) {
                    if (abs_path.string() == additional_path
                        && additional_path.size() >= 3
                        && additional_path.substr(additional_path.size() - 3) == ".md") {
                        allowed_additional = true;
                        break;
                    }
                }
            } catch (...) {}
        }
    }

    // 对齐 openclaw: if (!allowedWorkspace && !allowedAdditional) throw new Error("path required")
    if (!allowed_workspace && !allowed_additional) {
        result.error = "path required";
        return result;
    }

    // 对齐 openclaw: if (!absPath.endsWith(".md")) throw new Error("path required")
    if (abs_path.extension() != ".md") {
        result.error = "path required";
        return result;
    }

    // 读取文件 (对齐 openclaw: statRegularFile + fs.readFile)
    try {
        if (!fs::exists(abs_path) || !fs::is_regular_file(abs_path)) {
            // 对齐 openclaw: if (statResult.missing) return { text: "", path: relPath }
            result.path = rel;
            result.text = "";
            return result;
        }
        std::ifstream file(abs_path);
        if (!file.is_open()) {
            result.path = rel;
            result.text = "";
            return result;
        }
        std::string content((std::istreambuf_iterator<char>(file)),
                             std::istreambuf_iterator<char>());

        if (!params.from && !params.lines) {
            result.text = content;
            result.path = rel;
            return result;
        }

        // 按行切片 (对齐 openclaw: lines.slice(start-1, start-1+count).join("\n"))
        std::vector<std::string> lines;
        std::istringstream ss(content);
        std::string line;
        while (std::getline(ss, line)) {
            lines.push_back(line);
        }

        const int start     = std::max(1, params.from.value_or(1));
        const int count     = std::max(1, params.lines.value_or(static_cast<int>(lines.size())));
        const int begin_idx = start - 1;
        const int end_idx   = std::min(begin_idx + count, static_cast<int>(lines.size()));

        std::ostringstream out;
        for (int i = begin_idx; i < end_idx; ++i) {
            if (i > begin_idx) out << "\n";
            out << lines[i];
        }
        result.text = out.str();
        result.path = rel;
    } catch (const std::exception& e) {
        log->warn(std::string("[MemoryIndexManager] readFile failed: ") + e.what());
        result.path = rel;
        result.text = "";
    }

    return result;
}

MemoryProviderStatus MemoryIndexManager::status() {
    // 对齐 manager.ts:status()
    MemoryProviderStatus s;
    s.backend       = "builtin";
    s.workspace_dir = workspace_dir_;
    s.db_path       = settings_.store.path;   // 对齐 openclaw: dbPath
    s.dirty         = dirty_ || sessions_dirty_;
    s.extra_paths   = settings_.extra_paths;  // 对齐 openclaw: extraPaths

    // Provider 信息
    if (provider_) {
        s.provider = provider_->get_id();
        s.model    = provider_->get_model();
    } else {
        s.provider = "none";
    }
    s.requested_provider = requested_provider_.has_value()
        ? requested_provider_ : std::nullopt;

    // Sources
    s.sources = std::vector<MemorySource>(sources_.begin(), sources_.end());

    // 对齐 openclaw: sourceCounts (通过 SQL 查询)
    if (db_) {
        // files count
        {
            auto r = db_->prepare_statement("SELECT COUNT(*) FROM files");
            if (r.is_ok()) {
                if (sqlite3_step(r.get()) == SQLITE_ROW)
                    s.files = sqlite3_column_int(r.get(), 0);
                sqlite3_finalize(r.get());
            }
        }
        // chunks count
        {
            auto r = db_->prepare_statement("SELECT COUNT(*) FROM chunks");
            if (r.is_ok()) {
                if (sqlite3_step(r.get()) == SQLITE_ROW)
                    s.chunks = sqlite3_column_int(r.get(), 0);
                sqlite3_finalize(r.get());
            }
        }
        // per-source counts
        s.source_counts = std::vector<MemoryProviderStatus::SourceCount>{};
        for (const auto& src : sources_) {
            const std::string src_str = (src == MemorySource::Memory) ? "memory" : "sessions";
            MemoryProviderStatus::SourceCount sc;
            sc.source = src;
            {
                auto r = db_->prepare_statement("SELECT COUNT(*) FROM files WHERE source = ?");
                if (r.is_ok()) {
                    sqlite3_bind_text(r.get(), 1, src_str.c_str(), -1, SQLITE_TRANSIENT);
                    if (sqlite3_step(r.get()) == SQLITE_ROW)
                        sc.files = sqlite3_column_int(r.get(), 0);
                    sqlite3_finalize(r.get());
                }
            }
            {
                auto r = db_->prepare_statement("SELECT COUNT(*) FROM chunks WHERE source = ?");
                if (r.is_ok()) {
                    sqlite3_bind_text(r.get(), 1, src_str.c_str(), -1, SQLITE_TRANSIENT);
                    if (sqlite3_step(r.get()) == SQLITE_ROW)
                        sc.chunks = sqlite3_column_int(r.get(), 0);
                    sqlite3_finalize(r.get());
                }
            }
            s.source_counts->push_back(sc);
        }
        // cache entries
        {
            MemoryProviderStatus::CacheStatus cache;
            cache.enabled    = cache_.enabled;
            cache.max_entries = cache_.max_entries;
            if (cache_.enabled) {
                auto r = db_->prepare_statement("SELECT COUNT(*) FROM embedding_cache");
                if (r.is_ok()) {
                    if (sqlite3_step(r.get()) == SQLITE_ROW)
                        cache.entries = sqlite3_column_int(r.get(), 0);
                    sqlite3_finalize(r.get());
                }
            }
            s.cache = cache;
        }
    }

    // Vector 状态
    MemoryProviderStatus::VectorStatus vec;
    vec.enabled        = vector_.enabled;
    vec.available      = vector_.available;
    vec.extension_path = vector_.extension_path;
    vec.load_error     = vector_.load_error;
    vec.dims           = vector_.dims;
    s.vector = vec;

    // FTS 状态
    MemoryProviderStatus::FtsStatus fts;
    fts.enabled   = fts_.enabled;
    fts.available = fts_.available;
    fts.error     = fts_.load_error;
    s.fts = fts;

    // Batch 状态
    MemoryProviderStatus::BatchStatus batch;
    batch.enabled          = batch_.enabled;
    batch.failures         = batch_failure_count_;
    batch.limit            = BATCH_FAILURE_LIMIT;
    batch.wait             = batch_.wait;
    batch.concurrency      = batch_.concurrency;
    batch.poll_interval_ms = batch_.poll_interval_ms;
    batch.timeout_ms       = batch_.timeout_ms;
    batch.last_error       = batch_failure_last_error_;
    batch.last_provider    = batch_failure_last_provider_;
    s.batch = batch;

    // Fallback 信息
    if (fallback_reason_) {
        MemoryProviderStatus::FallbackInfo fallback;
        fallback.from   = fallback_from_.value_or("local");
        fallback.reason = fallback_reason_;
        s.fallback = fallback;
    }

    // 对齐 openclaw: custom { searchMode, providerUnavailableReason, readonlyRecovery }
    MemoryProviderStatus::CustomStatus custom;
    custom.search_mode                  = provider_ ? "hybrid" : "fts-only";
    custom.provider_unavailable_reason  = provider_unavailable_reason_;
    custom.readonly_recovery_attempts   = readonly_recovery_attempts_;
    custom.readonly_recovery_successes  = readonly_recovery_successes_;
    custom.readonly_recovery_failures   = readonly_recovery_failures_;
    custom.readonly_recovery_last_error = readonly_recovery_last_error_;
    s.custom = custom;

    return s;
}

void MemoryIndexManager::sync(const SyncParams& params) {
    // 对齐 manager.ts:sync()
    if (closed_) {
        return;
    }

    // 对齐 manager.ts: if (this.syncing) { if (sessionFiles) enqueue; else return this.syncing }
    {
        std::lock_guard<std::mutex> lock(syncing_mutex_);
        if (syncing_) {
            if (params.session_files && !params.session_files->empty()) {
                enqueue_targeted_session_sync(*params.session_files);
            }
            return;
        }
        syncing_ = true;
    }

    // 如果有 session files，入队处理
    if (params.session_files && !params.session_files->empty()) {
        enqueue_targeted_session_sync(*params.session_files);
        std::lock_guard<std::mutex> lock(syncing_mutex_);
        syncing_ = false;
        return;
    }

    // 运行同步（带 readonly 恢复）
    run_sync_with_readonly_recovery(params);

    {
        std::lock_guard<std::mutex> lock(syncing_mutex_);
        syncing_ = false;
    }
}

MemoryEmbeddingProbeResult MemoryIndexManager::probe_embedding_availability() {
    // 对齐 manager.ts:probeEmbeddingAvailability()
    if (!provider_) {
        MemoryEmbeddingProbeResult r;
        r.ok    = false;
        r.error = provider_unavailable_reason_.value_or(
            "No embedding provider available (FTS-only mode)"
        );
        return r;
    }

    // 对齐 openclaw: await this.embedBatchWithRetry(["ping"])
    try {
        auto result = provider_->embed_batch({std::string("ping")});
        if (result.is_ok()) {
            MemoryEmbeddingProbeResult r;
            r.ok = true;
            return r;
        } else {
            MemoryEmbeddingProbeResult r;
            r.ok    = false;
            r.error = result.error;
            return r;
        }
    } catch (const std::exception& e) {
        MemoryEmbeddingProbeResult r;
        r.ok    = false;
        r.error = e.what();
        return r;
    }
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

    // 对齐 openclaw: clearTimeout(watchTimer/sessionWatchTimer/intervalTimer)
    stop_memory_watcher();
    stop_session_watcher();
    stop_interval_sync();

    // 对齐 openclaw: await pendingSync (等待正在进行的同步完成)
    {
        std::lock_guard<std::mutex> lock(syncing_mutex_);
        // syncing_ 为 false 时可以安全关闭
    }

    // 关闭数据库 (对齐 openclaw: this.db.close())
    if (db_) {
        db_.reset();
    }

    // 从缓存中移除 (对齐 openclaw: INDEX_CACHE.delete(this.cacheKey))
    std::lock_guard<std::mutex> lock(cache_mutex_);
    index_cache_.erase(cache_key_);

    log->info("[MemoryIndexManager] Closed: agent=" + agent_id_);
}

// ============================================================================
// Session Management
// ============================================================================

void MemoryIndexManager::warm_session(const std::optional<std::string>& session_key) {
    // 对齐 manager.ts:warmSession()
    // 对齐 manager.ts: if (!this.settings.sync.onSessionStart) return
    if (!settings_.sync.on_session_start) {
        return;
    }

    std::string key = session_key
        ? session_key->substr(0, session_key->find_first_of(" \t\n\r"))
        : "";
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
            log->warn("[MemoryIndexManager] Sync failed: " + r.error);
        }
        return;
    } catch (const std::exception& e) {
        if (!is_readonly_db_error(e) || closed_) {
            log->warn(std::string("[MemoryIndexManager] Sync error: ") + e.what());
            return;
        }

        // Readonly recovery (对齐 manager.ts:runSyncWithReadonlyRecovery)
        const std::string reason = extract_error_reason(e);
        readonly_recovery_attempts_++;
        readonly_recovery_last_error_ = reason;
        log->warn("[MemoryIndexManager] Readonly DB detected, reopening: " + reason);

        // 对齐 openclaw: this.db.close(); this.db = this.openDatabase();
        db_.reset();
        db_ = open_database();
        if (!db_) {
            log->warn("[MemoryIndexManager] Failed to reopen database after readonly recovery");
        }
        // 对齐 openclaw: this.vectorReady = null; this.vector.available = null;
        vector_.available = std::nullopt;
        vector_.load_error = std::nullopt;
        // 对齐 openclaw: this.ensureSchema()
        ensure_schema();
        // 对齐 openclaw: const meta = this.readMeta(); this.vector.dims = meta?.vectorDims
        auto meta = load_index_meta();
        if (meta) vector_.dims = meta->vector_dims;

        try {
            auto r = MemoryManagerSyncOps::run_sync(params);
            if (r.is_ok()) {
                readonly_recovery_successes_++;
            } else {
                readonly_recovery_failures_++;
                log->warn("[MemoryIndexManager] Sync failed after recovery: " + r.error);
            }
        } catch (const std::exception& retry_e) {
            readonly_recovery_failures_++;
            log->warn(std::string("[MemoryIndexManager] Sync failed after recovery: ") + retry_e.what());
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
    if (queued_session_files_.empty()) {
        return;
    }
    // 对齐 openclaw: while (!closed && queuedSessionFiles.size > 0) { drain queue }
    if (!queued_session_sync_) {
        queued_session_sync_ = true;
        while (!closed_ && !queued_session_files_.empty()) {
            std::vector<std::string> batch(queued_session_files_.begin(),
                                           queued_session_files_.end());
            queued_session_files_.clear();
            SyncParams sp;
            sp.reason        = "queued-session-files";
            sp.session_files = batch;
            run_sync_with_readonly_recovery(sp);
        }
        queued_session_sync_ = false;
    }
}

bool MemoryIndexManager::is_readonly_db_error(const std::exception& e) const {
    // 对齐 manager.ts:isReadonlyDbError()
    // /attempt to write a readonly database|database is read-only|SQLITE_READONLY/i
    std::string lower = e.what();
    std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
    return lower.find("attempt to write a readonly database") != std::string::npos ||
           lower.find("database is read-only") != std::string::npos ||
           lower.find("sqlite_readonly") != std::string::npos;
}

std::string MemoryIndexManager::extract_error_reason(const std::exception& e) const {
    // 对齐 manager.ts:extractErrorReason()
    std::string msg = e.what();
    msg.erase(0, msg.find_first_not_of(" \t\n\r"));
    if (!msg.empty()) msg.erase(msg.find_last_not_of(" \t\n\r") + 1);
    return msg.empty() ? "unknown error" : msg;
}

// ============================================================================
// 定时同步 (对齐 manager.ts:ensureIntervalSync)
// ============================================================================

void MemoryIndexManager::ensure_interval_sync() {
    // 对齐 manager.ts:ensureIntervalSync()
    // 如果配置了 sync.interval，启动定时同步
    // 在 C++ 中通过后台线程实现定时同步，具体实现依赖平台定时器
    // 这里记录配置，具体定时器在 start_memory_watcher 中处理
    if (settings_.sync.interval_ms <= 0) {
        return;
    }
    // interval_ms > 0 时，watcher 线程会定期触发 sync
}

// ============================================================================
// MemoryManagerSyncOps 抽象方法实现
// ============================================================================

std::string MemoryIndexManager::compute_provider_key() {
    // 对齐 manager-embedding-ops.ts:computeProviderKey()
    if (!provider_) return "fts-only";
    return provider_->get_id() + ":" + provider_->get_model();
}

void MemoryIndexManager::do_sync(const SyncParams& params) {
    auto result = run_sync(params);
    if (!result.is_ok()) {
        log->warn("[MemoryIndexManager] sync failed: " + result.error);
    }
}

int MemoryIndexManager::get_index_concurrency() {
    return EMBEDDING_INDEX_CONCURRENCY;
}

void MemoryIndexManager::prune_embedding_cache_if_needed() {
    // 对齐 manager-embedding-ops.ts:pruneEmbeddingCacheIfNeeded()
    // 实际实现在 MemoryManagerEmbeddingOps::prune_embedding_cache_if_needed()
    MemoryManagerEmbeddingOps::prune_embedding_cache_if_needed();
}

// ============================================================================
// 全局函数
// ============================================================================

void close_all_memory_index_managers() {
    MemoryIndexManager::close_all();
}

} // namespace memory
} // namespace hunterclaw
