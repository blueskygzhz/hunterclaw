#include <optional>
#include "hunterclaw/memory/qmd_manager.h"
#include "hunterclaw/memory/qmd_scope.h"
#include "hunterclaw/memory/qmd_process.h"
#include "hunterclaw/memory/json_parser.h"
#include "hunterclaw/memory/qmd_query_parser.h"
#include <stdexcept>
#include <sstream>
#include <cstdlib>
#include <unistd.h>
#include <sys/wait.h>
#include <filesystem>
#include <fstream>
#include <regex>
#include <algorithm>
#include <chrono>
#include <set>

namespace fs = std::filesystem;

namespace hunterclaw {
namespace memory {

// Forward declarations
std::vector<std::string> extract_keywords(const std::string& query);

namespace {
    // Helper functions
    bool has_han_script(const std::string& text) {
        // Check if text contains Han (CJK Unified Ideographs) characters
        for (unsigned char c : text) {
            // UTF-8 CJK range starts with bytes 0xE4-0xE9
            if (c >= 0xE4 && c <= 0xE9) {
                return true;
            }
        }
        return false;
    }

    int utf8_char_count(const std::string& text) {
        int count = 0;
        for (size_t i = 0; i < text.length(); ) {
            unsigned char c = text[i];
            if (c < 0x80) {
                i += 1;
            } else if (c < 0xE0) {
                i += 2;
            } else if (c < 0xF0) {
                i += 3;
            } else {
                i += 4;
            }
            count++;
        }
        return count;
    }
}

// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
// QmdMemoryManager Implementation
// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━

QmdMemoryManager::QmdMemoryManager(const CreateParams& params)
    : agent_id_(params.agent_id),
      workspace_dir_(params.workspace_dir),
      qmd_config_(params.qmd_config) {
    
    // Build directory paths
    const char* home = std::getenv("HOME");
    if (!home) throw std::runtime_error("HOME environment variable not set");
    
    state_dir_ = std::string(home) + "/.openclaw/agents/" + agent_id_;
    qmd_dir_ = state_dir_ + "/qmd";
    xdg_config_home_ = qmd_dir_ + "/xdg-config";
    xdg_cache_home_ = qmd_dir_ + "/xdg-cache";
    index_path_ = xdg_cache_home_ + "/qmd/index.sqlite";
    
    // Setup session exporter if enabled
    if (qmd_config_.sessions.enabled) {
        session_exporter_ = SessionExporterConfig{
            qmd_config_.sessions.export_dir.empty() 
                ? qmd_dir_ + "/sessions" 
                : qmd_config_.sessions.export_dir,
            qmd_config_.sessions.retention_days > 0
                ? std::optional<int64_t>(qmd_config_.sessions.retention_days * 24LL * 3600 * 1000)
                : std::nullopt,
            "sessions"
        };
        
        // Add sessions collection
        qmd_config_.collections.push_back(ManagedCollection{
            session_exporter_->collection_name,
            session_exporter_->dir,
            "**/*.md",
            "sessions"
        });
    }
}

std::shared_ptr<QmdMemoryManager> QmdMemoryManager::create(const CreateParams& params) {
    auto manager = std::shared_ptr<QmdMemoryManager>(new QmdMemoryManager(params));
    manager->initialize(params.mode);
    return manager;
}

QmdMemoryManager::~QmdMemoryManager() {
    try {
        close();
    } catch (...) {
        // Ignore errors in destructor
    }
}

void QmdMemoryManager::initialize(QmdManagerMode mode) {
    // Bootstrap collections
    bootstrap_collections();
    
    if (mode == QmdManagerMode::STATUS) {
        return;  // Status-only mode, skip full initialization
    }
    
    // Create directories
    fs::create_directories(xdg_config_home_);
    fs::create_directories(xdg_cache_home_);
    fs::create_directories(fs::path(index_path_).parent_path());
    
    if (session_exporter_.has_value()) {
        fs::create_directories(session_exporter_->dir);
    }
    
    // Ensure collections
    ensure_collections();
    
    // Run boot update if configured
    if (qmd_config_.update.on_boot) {
        try {
            run_update("boot", true);
        } catch (const std::exception& e) {
            // Log warning but don't fail
        }
    }
}

void QmdMemoryManager::bootstrap_collections() {
    collection_roots_.clear();
    sources_.clear();
    
    for (const auto& collection : qmd_config_.collections) {
        MemorySource kind = (collection.kind == "sessions") 
            ? MemorySource::Sessions 
            : MemorySource::Memory;
        
        collection_roots_[collection.name] = CollectionRoot{collection.path, kind};
        
        // Add to sources set (using set semantics)
        bool found = false;
        for (auto s : sources_) {
            if (s == kind) { found = true; break; }
        }
        if (!found) sources_.push_back(kind);
    }
    
    // \u5bf9\u9f50 openclaw: computeManagedCollectionNames
    managed_collection_names_ = compute_managed_collection_names();
}

void QmdMemoryManager::ensure_collections() {
    // List existing collections
    auto existing = list_collections_best_effort();
    
    // Add/update each configured collection
    for (const auto& collection : qmd_config_.collections) {
        auto it = existing.find(collection.name);
        
        // 对齐 openclaw: shouldRebindCollection
        if (it != existing.end() && !should_rebind_collection(collection, it->second)) {
            continue;
        }
        
        // Remove old collection if exists
        if (it != existing.end()) {
            try {
                remove_collection(collection.name);
            } catch (const std::exception& e) {
                std::string msg = e.what();
                std::transform(msg.begin(), msg.end(), msg.begin(), ::tolower);
                if (!is_collection_missing_error(msg)) {
                    // log warning
                }
            }
        }
        
        // Ensure collection path exists
        ensure_collection_path(collection);
        
        // Add collection
        try {
            add_collection(collection.path, collection.name, collection.pattern);
            existing[collection.name] = {collection.path, collection.pattern};
        } catch (const std::exception& e) {
            std::string msg = e.what();
            std::transform(msg.begin(), msg.end(), msg.begin(), ::tolower);
            if (is_collection_already_exists_error(msg)) {
                // log warning: already exists
            } else {
                // log warning
            }
        }
    }
}

std::map<std::string, ListedCollection> QmdMemoryManager::list_collections_best_effort() {
    std::map<std::string, ListedCollection> result;
    
    try {
        QmdCommandOptions opts;
        opts.timeout_ms = qmd_config_.update.command_timeout_ms;
        auto qmd_result = run_qmd({"collection", "list", "--json"}, &opts);
        result = parse_listed_collections(qmd_result.stdout_data);
    } catch (...) {
        // ignore; older qmd versions might not support list --json.
    }
    
    return result;
}

void QmdMemoryManager::add_collection(
    const std::string& path,
    const std::string& name,
    const std::string& pattern
) {
    // 对齐 openclaw qmd-manager.ts:addCollection
    // args: ["collection", "add", path, "--name", name, "--mask", pattern]
    std::vector<std::string> args = {
        "collection", "add",
        path,
        "--name", name,
        "--mask", pattern
    };
    
    QmdCommandOptions opts{qmd_config_.update.command_timeout_ms, {}};
    run_qmd(args, &opts);
}

void QmdMemoryManager::remove_collection(const std::string& name) {
    QmdCommandOptions opts{qmd_config_.update.command_timeout_ms, {}};
    run_qmd({"collection", "remove", name}, &opts);
}

void QmdMemoryManager::ensure_collection_path(const ManagedCollection& collection) {
    fs::create_directories(collection.path);
}

void QmdMemoryManager::run_update(const std::string& reason, bool force) {
    if (closed_) return;

    // 对齐 openclaw: shouldSkipUpdate
    if (should_skip_update(force)) {
        return;
    }

    // Export sessions if configured
    if (session_exporter_.has_value()) {
        try {
            export_session_files();
        } catch (...) {}
    }

    // Run qmd update
    try {
        QmdCommandOptions opts;
        opts.timeout_ms = qmd_config_.update.update_timeout_ms;
        opts.discard_output = true;
        run_qmd({"update"}, &opts);
    } catch (const std::exception& e) {
        // Log warning but don't fail
    }

    // Run embed if needed (对齐 openclaw: shouldRunEmbed)
    if (should_run_embed(force)) {
        try {
            QmdCommandOptions embed_opts;
            embed_opts.timeout_ms = qmd_config_.update.embed_timeout_ms;
            embed_opts.discard_output = true;
            run_qmd({"embed"}, &embed_opts);
            last_embed_at_ = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch()
            ).count();
            embed_backoff_until_ = 0;
            embed_failure_count_ = 0;
        } catch (const std::exception& e) {
            // note embed failure
            embed_failure_count_++;
            const int64_t base_ms = 60000;
            const int64_t max_ms = 60LL * 60 * 1000;
            int64_t delay_ms = std::min(max_ms, base_ms * (int64_t(1) << std::max(0, embed_failure_count_ - 1)));
            embed_backoff_until_ = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch()
            ).count() + delay_ms;
        }
    }

