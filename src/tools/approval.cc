// HunterClaw - High-Performance AI Agent Platform (C++)
// Copyright (c) 2025 HunterClaw Team
// 
// Tool Approval System Implementation (100% 对齐 OpenClaw)

#include "hunterclaw/tools/approval.h"
#include "hunterclaw/core/utils.h"
#include <fstream>
#include <sstream>
#include <algorithm>
#include <filesystem>
#include <fnmatch.h>
#include <openssl/md5.h>

namespace fs = std::filesystem;

namespace hunterclaw {
namespace tools {

// ============================================================================
// ApprovalManager Implementation
// ============================================================================

std::shared_ptr<ApprovalManager> ApprovalManager::create(const std::string& config_path) {
    return std::make_shared<ApprovalManager>(config_path);
}

ApprovalManager::ApprovalManager(const std::string& config_path)
    : config_path_(config_path.empty() ? default_config_path() : config_path) {
    load_config();
}

ApprovalManager::~ApprovalManager() {
    // 清理所有等待的条件变量
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& [id, cv] : cv_map_) {
        cv.notify_all();
    }
}

std::string ApprovalManager::default_config_path() const {
    const char* home = std::getenv("HOME");
    if (!home) home = "/tmp";
    return std::string(home) + "/.hunterclaw/approvals.json";
}

core::Result<void> ApprovalManager::load_config() {
    std::lock_guard<std::mutex> lock(mutex_);
    
    if (!fs::exists(config_path_)) {
        // 创建默认配置
        config_.version = 1;
        config_.defaults.security = ExecSecurity::ALLOWLIST;
        config_.defaults.ask = ExecAsk::ON_MISS;
        config_.defaults.ask_fallback = ExecSecurity::DENY;  // 对齐 OpenClaw DEFAULT_ASK_FALLBACK = "deny"
        config_.defaults.auto_allow_skills = false;
        
        save_config();
        return core::Result<void>::Ok();
    }
    
    std::ifstream file(config_path_);
    if (!file) {
        return core::Result<void>::Err("Failed to open config file: " + config_path_);
    }
    
    std::stringstream buffer;
    buffer << file.rdbuf();
    std::string content = buffer.str();
    
    try {
        auto json = nlohmann::json::parse(content);
        
        config_.version = json["version"].get<int>();
        
        // 解析 defaults
        if (json.contains("defaults")) {
            auto& defaults = json["defaults"];
            auto security_str = defaults["security"].get<std::string>();
            if (security_str == "deny") config_.defaults.security = ExecSecurity::DENY;
            else if (security_str == "allowlist") config_.defaults.security = ExecSecurity::ALLOWLIST;
            else if (security_str == "full") config_.defaults.security = ExecSecurity::FULL;
            
            auto ask_str = defaults["ask"].get<std::string>();
            if (ask_str == "off") config_.defaults.ask = ExecAsk::OFF;
            else if (ask_str == "on-miss") config_.defaults.ask = ExecAsk::ON_MISS;
            else if (ask_str == "always") config_.defaults.ask = ExecAsk::ALWAYS;
            
            // 对齐 OpenClaw: askFallback (ExecSecurity 类型)
            if (defaults.contains("askFallback") && defaults["askFallback"].is_string()) {
                auto fallback_str = defaults["askFallback"].get<std::string>();
                if (fallback_str == "deny") config_.defaults.ask_fallback = ExecSecurity::DENY;
                else if (fallback_str == "allowlist") config_.defaults.ask_fallback = ExecSecurity::ALLOWLIST;
                else if (fallback_str == "full") config_.defaults.ask_fallback = ExecSecurity::FULL;
            }
            
            if (defaults.contains("autoAllowSkills")) {
                config_.defaults.auto_allow_skills = defaults["autoAllowSkills"].get<bool>();
            }
        }
        
        // 解析 agents 配置
        if (json.contains("agents")) {
            auto& agents = json["agents"];
            for (auto& [agent_id, agent_cfg] : agents.items()) {
                AgentApprovalConfig cfg = config_.defaults;
                
                if (agent_cfg.contains("security")) {
                    auto sec = agent_cfg["security"].get<std::string>();
                    if (sec == "deny") cfg.security = ExecSecurity::DENY;
                    else if (sec == "allowlist") cfg.security = ExecSecurity::ALLOWLIST;
                    else if (sec == "full") cfg.security = ExecSecurity::FULL;
                }
                
                if (agent_cfg.contains("ask")) {
                    auto ask = agent_cfg["ask"].get<std::string>();
                    if (ask == "off") cfg.ask = ExecAsk::OFF;
                    else if (ask == "on-miss") cfg.ask = ExecAsk::ON_MISS;
                    else if (ask == "always") cfg.ask = ExecAsk::ALWAYS;
                }
                
                if (agent_cfg.contains("allowlist")) {
                    for (auto& entry : agent_cfg["allowlist"]) {
                        AllowlistEntry e;
                        e.id = entry["id"].get<std::string>();
                        e.pattern = entry["pattern"].get<std::string>();
                        e.created_at_ms = entry.contains("createdAt") ? 
                            entry["createdAt"].get<int64_t>() : 0;
                        cfg.allowlist.push_back(e);
                    }
                }
                
                config_.agents[agent_id] = cfg;
            }
        }
        
        return core::Result<void>::Ok();
    } catch (const std::exception& e) {
        return core::Result<void>::Err("Failed to parse config: " + std::string(e.what()));
    }
}

core::Result<void> ApprovalManager::save_config() {
    // 确保目录存在
    fs::path path(config_path_);
    fs::create_directories(path.parent_path());
    
    std::lock_guard<std::mutex> lock(mutex_);
    
    nlohmann::json json;
    json["version"] = static_cast<int64_t>(config_.version);
    
    // defaults
    nlohmann::json defaults;
    defaults["security"] = (config_.defaults.security == ExecSecurity::DENY ? "deny" :
                            config_.defaults.security == ExecSecurity::ALLOWLIST ? "allowlist" : "full");
    defaults["ask"] = (config_.defaults.ask == ExecAsk::OFF ? "off" :
                       config_.defaults.ask == ExecAsk::ON_MISS ? "on-miss" : "always");
    // 对齐 OpenClaw: askFallback (ExecSecurity 类型)
    defaults["askFallback"] = (config_.defaults.ask_fallback == ExecSecurity::DENY ? "deny" :
                                config_.defaults.ask_fallback == ExecSecurity::ALLOWLIST ? "allowlist" : "full");
    defaults["autoAllowSkills"] = config_.defaults.auto_allow_skills;
    json["defaults"] = defaults;
    
    // agents
    nlohmann::json agents;
    for (const auto& [agent_id, cfg] : config_.agents) {
        nlohmann::json agent_cfg;
        agent_cfg["security"] = (cfg.security == ExecSecurity::DENY ? "deny" :
                                 cfg.security == ExecSecurity::ALLOWLIST ? "allowlist" : "full");
        agent_cfg["ask"] = (cfg.ask == ExecAsk::OFF ? "off" :
                            cfg.ask == ExecAsk::ON_MISS ? "on-miss" : "always");
        
        nlohmann::json allowlist = nlohmann::json::array();
        for (const auto& entry : cfg.allowlist) {
            nlohmann::json e;
            e["id"] = entry.id;
            e["pattern"] = entry.pattern;
            e["createdAt"] = entry.created_at_ms;
            allowlist.push_back(e);
        }
        agent_cfg["allowlist"] = allowlist;
        
        agents[agent_id] = agent_cfg;
    }
    json["agents"] = agents;
    
    std::ofstream file(config_path_);
    if (!file) {
        return core::Result<void>::Err("Failed to write config file: " + config_path_);
    }
    
    file << json.dump(2);
    return core::Result<void>::Ok();
}

AgentApprovalConfig ApprovalManager::get_agent_config(const std::string& agent_id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = config_.agents.find(agent_id);
    if (it != config_.agents.end()) {
        return it->second;
    }
    return config_.defaults;
}

void ApprovalManager::set_agent_config(const std::string& agent_id, 
                                        const AgentApprovalConfig& config) {
    std::lock_guard<std::mutex> lock(mutex_);
    config_.agents[agent_id] = config;
    save_config();
}

void ApprovalManager::add_allowlist_entry(const std::string& agent_id, 
                                           const AllowlistEntry& entry) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto& cfg = config_.agents[agent_id];
    cfg.allowlist.push_back(entry);
    save_config();
}

