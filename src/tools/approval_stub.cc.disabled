#include "hunterclaw/tools/approval.h"

namespace hunterclaw {
namespace tools {

// Stub constructor
ApprovalManager::ApprovalManager(const std::string& config_path) {
    // Stub: do nothing
}

// Stub destructor  
ApprovalManager::~ApprovalManager() {
    // Stub: do nothing
}

// Stub implementation for ApprovalManager::create
std::shared_ptr<ApprovalManager> ApprovalManager::create(
    const std::string& config_path
) {
    return std::make_shared<ApprovalManager>(config_path);
}

// Stub for load_config
core::Result<void> ApprovalManager::load_config() {
    return core::Result<void>::Ok();
}

// Stub for save_config
core::Result<void> ApprovalManager::save_config() {
    return core::Result<void>::Ok();
}

// Stub for get_agent_config
AgentApprovalConfig ApprovalManager::get_agent_config(
    const std::string& agent_id
) const {
    // Return permissive default config
    AgentApprovalConfig config;
    config.security = ExecSecurity::FULL;  // Allow all commands
    config.ask = ExecAsk::OFF;  // Never ask
    config.auto_allow_skills = true;  // Auto-allow skills
    return config;
}

// Stub for set_agent_config
void ApprovalManager::set_agent_config(
    const std::string& agent_id,
    const AgentApprovalConfig& config
) {
    // Stub: do nothing
}

// Stub for add_allowlist_entry
void ApprovalManager::add_allowlist_entry(
    const std::string& agent_id,
    const AllowlistEntry& entry
) {
    // Stub: do nothing
}

// Stub for remove_allowlist_entry
void ApprovalManager::remove_allowlist_entry(
    const std::string& agent_id,
    const std::string& entry_id
) {
    // Stub: do nothing
}

// Stub for is_allowlisted
bool ApprovalManager::is_allowlisted(
    const std::string& agent_id,
    const std::string& command
) const {
    // Stub: always return true (auto-approve)
    return true;
}

// Stub for match_allowlist
std::vector<AllowlistEntry> ApprovalManager::match_allowlist(
    const std::string& agent_id,
    const std::string& command
) const {
    // Stub: return empty list
    return {};
}

// Stub for request_approval
core::Result<ApprovalRequest> ApprovalManager::request_approval(const ApprovalRequest& request) {
    // Auto-approve: immediately return the request back
    return core::Result<ApprovalRequest>::Ok(request);
}

// Stub for wait_decision
core::Result<ApprovalResponse> ApprovalManager::wait_decision(
    const std::string& request_id
) {
    // Auto-approve: return immediate approval
    ApprovalResponse response;
    response.id = request_id;
    response.decision = ApprovalDecision::ALLOW_ONCE;
    response.resolved_by = "auto";
    response.resolved_at_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()
    ).count();
    return core::Result<ApprovalResponse>::Ok(response);
}

// Stub for build_approval_request
ApprovalRequest build_approval_request(
    const std::string& agent_id,
    const std::vector<std::string>& command_parts,
    const std::string& cwd,
    const std::string& /* stdin_data */,
    const std::string& /* context */,
    const AgentApprovalConfig& config
) {
    ApprovalRequest request;
    request.id = "req_" + std::to_string(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()
        ).count()
    );
    request.agent_id = agent_id;
    request.command_argv = command_parts;
    request.cwd = cwd;
    request.security = config.security;
    request.ask = config.ask;
    request.created_at_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()
    ).count();
    request.expires_at_ms = request.created_at_ms + request.timeout_ms;
    
    // Build command string from argv
    if (!command_parts.empty()) {
        request.command = command_parts[0];
        for (size_t i = 1; i < command_parts.size(); i++) {
            request.command += " " + command_parts[i];
        }
    }
    
    return request;
}

} // namespace tools
} // namespace hunterclaw