    last_update_at_ = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()
    ).count();
}

// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
// MemorySearchManager Interface Implementation
// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━

std::vector<MemorySearchResult> QmdMemoryManager::search(
    const std::string& query,
    const SearchOptions& opts
) {
    if (closed_) {
        throw std::runtime_error("QmdMemoryManager is closed");
    }

    // 对齐 openclaw: scope check
    if (!is_scope_allowed(opts.session_key)) {
        log_scope_denied(opts.session_key);
        return {};
    }

    std::string trimmed = query;
    trimmed.erase(0, trimmed.find_first_not_of(" \t\n\r"));
    trimmed.erase(trimmed.find_last_not_of(" \t\n\r") + 1);
    if (trimmed.empty()) {
        return {};
    }

    // 对齐 openclaw: waitForPendingUpdateBeforeSearch
    wait_for_pending_update();

    // 对齐 openclaw: limit = min(qmd.limits.maxResults, opts.maxResults ?? qmd.limits.maxResults)
    int limit = qmd_config_.limits.max_results;
    if (opts.max_results.has_value() && opts.max_results.value() > 0) {
        limit = std::min(limit, opts.max_results.value());
    }

    auto collection_names = list_managed_collection_names();
    if (collection_names.empty()) {
        // log.warn: qmd query skipped: no managed collections configured
        return {};
    }

    const std::string& qmd_search_command = qmd_config_.search_mode;

    // Run search attempt
    auto run_search_attempt = [&](bool allow_missing_collection_repair) -> std::vector<QmdQueryResult> {
        try {
            if (collection_names.size() > 1) {
                return run_query_across_collections(trimmed, limit, collection_names, qmd_search_command);
            }
            auto args = build_search_args(qmd_search_command, trimmed, limit);
            auto filter_args = build_collection_filter_args(collection_names);
            args.insert(args.end(), filter_args.begin(), filter_args.end());

            QmdCommandOptions cmd_opts;
            cmd_opts.timeout_ms = qmd_config_.limits.timeout_ms;
            auto result = run_qmd(args, &cmd_opts);
            return hunterclaw::memory::parse_qmd_query_json(result.stdout_data, result.stderr_data);
        } catch (const std::exception& err) {
            if (allow_missing_collection_repair && is_missing_collection_search_error(err)) {
                throw;
            }
            // 对齐 openclaw: fallback to "query" if unsupported option
            if (qmd_search_command != "query" && is_unsupported_qmd_option_error(err)) {
                try {
                    if (collection_names.size() > 1) {
                        return run_query_across_collections(trimmed, limit, collection_names, "query");
                    }
                    auto fallback_args = build_search_args("query", trimmed, limit);
                    auto filter_args = build_collection_filter_args(collection_names);
                    fallback_args.insert(fallback_args.end(), filter_args.begin(), filter_args.end());
                    QmdCommandOptions cmd_opts;
                    cmd_opts.timeout_ms = qmd_config_.limits.timeout_ms;
                    auto fallback = run_qmd(fallback_args, &cmd_opts);
                    return hunterclaw::memory::parse_qmd_query_json(fallback.stdout_data, fallback.stderr_data);
                } catch (const std::exception& fallback_err) {
                    throw std::runtime_error(std::string(fallback_err.what()));
                }
            }
            throw;
        }
    };

    std::vector<QmdQueryResult> parsed;
    try {
        parsed = run_search_attempt(true);
    } catch (const std::exception& err) {
        if (is_missing_collection_search_error(err)) {
            // repair: re-ensure collections then retry once
            ensure_collections();
            parsed = run_search_attempt(false);
        } else {
            throw;
        }
    }

    std::vector<MemorySearchResult> results;
    double min_score = opts.min_score.value_or(0.0);
    for (const auto& entry : parsed) {
        auto mem_res = qmd_result_to_memory_result(entry);
        // 对齐 openclaw: snippet clamped to maxSnippetChars
        if (!mem_res.snippet.empty() && 
            qmd_config_.limits.max_snippet_chars > 0 &&
            (int)mem_res.snippet.size() > qmd_config_.limits.max_snippet_chars) {
            mem_res.snippet = mem_res.snippet.substr(0, qmd_config_.limits.max_snippet_chars);
        }
        if (mem_res.score < min_score) {
            continue;
        }
        results.push_back(mem_res);
    }

    return clamp_results_by_injected_chars(diversify_results_by_source(results, limit));
}

