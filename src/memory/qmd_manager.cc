#include "hunterclaw/memory/qmd_manager.h"
#include <stdexcept>
#include <sstream>
#include <cstdlib>
#include <unistd.h>
#include <sys/wait.h>
#include <filesystem>
#include <fstream>
#include <regex>

namespace fs = std::filesystem;

namespace hunterclaw {
namespace memory {

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
            ? MemorySource::SESSIONS 
            : MemorySource::MEMORY;
        
        collection_roots_[collection.name] = CollectionRoot{collection.path, kind};
        
        // Add to sources set
        bool found = false;
        for (auto s : sources_) {
            if (s == kind) {
                found = true;
                break;
            }
        }
        if (!found) {
            sources_.push_back(kind);
        }
        
        managed_collection_names_.push_back(collection.name);
    }
}

void QmdMemoryManager::ensure_collections() {
    // List existing collections
    auto existing = list_collections_best_effort();
    
    // Add/update each configured collection
    for (const auto& collection : qmd_config_.collections) {
        auto it = existing.find(collection.name);
        
        // If exists and matches, skip
        if (it != existing.end() && 
            it->second.path == collection.path &&
            it->second.pattern == collection.pattern) {
            continue;
        }
        
        // Remove old collection if exists
        if (it != existing.end()) {
            try {
                remove_collection(collection.name);
            } catch (...) {
                // Ignore errors
            }
        }
        
        // Ensure collection path exists
        ensure_collection_path(collection);
        
        // Add collection
        try {
            add_collection(collection.path, collection.name, collection.pattern);
        } catch (const std::exception& e) {
            // Log warning
        }
    }
}

std::map<std::string, ListedCollection> QmdMemoryManager::list_collections_best_effort() {
    std::map<std::string, ListedCollection> result;
    
    try {
        auto qmd_result = run_qmd({"collection", "list", "--json"}, 
            QmdCommandOptions{qmd_config_.update.command_timeout_ms});
        
        // Parse JSON (simplified - real implementation would use JSON library)
        // Format: [{"name": "...", "path": "...", "pattern": "..."}, ...]
        
    } catch (...) {
        // Ignore errors - older QMD versions might not support list --json
    }
    
    return result;
}

void QmdMemoryManager::add_collection(
    const std::string& path,
    const std::string& name,
    const std::string& pattern
) {
    std::vector<std::string> args = {
        "collection", "add",
        path,
        "--name", name,
        "--glob", pattern
    };
    
    run_qmd(args, QmdCommandOptions{qmd_config_.update.command_timeout_ms});
}

void QmdMemoryManager::remove_collection(const std::string& name) {
    run_qmd({"collection", "remove", name}, 
        QmdCommandOptions{qmd_config_.update.command_timeout_ms});
}

void QmdMemoryManager::ensure_collection_path(const ManagedCollection& collection) {
    fs::create_directories(collection.path);
}

void QmdMemoryManager::run_update(const std::string& reason, bool force) {
    std::vector<std::string> args = {"update"};
    if (force) {
        args.push_back("--force");
    }
    
    run_qmd(args, QmdCommandOptions{qmd_config_.update.command_timeout_ms});
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
    
    // Normalize query for Han script (Chinese)
    std::string normalized_query = normalize_han_bm25_query(query);
    
    // Build QMD query args
    std::vector<std::string> args = {"query", normalized_query};
    
    if (opts.max_results > 0) {
        args.push_back("--limit");
        args.push_back(std::to_string(opts.max_results));
    }
    
    args.push_back("--json");
    
    // Run QMD query
    auto qmd_result = run_qmd(args, QmdCommandOptions{30000});
    
    // Parse results
    auto qmd_results = parse_qmd_query_json(qmd_result.stdout_data, qmd_result.stderr_data);
    
    // Convert to MemorySearchResult
    std::vector<MemorySearchResult> results;
    for (const auto& qmd_res : qmd_results) {
        auto mem_res = qmd_result_to_memory_result(qmd_res);
        
        // Filter by min_score
        if (mem_res.score >= opts.min_score) {
            results.push_back(mem_res);
        }
    }
    
    return results;
}