void ApprovalManager::remove_allowlist_entry(const std::string& agent_id, 
                                              const std::string& entry_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = config_.agents.find(agent_id);
    if (it != config_.agents.end()) {
        auto& allowlist = it->second.allowlist;
        allowlist.erase(
            std::remove_if(allowlist.begin(), allowlist.end(),
                [&entry_id](const AllowlistEntry& e) { return e.id == entry_id; }),
            allowlist.end()
        );
        save_config();
    }
}

bool ApprovalManager::glob_match(const std::string& pattern, const std::string& text) const {
    return fnmatch(pattern.c_str(), text.c_str(), 0) == 0;
}

bool ApprovalManager::is_allowlisted(const std::string& agent_id, 
                                      const std::string& command) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = config_.agents.find(agent_id);
    if (it == config_.agents.end()) {
        return false;
    }
    
    for (const auto& entry : it->second.allowlist) {
        if (glob_match(entry.pattern, command)) {
            return true;
        }
    }
    return false;
}

std::vector<AllowlistEntry> ApprovalManager::match_allowlist(
    const std::string& agent_id, 
    const std::string& command) const {
    
    std::vector<AllowlistEntry> matches;
    std::lock_guard<std::mutex> lock(mutex_);
    
    auto it = config_.agents.find(agent_id);
    if (it != config_.agents.end()) {
        for (const auto& entry : it->second.allowlist) {
            if (glob_match(entry.pattern, command)) {
                matches.push_back(entry);
            }
        }
    }
    return matches;
}