ReadFileResult QmdMemoryManager::read_file(const ReadFileParams& params) {
    const std::string& rel_path = params.rel_path;
    int from = params.from.value_or(0);
    int lines = params.lines.value_or(-1);
    // Read file from workspace
    std::string full_path = workspace_dir_ + "/" + rel_path;
    
    std::ifstream file(full_path);
    if (!file.is_open()) {
        throw std::runtime_error("Failed to open file: " + full_path);
    }
    
    std::ostringstream content;
    std::string line;
    int current_line = 0;
    int read_count = 0;
    
    while (std::getline(file, line)) {
        if (current_line >= from) {
            content << line << "\n";
            read_count++;
            
            if (lines > 0 && read_count >= lines) {
                break;
            }
        }
        current_line++;
    }
    
    return ReadFileResult{content.str(), full_path};
}

MemoryProviderStatus QmdMemoryManager::status() {
    // 对齐 openclaw: status() returns full provider status
    MemoryProviderStatus s;
    s.backend = "qmd";
    s.provider = "qmd";
    s.model = "qmd";
    s.requested_provider = "qmd";
    s.workspace_dir = workspace_dir_;
    s.db_path = index_path_;
    s.dirty = false;
    
    // Sources
    std::vector<MemorySource> src_list(sources_.begin(), sources_.end());
    s.sources = src_list;
    
    // Stats (simplified - real impl would query SQLite)
    s.files = 0;
    s.chunks = 0;
    
    return s;
}