MemorySearchManager::ReadFileResult QmdMemoryManager::read_file(
    const std::string& rel_path,
    int from,
    int lines
) {
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
    MemoryProviderStatus status;
    status.backend = MemoryProviderStatus::Backend::QMD;
    status.provider = "qmd";
    status.workspace_dir = workspace_dir_;
    status.db_path = index_path_;
    status.sources = sources_;
    
    // Query index stats (simplified)
    try {
        // Could use `qmd status --json` to get real stats
        status.files = 0;
        status.chunks = 0;
    } catch (...) {
        // Ignore
    }
    
    return status;
}

void QmdMemoryManager::sync(const SyncParams& params) {
    run_update(params.reason.empty() ? "manual" : params.reason, params.force);
}

MemoryEmbeddingProbeResult QmdMemoryManager::probe_embedding_availability() {
    try {
        // Try to run a test query
        run_qmd({"query", "test", "--limit", "1"}, QmdCommandOptions{5000});
        return MemoryEmbeddingProbeResult{true, ""};
    } catch (const std::exception& e) {
        return MemoryEmbeddingProbeResult{false, e.what()};
    }
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
    const QmdCommandOptions& opts
) {
    // Build command
    std::ostringstream cmd;
    cmd << qmd_config_.qmd_binary_path;
    for (const auto& arg : args) {
        cmd << " \"" << arg << "\"";
    }
    
    // Build environment
    auto env = build_xdg_env();
    for (const auto& [key, value] : opts.env) {
        env[key] = value;
    }
    
    // Run command (simplified - real implementation would use proper subprocess handling)
    FILE* pipe = popen(cmd.str().c_str(), "r");
    if (!pipe) {
        throw std::runtime_error("Failed to run QMD command");
    }
    
    std::ostringstream stdout_stream;
    char buffer[1024];
    while (fgets(buffer, sizeof(buffer), pipe)) {
        stdout_stream << buffer;
    }
    
    int exit_code = pclose(pipe);
    
    return QmdCommandResult{
        stdout_stream.str(),
        "",  // stderr not captured in this simplified version
        exit_code
    };
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
    // Simplified JSON parsing (real implementation would use nlohmann/json or similar)
    std::vector<QmdQueryResult> results;
    
    // Check for "no results found" marker
    if (stdout_data.find("no results found") != std::string::npos ||
        stdout_data.find("No results found") != std::string::npos) {
        return results;
    }
    
    // Real implementation would properly parse JSON array
    // Format: [{"docid":"...","score":0.85,"collection":"...","file":"...","snippet":"..."}]
    
    return results;
}

MemorySearchResult QmdMemoryManager::qmd_result_to_memory_result(const QmdQueryResult& qmd_result) {
    MemorySearchResult result;
    result.path = qmd_result.file;
    result.start_line = 0;  // QMD doesn't provide line numbers in simple mode
    result.end_line = 0;
    result.score = qmd_result.score;
    result.snippet = qmd_result.snippet;
    
    // Determine source from collection
    auto it = collection_roots_.find(qmd_result.collection);
    result.source = (it != collection_roots_.end()) 
        ? it->second.kind 
        : MemorySource::MEMORY;
    
    return result;
}

std::string QmdMemoryManager::normalize_han_bm25_query(const std::string& query) {
    // Simplified implementation - real version would use proper Han script detection
    // and keyword extraction
    return query;
}

void QmdMemoryManager::export_session_files() {
    // Export session files to session exporter directory
    // (Implementation would sync sessions from session manager)
}

bool QmdMemoryManager::is_collection_missing_error(const std::string& error_msg) {
    return error_msg.find("not found") != std::string::npos ||
           error_msg.find("does not exist") != std::string::npos;
}

bool QmdMemoryManager::is_collection_already_exists_error(const std::string& error_msg) {
    return error_msg.find("already exists") != std::string::npos ||
           error_msg.find("duplicate") != std::string::npos;
}

// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
// Global Parse Function
// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━

std::vector<QmdQueryResult> parse_qmd_query_json(
    const std::string& stdout_data,
    const std::string& stderr_data
) {
    // Delegate to static implementation
    std::vector<QmdQueryResult> results;
    
    // Check for "no results found" marker
    if (stdout_data.find("no results found") != std::string::npos) {
        return results;
    }
    
    // Real implementation would parse JSON properly
    
    return results;
}

}  // namespace memory
}  // namespace hunterclaw