core::Result<ApprovalRequest> ApprovalManager::request_approval(
    const ApprovalRequest& request) {
    
    std::lock_guard<std::mutex> lock(mutex_);
    
    // 检查是否已存在
    if (pending_requests_.find(request.id) != pending_requests_.end()) {
        return core::Result<ApprovalRequest>::Err("Approval request already exists: " + request.id);
    }
    
    // 存储请求
    pending_requests_[request.id] = request;
    
    // 通知回调
    notify_callbacks(request, ApprovalStatus::ACCEPTED);
    
    return core::Result<ApprovalRequest>::Ok(request);
}

core::Result<ApprovalResponse> ApprovalManager::wait_decision(const std::string& request_id) {
    std::unique_lock<std::mutex> lock(mutex_);
    
    auto req_it = pending_requests_.find(request_id);
    if (req_it == pending_requests_.end()) {
        return core::Result<ApprovalResponse>::Err("Approval request not found: " + request_id);
    }
    
    int64_t timeout_ms = req_it->second.timeout_ms;
    lock.unlock();
    
    return wait_decision_timeout(request_id, timeout_ms);
}

core::Result<ApprovalResponse> ApprovalManager::wait_decision_timeout(
    const std::string& request_id,
    int64_t timeout_ms) {
    
    std::unique_lock<std::mutex> lock(mutex_);
    
    auto req_it = pending_requests_.find(request_id);
    if (req_it == pending_requests_.end()) {
        return core::Result<ApprovalResponse>::Err("Approval request not found: " + request_id);
    }
    
    // 检查是否已决策
    auto resp_it = resolved_responses_.find(request_id);
    if (resp_it != resolved_responses_.end()) {
        return core::Result<ApprovalResponse>::Ok(resp_it->second);
    }
    
    // 创建条件变量
    auto& cv = cv_map_[request_id];
    
    // 等待决策或超时
    auto timeout = std::chrono::milliseconds(timeout_ms);
    bool timed_out = !cv.wait_for(lock, timeout, [this, &request_id]() {
        return resolved_responses_.find(request_id) != resolved_responses_.end();
    });
    
    if (timed_out) {
        // 超时，清理请求
        pending_requests_.erase(request_id);
        cv_map_.erase(request_id);
        
        ApprovalResponse response;
        response.id = request_id;
        response.decision = ApprovalDecision::TIMEOUT;
        response.resolved_at_ms = core::utils::current_timestamp_ms();
        response.reason = "Approval timeout";
        
        return core::Result<ApprovalResponse>::Ok(response);
    }
    
    // 获取决策
    auto response = resolved_responses_[request_id];
    
    // 清理
    pending_requests_.erase(request_id);
    resolved_responses_.erase(request_id);
    cv_map_.erase(request_id);
    
    return core::Result<ApprovalResponse>::Ok(response);
}