void QmdMemoryManager::sync(const SyncParams& params) {
    // 对齐 openclaw: sync() with progress callback
    if (params.progress) {
        params.progress({0, 1, std::string("Updating QMD index\xe2\x80\xa6")});
    }
    std::string reason = params.reason.value_or("manual");
    bool force = params.force.value_or(false);
    run_update(reason, force);
    if (params.progress) {
        params.progress({1, 1, std::string("QMD index updated")});
    }
}

MemoryEmbeddingProbeResult QmdMemoryManager::probe_embedding_availability() {
    // 对齐 openclaw: probeEmbeddingAvailability() { return { ok: true }; }
    return MemoryEmbeddingProbeResult{true, std::nullopt};
}

bool QmdMemoryManager::probe_vector_availability() {
    // QMD always has vector search available
    return true;
}

void QmdMemoryManager::close() {
    if (closed_) return;
    closed_ = true;
    
    // Cleanup resources
    // (QMD is stateless, no cleanup needed)
}

// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
// QMD Process Management
// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━

QmdMemoryManager::QmdCommandResult QmdMemoryManager::run_qmd(
    const std::vector<std::string>& args,
    const QmdCommandOptions* opts
) {
    // Build environment
    auto env = build_xdg_env();
    if (opts && !opts->env.empty()) {
        for (const auto& [key, value] : opts->env) {
            env[key] = value;
        }
    }

    int timeout_ms = opts ? opts->timeout_ms : 30000;
    bool discard_output = opts ? opts->discard_output : false;

    // \u5bf9\u9f50 openclaw: resolveCliSpawnInvocation + runCliCommand
    CliSpawnInvocation invocation = resolve_cli_spawn_invocation(
        qmd_config_.qmd_binary_path, args, env, "qmd"
    );

    std::string command_summary = "qmd";
    for (const auto& arg : args) {
        command_summary += " " + arg;
    }

    auto result = run_cli_command(
        command_summary,
        invocation,
        env,
        workspace_dir_,
        timeout_ms,
        200000,  // MAX_QMD_OUTPUT_CHARS
        discard_output
    );

    return QmdCommandResult{result.stdout_text, result.stderr_text, result.exit_code};
}
std::map<std::string, std::string> QmdMemoryManager::build_xdg_env() {
    std::map<std::string, std::string> env;
    env["XDG_CONFIG_HOME"] = xdg_config_home_;
    env["XDG_CACHE_HOME"] = xdg_cache_home_;
    env["QMD_CONFIG_DIR"] = xdg_config_home_;  // Workaround for QMD bug
    env["NO_COLOR"] = "1";
    return env;
}

// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
// Query & Search Utilities
// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━

std::vector<QmdQueryResult> QmdMemoryManager::parse_qmd_query_json(
    const std::string& stdout_data,
    const std::string& stderr_data
) {
    // 对齐 openclaw qmd-manager.ts: parseQmdQueryJson(result.stdout, result.stderr)
    // 委托给 qmd_query_parser.cc 中的实现（严格对齐 openclaw qmd-query-parser.ts）
    return hunterclaw::memory::parse_qmd_query_json(stdout_data, stderr_data);
}

MemorySearchResult QmdMemoryManager::qmd_result_to_memory_result(const QmdQueryResult& qmd_result) {
    MemorySearchResult result;
    result.path = qmd_result.file.value_or("");
    result.start_line = 0;  // QMD doesn't provide line numbers in simple mode
    result.end_line = 0;
    result.score = qmd_result.score.value_or(0.0);
    result.snippet = qmd_result.snippet.value_or("");
    
    // Determine source from collection
    const std::string collection = qmd_result.collection.value_or("");
    auto it = collection_roots_.find(collection);
    result.source = (it != collection_roots_.end()) 
        ? it->second.kind 
        : MemorySource::Memory;
    
    return result;
}

std::string QmdMemoryManager::normalize_han_bm25_query(const std::string& query) {
    // Normalize query for Han script (Chinese) BM25 search
    std::string trimmed = query;
    trimmed.erase(0, trimmed.find_first_not_of(" \t\n\r"));
    trimmed.erase(trimmed.find_last_not_of(" \t\n\r") + 1);
    
    if (trimmed.empty() || !has_han_script(trimmed)) {
        return trimmed;
    }
    
    // Extract keywords
    std::vector<std::string> keywords = extract_keywords(trimmed);
    std::vector<std::string> normalized_keywords;
    std::set<std::string> seen;
    
    const int MAX_KEYWORDS = 12;  // QMD_BM25_HAN_KEYWORD_LIMIT
    
    for (const auto& keyword : keywords) {
        std::string token = keyword;
        token.erase(0, token.find_first_not_of(" \t"));
        token.erase(token.find_last_not_of(" \t") + 1);
        
        if (token.empty() || seen.count(token) > 0) {
            continue;
        }
        
        bool includes_han = has_han_script(token);
        
        // Han unigrams are too broad for BM25
        if (includes_han && utf8_char_count(token) < 2) {
            continue;
        }
        
        // Non-Han tokens need at least 2 chars
        if (!includes_han && token.length() < 2) {
            continue;
        }
        
        seen.insert(token);
        normalized_keywords.push_back(token);
        
        if (normalized_keywords.size() >= static_cast<size_t>(MAX_KEYWORDS)) {
            break;
        }
    }
    
    if (normalized_keywords.empty()) {
        return trimmed;
    }
    
    // Join with spaces
    std::string result;
    for (size_t i = 0; i < normalized_keywords.size(); ++i) {
        if (i > 0) result += " ";
        result += normalized_keywords[i];
    }
    
    return result;
}

void QmdMemoryManager::export_session_files() {
    if (!session_exporter_.has_value()) {
        return;  // Session export not enabled
    }
    
    // Check if session directory exists
    if (!fs::exists(session_exporter_->dir)) {
        fs::create_directories(session_exporter_->dir);
    }
    
    // Clean up old sessions if retention is configured
    if (session_exporter_->retention_ms.has_value()) {
        auto now = std::chrono::system_clock::now();
        auto cutoff = now - std::chrono::milliseconds(session_exporter_->retention_ms.value());
        
        try {
            for (const auto& entry : fs::directory_iterator(session_exporter_->dir)) {
                if (!entry.is_regular_file()) {
                    continue;
                }
                
                auto file_time = fs::last_write_time(entry.path());
                auto file_time_sys = std::chrono::time_point_cast<std::chrono::system_clock::duration>(
                    file_time - fs::file_time_type::clock::now() + std::chrono::system_clock::now()
                );
                
                if (file_time_sys < cutoff) {
                    // Delete old session file
                    fs::remove(entry.path());
                }
            }
        } catch (const std::exception& e) {
            // Log warning but don't fail
        }
    }
    
    // Note: Session file export from session manager would be done here
    // For now, this is a placeholder for the full implementation
}

// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
// Scope Helpers
// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━

bool QmdMemoryManager::is_scope_allowed(const std::optional<std::string>& session_key) const {
    return is_qmd_scope_allowed(qmd_config_.scope_config, session_key);
}

void QmdMemoryManager::log_scope_denied(const std::optional<std::string>& session_key) const {
    // log.warn: qmd search denied by scope
    (void)session_key;
}

// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
// Collection Name Management
// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━

std::vector<std::string> QmdMemoryManager::list_managed_collection_names() const {
    return managed_collection_names_;
}