core::Result<void> ApprovalManager::resolve_approval(
    const std::string& request_id,
    ApprovalDecision decision,
    const std::string& resolved_by) {
    
    std::lock_guard<std::mutex> lock(mutex_);
    
    auto req_it = pending_requests_.find(request_id);
    if (req_it == pending_requests_.end()) {
        return core::Result<void>::Err("Approval request not found: " + request_id);
    }
    
    // 创建响应
    ApprovalResponse response;
    response.id = request_id;
    response.decision = decision;
    response.resolved_by = resolved_by;
    response.resolved_at_ms = core::utils::current_timestamp_ms();
    
    // 如果是 ALLOW_ALWAYS，添加到白名单
    // 对齐 OpenClaw: resolveAllowAlwaysPatterns() — 提取内层可执行文件路径
    if (decision == ApprovalDecision::ALLOW_ALWAYS) {
        const auto& request = req_it->second;
        
        // 生成白名单模式 (对齐 OpenClaw: 当 shell wrapper 时持久化内层命令)
        std::vector<std::string> patterns;
        if (is_shell_wrapper(request.command_argv) && request.command_argv.size() >= 3) {
            // Shell wrapper (e.g., "zsh -lc cmd"): 提取内层命令的可执行文件
            // 对齐 OpenClaw resolveAllowAlwaysPatterns(): persist inner executable
            for (size_t i = 0; i < request.command_argv.size(); ++i) {
                if (request.command_argv[i] == "-c" && i + 1 < request.command_argv.size()) {
                    // 使用 -c 后面的命令作为 pattern 来源
                    // 简化版：取内层命令的第一个 token
                    const std::string& inner = request.command_argv[i + 1];
                    auto space_pos = inner.find(' ');
                    if (space_pos != std::string::npos) {
                        patterns.push_back(inner.substr(0, space_pos));
                    } else {
                        patterns.push_back(inner);
                    }
                    break;
                }
            }
        }
        
        // 如果未提取到内层命令，使用 argv[0] 或完整命令
        if (patterns.empty()) {
            if (!request.command_argv.empty()) {
                patterns.push_back(request.command_argv[0]);
            } else {
                patterns.push_back(request.command);
            }
        }
        
        auto& agent_cfg = config_.agents[request.agent_id];
        for (const auto& pattern : patterns) {
            if (pattern.empty()) continue;
            // 检查是否已存在相同 pattern
            bool exists = false;
            for (const auto& e : agent_cfg.allowlist) {
                if (e.pattern == pattern) { exists = true; break; }
            }
            if (!exists) {
                AllowlistEntry entry;
                entry.id = core::utils::generate_uuid();
                entry.pattern = pattern;
                entry.created_at_ms = core::utils::current_timestamp_ms();
                entry.created_by = resolved_by;
                agent_cfg.allowlist.push_back(entry);
            }
        }
        save_config();
    }
    
    // 存储响应
    resolved_responses_[request_id] = response;
    
    // 通知等待线程
    auto cv_it = cv_map_.find(request_id);
    if (cv_it != cv_map_.end()) {
        cv_it->second.notify_all();
    }
    
    // 通知回调
    notify_callbacks(req_it->second, ApprovalStatus::RESOLVED);
    
    return core::Result<void>::Ok();
}

core::Result<void> ApprovalManager::cancel_approval(const std::string& request_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    auto req_it = pending_requests_.find(request_id);
    if (req_it == pending_requests_.end()) {
        return core::Result<void>::Err("Approval request not found: " + request_id);
    }
    
    // 通知回调
    notify_callbacks(req_it->second, ApprovalStatus::CANCELLED);
    
    // 清理
    pending_requests_.erase(request_id);
    auto cv_it = cv_map_.find(request_id);
    if (cv_it != cv_map_.end()) {
        cv_it->second.notify_all();
        cv_map_.erase(cv_it);
    }
    
    return core::Result<void>::Ok();
}

std::vector<ApprovalRequest> ApprovalManager::get_pending_approvals() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<ApprovalRequest> result;
    for (const auto& [id, request] : pending_requests_) {
        result.push_back(request);
    }
    return result;
}

core::Result<ApprovalStatus> ApprovalManager::get_approval_status(
    const std::string& request_id) const {
    
    std::lock_guard<std::mutex> lock(mutex_);
    
    if (pending_requests_.find(request_id) != pending_requests_.end()) {
        return core::Result<ApprovalStatus>::Ok(ApprovalStatus::ACCEPTED);
    }
    
    if (resolved_responses_.find(request_id) != resolved_responses_.end()) {
        return core::Result<ApprovalStatus>::Ok(ApprovalStatus::RESOLVED);
    }
    
    return core::Result<ApprovalStatus>::Err("Approval request not found: " + request_id);
}