std::vector<std::string> QmdMemoryManager::compute_managed_collection_names() const {
    std::set<std::string> seen;
    std::vector<std::string> names;
    for (const auto& collection : qmd_config_.collections) {
        std::string name = collection.name;
        // trim
        name.erase(0, name.find_first_not_of(" \t"));
        name.erase(name.find_last_not_of(" \t") + 1);
        if (name.empty() || seen.count(name)) continue;
        seen.insert(name);
        names.push_back(name);
    }
    return names;
}

// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
// Search Arg Builders
// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━

std::vector<std::string> QmdMemoryManager::build_search_args(
    const std::string& command,
    const std::string& query,
    int limit
) const {
    // 对齐 openclaw: buildSearchArgs
    // For "search" command, normalize Han BM25 query
    std::string normalized_query = (command == "search") 
        ? const_cast<QmdMemoryManager*>(this)->normalize_han_bm25_query(query)
        : query;
    if (command == "query") {
        return {"query", normalized_query, "--json", "-n", std::to_string(limit)};
    }
    return {command, normalized_query, "--json", "-n", std::to_string(limit)};
}

std::vector<std::string> QmdMemoryManager::build_collection_filter_args(
    const std::vector<std::string>& collection_names
) const {
    // 对齐 openclaw: buildCollectionFilterArgs -> flatMap([name] => ["-c", name])
    std::vector<std::string> args;
    for (const auto& name : collection_names) {
        if (!name.empty()) {
            args.push_back("-c");
            args.push_back(name);
        }
    }
    return args;
}

// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
// Multi-Collection Query
// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━

std::vector<QmdQueryResult> QmdMemoryManager::run_query_across_collections(
    const std::string& query,
    int limit,
    const std::vector<std::string>& collection_names,
    const std::string& command
) {
    // 对齐 openclaw: runQueryAcrossCollections
    // Deduplicate by result key, keep best score
    std::map<std::string, QmdQueryResult> best_by_result_key;

    for (const auto& collection_name : collection_names) {
        auto args = build_search_args(command, query, limit);
        args.push_back("-c");
        args.push_back(collection_name);

        QmdCommandOptions cmd_opts;
        cmd_opts.timeout_ms = qmd_config_.limits.timeout_ms;
        auto result = run_qmd(args, &cmd_opts);
        auto parsed = hunterclaw::memory::parse_qmd_query_json(result.stdout_data, result.stderr_data);

        for (auto& entry : parsed) {
            // Assign collection if missing
            if (!entry.collection.has_value() || entry.collection->empty()) {
                entry.collection = collection_name;
            }
            // Build result key
            std::string result_key;
            const std::string docid = entry.docid.value_or("");
            const std::string coll = entry.collection.value_or("");
            const std::string file = entry.file.value_or("");
            if (!docid.empty()) {
                result_key = "docid:" + docid;
            } else if (!coll.empty() && !file.empty()) {
                result_key = "file:" + coll + ":" + file;
            } else {
                continue;
            }

            auto it = best_by_result_key.find(result_key);
            double entry_score = entry.score.value_or(0.0);
            if (it == best_by_result_key.end() || entry_score > it->second.score.value_or(0.0)) {
                best_by_result_key[result_key] = entry;
            }
        }
    }

    // Collect and sort by score descending
    std::vector<QmdQueryResult> results;
    for (auto& [key, entry] : best_by_result_key) {
        results.push_back(entry);
    }
    std::sort(results.begin(), results.end(),
        [](const QmdQueryResult& a, const QmdQueryResult& b) {
            return a.score.value_or(0.0) > b.score.value_or(0.0);
        });
    return results;
}

// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
// Parse Listed Collections
// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━

std::map<std::string, ListedCollection> QmdMemoryManager::parse_listed_collections(
    const std::string& output
) {
    std::map<std::string, ListedCollection> listed;
    std::string trimmed = output;
    trimmed.erase(0, trimmed.find_first_not_of(" \t\n\r"));
    trimmed.erase(trimmed.find_last_not_of(" \t\n\r") + 1);
    if (trimmed.empty()) return listed;

    // Try JSON parse first
    try {
        auto j = nlohmann::json::parse(trimmed);
        if (j.is_array()) {
            for (const auto& entry : j) {
                if (entry.is_string()) {
                    listed[entry.get<std::string>()] = {};
                    continue;
                }
                if (!entry.is_object()) continue;
                std::string name;
                if (entry.contains("name") && entry["name"].is_string()) {
                    name = entry["name"].get<std::string>();
                }
                if (name.empty()) continue;
                ListedCollection lc;
                if (entry.contains("path") && entry["path"].is_string()) {
                    lc.path = entry["path"].get<std::string>();
                }
                if (entry.contains("pattern") && entry["pattern"].is_string()) {
                    lc.pattern = entry["pattern"].get<std::string>();
                } else if (entry.contains("mask") && entry["mask"].is_string()) {
                    lc.pattern = entry["mask"].get<std::string>();
                }
                listed[name] = lc;
            }
            return listed;
        }
    } catch (...) {}

    // Fallback: parse table output line by line
    std::istringstream iss(output);
    std::string line;
    std::string current_name;
    std::regex collection_line_re(R"(^\s*([a-z0-9._-]+)\s+\(qmd://[^)]+\)\s*$)",
        std::regex::icase);
    std::regex bare_name_re(R"(^\s*([a-z0-9._-]+)\s*$)", std::regex::icase);
    std::regex pattern_re(R"(^\s*(?:pattern|mask)\s*:\s*(.+?)\s*$)", std::regex::icase);
    std::regex path_re(R"(^\s*path\s*:\s*(.+?)\s*$)", std::regex::icase);

    while (std::getline(iss, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        std::string trimmed_line = line;
        trimmed_line.erase(0, trimmed_line.find_first_not_of(" \t"));
        trimmed_line.erase(trimmed_line.find_last_not_of(" \t") + 1);
        if (trimmed_line.empty()) { current_name.clear(); continue; }

        std::smatch m;
        if (std::regex_match(line, m, collection_line_re)) {
            current_name = m[1].str();
            if (!listed.count(current_name)) listed[current_name] = {};
            continue;
        }
        if (std::regex_search(trimmed_line, std::regex(R"(^collections\b)", std::regex::icase))) {
            continue;
        }
        if (std::regex_match(trimmed_line, m, bare_name_re) && trimmed_line.find(':') == std::string::npos) {
            current_name = m[1].str();
            if (!listed.count(current_name)) listed[current_name] = {};
            continue;
        }
        if (current_name.empty()) continue;
        if (std::regex_match(line, m, pattern_re)) {
            listed[current_name].pattern = m[1].str();
            continue;
        }
        if (std::regex_match(line, m, path_re)) {
            listed[current_name].path = m[1].str();
        }
    }
    return listed;
}

// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
// Collection Rebind Helpers
// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━

bool QmdMemoryManager::should_rebind_collection(
    const ManagedCollection& collection,
    const ListedCollection& listed
) {
    // 对齐 openclaw: shouldRebindCollection
    if (listed.path.empty()) {
        // Older qmd versions may only return names; do not rebind without metadata
        return false;
    }
    if (!paths_match(listed.path, collection.path)) {
        return true;
    }
    if (!listed.pattern.empty() && listed.pattern != collection.pattern) {
        return true;
    }
    return false;
}

bool QmdMemoryManager::paths_match(const std::string& left, const std::string& right) {
    // 对齐 openclaw: pathsMatch - resolve to absolute and normalize
    auto normalize = [&](const std::string& value) -> std::string {
        if (value.empty()) return value;
        std::string resolved;
        if (value[0] == '/') {
            resolved = value;
        } else {
            resolved = workspace_dir_ + "/" + value;
        }
        // Normalize: remove trailing slash, resolve ..
        return fs::path(resolved).lexically_normal().string();
    };
    return normalize(left) == normalize(right);
}

// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
// Clamp Results by Injected Chars
// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━

std::vector<MemorySearchResult> QmdMemoryManager::clamp_results_by_injected_chars(
    const std::vector<MemorySearchResult>& results
) {
    // 对齐 openclaw: clampResultsByInjectedChars
    int budget = qmd_config_.limits.max_injected_chars;
    if (budget <= 0) {
        return results;
    }
    int remaining = budget;
    std::vector<MemorySearchResult> clamped;
    for (const auto& entry : results) {
        if (remaining <= 0) break;
        const std::string& snippet = entry.snippet;
        if ((int)snippet.size() <= remaining) {
            clamped.push_back(entry);
            remaining -= (int)snippet.size();
        } else {
            MemorySearchResult trimmed_entry = entry;
            trimmed_entry.snippet = snippet.substr(0, std::max(0, remaining));
            clamped.push_back(trimmed_entry);
            break;
        }
    }
    return clamped;
}

// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
// Update Helpers
// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━

bool QmdMemoryManager::should_skip_update(bool force) const {
    // 对齐 openclaw: shouldSkipUpdate
    if (force) return false;
    int debounce_ms = qmd_config_.update.debounce_ms;
    if (debounce_ms <= 0) return false;
    if (last_update_at_ == 0) return false;
    int64_t now = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()
    ).count();
    return (now - last_update_at_) < debounce_ms;
}