bool ApprovalManager::requires_approval(
    const std::string& agent_id,
    const std::string& command,
    const AgentApprovalConfig& config) const {
    
    // 如果是 FULL 模式且 ask 为 OFF，不需要审批
    if (config.security == ExecSecurity::FULL && config.ask == ExecAsk::OFF) {
        return false;
    }
    
    // 如果是 DENY 模式，总是需要审批（或直接拒绝）
    if (config.security == ExecSecurity::DENY) {
        return true;
    }
    
    // ALLOWLIST 模式
    if (config.security == ExecSecurity::ALLOWLIST) {
        bool in_allowlist = is_allowlisted(agent_id, command);
        
        if (config.ask == ExecAsk::ALWAYS) {
            return true;  // 总是询问
        } else if (config.ask == ExecAsk::ON_MISS) {
            return !in_allowlist;  // 不在白名单时询问
        }
    }
    
    return false;
}

// 严格对齐 OpenClaw requiresExecApproval({ ask, security, analysisOk, allowlistSatisfied })
// exec-approvals.ts:484-496
bool ApprovalManager::requires_exec_approval(
    ExecAsk ask,
    ExecSecurity security,
    bool analysis_ok,
    bool allowlist_satisfied) {
    
    return (
        ask == ExecAsk::ALWAYS ||
        (ask == ExecAsk::ON_MISS &&
         security == ExecSecurity::ALLOWLIST &&
         (!analysis_ok || !allowlist_satisfied))
    );
}

core::Result<ApprovalDecision> ApprovalManager::auto_decide(
    const std::string& agent_id,
    const std::string& command) const {
    
    auto config = get_agent_config(agent_id);
    
    // FULL 模式 + OFF -> 自动允许
    if (config.security == ExecSecurity::FULL && config.ask == ExecAsk::OFF) {
        return core::Result<ApprovalDecision>::Ok(ApprovalDecision::ALLOW_ONCE);
    }
    
    // DENY 模式 -> 自动拒绝
    if (config.security == ExecSecurity::DENY) {
        return core::Result<ApprovalDecision>::Ok(ApprovalDecision::DENY);
    }
    
    // ALLOWLIST 模式
    if (config.security == ExecSecurity::ALLOWLIST) {
        bool in_allowlist = is_allowlisted(agent_id, command);
        
        if (in_allowlist && config.ask == ExecAsk::ON_MISS) {
            return core::Result<ApprovalDecision>::Ok(ApprovalDecision::ALLOW_ONCE);
        }
    }
    
    // 需要人工决策
    return core::Result<ApprovalDecision>::Err("Requires manual approval");
}

void ApprovalManager::register_callback(ApprovalUpdateCallback callback) {
    std::lock_guard<std::mutex> lock(mutex_);
    callbacks_.push_back(callback);
}

void ApprovalManager::cleanup_expired() {
    std::lock_guard<std::mutex> lock(mutex_);
    
    int64_t now = core::utils::current_timestamp_ms();
    std::vector<std::string> expired_ids;
    
    for (const auto& [id, request] : pending_requests_) {
        if (now > request.expires_at_ms) {
            expired_ids.push_back(id);
        }
    }
    
    for (const auto& id : expired_ids) {
        notify_callbacks(pending_requests_[id], ApprovalStatus::EXPIRED);
        pending_requests_.erase(id);
        
        auto cv_it = cv_map_.find(id);
        if (cv_it != cv_map_.end()) {
            cv_it->second.notify_all();
            cv_map_.erase(cv_it);
        }
    }
}

void ApprovalManager::notify_callbacks(const ApprovalRequest& request, 
                                        ApprovalStatus status) {
    for (const auto& callback : callbacks_) {
        try {
            callback(request, status);
        } catch (...) {
            // 忽略回调异常
        }
    }
}

// ============================================================================
// Helper Functions
// ============================================================================

ApprovalRequest build_approval_request(
    const std::string& command,
    const std::vector<std::string>& argv,
    const std::string& cwd,
    const std::string& agent_id,
    const std::string& session_key,
    const AgentApprovalConfig& config) {
    
    ApprovalRequest request;
    request.id = core::utils::generate_uuid();
    request.command = command;
    request.command_argv = argv;
    request.cwd = cwd;
    request.host = "gateway";
    request.security = config.security;
    request.ask = config.ask;
    request.agent_id = agent_id;
    request.session_key = session_key;
    request.timeout_ms = 120000;  // 2 minutes
    request.two_phase = true;
    request.created_at_ms = core::utils::current_timestamp_ms();
    request.expires_at_ms = request.created_at_ms + request.timeout_ms;
    
    return request;
}