bool QmdMemoryManager::should_run_embed(bool force) const {
    // 对齐 openclaw: shouldRunEmbed
    if (qmd_config_.search_mode == "search") return false;
    int64_t now = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()
    ).count();
    if (embed_backoff_until_ > 0 && now < embed_backoff_until_) return false;
    int embed_interval_ms = qmd_config_.update.embed_interval_ms;
    return force || last_embed_at_ == 0 ||
           (embed_interval_ms > 0 && (now - last_embed_at_) > embed_interval_ms);
}

// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
// Error Helpers
// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━

bool QmdMemoryManager::is_sqlite_busy_error(const std::exception& err) const {
    std::string msg = err.what();
    std::transform(msg.begin(), msg.end(), msg.begin(), ::tolower);
    return msg.find("sqlite_busy") != std::string::npos ||
           msg.find("database is locked") != std::string::npos;
}

bool QmdMemoryManager::is_unsupported_qmd_option_error(const std::exception& err) const {
    std::string msg = err.what();
    std::transform(msg.begin(), msg.end(), msg.begin(), ::tolower);
    return msg.find("unknown flag") != std::string::npos ||
           msg.find("unknown option") != std::string::npos ||
           msg.find("unrecognized option") != std::string::npos ||
           msg.find("flag provided but not defined") != std::string::npos ||
           msg.find("unexpected argument") != std::string::npos;
}

bool QmdMemoryManager::is_missing_collection_search_error(const std::exception& err) const {
    std::string msg = err.what();
    std::transform(msg.begin(), msg.end(), msg.begin(), ::tolower);
    return is_collection_missing_error(msg) && msg.find("collection") != std::string::npos;
}

// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
// Additional Helper Methods
// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━

std::vector<MemorySearchResult> QmdMemoryManager::diversify_results_by_source(
    const std::vector<MemorySearchResult>& results,
    int limit
) {
    int target = std::max(0, limit);
    if (target <= 0) {
        return {};
    }
    
    if (results.size() <= 1) {
        std::vector<MemorySearchResult> subset;
        for (size_t i = 0; i < results.size() && i < static_cast<size_t>(target); ++i) {
            subset.push_back(results[i]);
        }
        return subset;
    }
    
    // Group by source
    std::map<MemorySource, std::vector<MemorySearchResult>> by_source;
    for (const auto& entry : results) {
        by_source[entry.source].push_back(entry);
    }
    
    bool has_sessions = by_source.count(MemorySource::Sessions) > 0;
    bool has_memory = by_source.count(MemorySource::Memory) > 0;
    
    // If only one source, just slice
    if (!has_sessions || !has_memory) {
        std::vector<MemorySearchResult> subset;
        for (size_t i = 0; i < results.size() && i < static_cast<size_t>(target); ++i) {
            subset.push_back(results[i]);
        }
        return subset;
    }
    
    // Sort sources by best score
    std::vector<std::pair<MemorySource, double>> source_order;
    for (const auto& [source, entries] : by_source) {
        double best_score = !entries.empty() ? entries[0].score : 0.0;
        source_order.push_back({source, best_score});
    }
    
    std::sort(source_order.begin(), source_order.end(),
        [](const auto& a, const auto& b) { return a.second > b.second; });
    
    // Interleave results from different sources
    std::vector<MemorySearchResult> diversified;
    std::map<MemorySource, size_t> indices;
    
    while (diversified.size() < static_cast<size_t>(target)) {
        bool emitted = false;
        
        for (const auto& [source, _] : source_order) {
            auto& source_results = by_source[source];
            size_t& idx = indices[source];
            
            if (idx < source_results.size()) {
                diversified.push_back(source_results[idx]);
                idx++;
                emitted = true;
                
                if (diversified.size() >= static_cast<size_t>(target)) {
                    break;
                }
            }
        }
        
        if (!emitted) {
            break;
        }
    }
    
    return diversified;
}

void QmdMemoryManager::wait_for_pending_update() {
    // Wait for pending update to complete (with timeout)
    // In real implementation, this would check pendingUpdate promise
    // For now, this is a no-op placeholder
    
    // TypeScript version:
    // await Promise.race([
    //   pending.catch(() => undefined),
    //   new Promise<void>((resolve) => setTimeout(resolve, SEARCH_PENDING_UPDATE_WAIT_MS))
    // ]);
}

}  // namespace memory
}  // namespace hunterclaw