std::vector<std::string> generate_allowlist_patterns(
    const std::string& command,
    const std::vector<std::string>& argv,
    const std::string& cwd) {
    
    std::vector<std::string> patterns;
    
    if (argv.empty()) {
        patterns.push_back(command);
        return patterns;
    }
    
    // 完整命令
    patterns.push_back(command);
    
    // 命令 + 通配符参数
    if (argv.size() >= 1) {
        patterns.push_back(argv[0] + " *");
    }
    
    // 命令 + 第一个参数 + 通配符
    if (argv.size() >= 2) {
        patterns.push_back(argv[0] + " " + argv[1] + " *");
    }
    
    return patterns;
}

bool is_sensitive_command(const std::string& command) {
    static const std::vector<std::string> sensitive_patterns = {
        "rm -rf", "rm -fr", "rm -r", "rm -f",
        "sudo", "su ",
        "chmod 777", "chmod -R",
        "chown -R",
        "mkfs", "dd ",
        "curl *| sh", "wget *| bash",
        "> /dev/", "| tee /dev/"
    };
    
    for (const auto& pattern : sensitive_patterns) {
        if (command.find(pattern) != std::string::npos) {
            return true;
        }
    }
    
    return false;
}

bool is_shell_wrapper(const std::vector<std::string>& argv) {
    if (argv.empty()) return false;
    
    const std::string& cmd = argv[0];
    
    // 检查是否为 shell 包装器
    if (cmd == "sh" || cmd == "bash" || cmd == "zsh" || 
        cmd == "/bin/sh" || cmd == "/bin/bash" || cmd == "/bin/zsh") {
        
        // 检查是否有 -c 参数
        for (const auto& arg : argv) {
            if (arg == "-c") {
                return true;
            }
        }
    }
    
    // Windows cmd.exe
    if (cmd.find("cmd.exe") != std::string::npos || cmd.find("CMD.EXE") != std::string::npos) {
        for (const auto& arg : argv) {
            if (arg == "/c" || arg == "/C") {
                return true;
            }
        }
    }
    
    return false;
}

// ============================================================================
// Policy Merge Functions (对齐 OpenClaw exec-approvals.ts:547-554)
// ============================================================================

ExecSecurity min_security(ExecSecurity a, ExecSecurity b) {
    // 对齐 OpenClaw: const order: Record<ExecSecurity, number> = { deny: 0, allowlist: 1, full: 2 };
    // return order[a] <= order[b] ? a : b;
    static constexpr int order[] = { 0, 1, 2 };  // DENY=0, ALLOWLIST=1, FULL=2
    return order[static_cast<int>(a)] <= order[static_cast<int>(b)] ? a : b;
}

ExecAsk max_ask(ExecAsk a, ExecAsk b) {
    // 对齐 OpenClaw: const order: Record<ExecAsk, number> = { off: 0, "on-miss": 1, always: 2 };
    // return order[a] >= order[b] ? a : b;
    static constexpr int order[] = { 0, 1, 2 };  // OFF=0, ON_MISS=1, ALWAYS=2
    return order[static_cast<int>(a)] >= order[static_cast<int>(b)] ? a : b;
}

// ============================================================================
// Approval Decision Resolution
// 对齐 OpenClaw bash-tools.exec-host-shared.ts:141-170
// ============================================================================

BaseApprovalDecisionResult resolve_base_exec_approval_decision(
    const std::string& decision,
    ExecSecurity ask_fallback,
    bool obfuscation_detected) {
    
    BaseApprovalDecisionResult result;
    
    if (decision == "deny") {
        result.denied_reason = "user-denied";
        return result;
    }
    
    if (decision.empty()) {
        // No decision (null/timeout)
        result.timed_out = true;
        
        if (obfuscation_detected) {
            result.denied_reason = "approval-timeout (obfuscation-detected)";
            return result;
        }
        
        if (ask_fallback == ExecSecurity::FULL) {
            result.approved_by_ask = true;
            return result;
        }
        
        if (ask_fallback == ExecSecurity::DENY) {
            result.denied_reason = "approval-timeout";
            return result;
        }
        
        // ask_fallback == ALLOWLIST → timedOut=true, deniedReason="" (caller handles)
        return result;
    }
    
    // decision is present but not "deny" and not empty
    // (could be "allow-once" or "allow-always" — handled by caller)
    return result;
}

}  // namespace tools
}  // namespace hunterclaw
