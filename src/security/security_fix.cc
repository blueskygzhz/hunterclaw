#include <string>
#include <vector>
#include <map>
#include <set>
#include <optional>
#include <filesystem>
#include <fstream>
#include <sys/stat.h>
#include <unistd.h>
#include <nlohmann/json.hpp>

namespace hunterclaw {
namespace security {

// ============================================================================
// Security Fix Engine - 自动安全修复引擎
// 严格对齐 OpenClaw: src/security/fix.ts
// ============================================================================

namespace fs = std::filesystem;

// --- Types ---

struct SecurityFixAction {
    enum Kind { Chmod, ConfigWrite };
    Kind kind;
    std::string path;
    int mode = 0;           // for chmod
    bool ok = false;
    std::string skipped;    // reason if skipped
    std::string error;      // error if failed
};

struct SecurityFixResult {
    bool ok = true;
    std::string state_dir;
    std::string config_path;
    bool config_written = false;
    std::vector<std::string> changes;
    std::vector<SecurityFixAction> actions;
    std::vector<std::string> errors;
};

namespace {

// 对齐 OpenClaw: safeChmod(params)
SecurityFixAction safe_chmod(const std::string& path, int mode, bool require_dir) {
    SecurityFixAction action;
    action.kind = SecurityFixAction::Chmod;
    action.path = path;
    action.mode = mode;
    
    struct stat st;
    if (lstat(path.c_str(), &st) != 0) {
        // ENOENT
        action.skipped = "missing";
        return action;
    }
    
    if (S_ISLNK(st.st_mode)) {
        action.skipped = "symlink";
        return action;
    }
    
    if (require_dir && !S_ISDIR(st.st_mode)) {
        action.skipped = "not-a-directory";
        return action;
    }
    if (!require_dir && !S_ISREG(st.st_mode)) {
        action.skipped = "not-a-file";
        return action;
    }
    
    int current = st.st_mode & 0777;
    if (current == mode) {
        action.skipped = "already";
        return action;
    }
    
    if (chmod(path.c_str(), mode) != 0) {
        action.error = "chmod failed: " + std::string(strerror(errno));
        return action;
    }
    
    action.ok = true;
    return action;
}

// 对齐 OpenClaw: setGroupPolicyAllowlist(params)
void set_group_policy_allowlist(
    nlohmann::json& cfg,
    const std::string& channel,
    std::vector<std::string>& changes,
    std::set<std::string>& policy_flips
) {
    if (!cfg.contains("channels") || !cfg["channels"].contains(channel)) return;
    auto& section = cfg["channels"][channel];
    if (!section.is_object()) return;
    
    // Top-level groupPolicy
    if (section.contains("groupPolicy") && section["groupPolicy"] == "open") {
        section["groupPolicy"] = "allowlist";
        changes.push_back("channels." + channel + ".groupPolicy=open -> allowlist");
        policy_flips.insert("channels." + channel + ".");
    }
    
    // Per-account groupPolicy
    if (section.contains("accounts") && section["accounts"].is_object()) {
        for (auto& [account_id, account_val] : section["accounts"].items()) {
            if (!account_val.is_object()) continue;
            if (account_val.contains("groupPolicy") && account_val["groupPolicy"] == "open") {
                account_val["groupPolicy"] = "allowlist";
                changes.push_back("channels." + channel + ".accounts." + account_id + ".groupPolicy=open -> allowlist");
                policy_flips.insert("channels." + channel + ".accounts." + account_id + ".");
            }
        }
    }
}

// 对齐 OpenClaw: applyConfigFixes(params)
struct ConfigFixResult {
    nlohmann::json cfg;
    std::vector<std::string> changes;
    std::set<std::string> policy_flips;
};

ConfigFixResult apply_config_fixes(const nlohmann::json& config) {
    ConfigFixResult result;
    result.cfg = config; // deep copy
    
    // Fix redactSensitive
    if (result.cfg.contains("logging") && result.cfg["logging"].contains("redactSensitive")) {
        if (result.cfg["logging"]["redactSensitive"] == "off") {
            result.cfg["logging"]["redactSensitive"] = "tools";
            result.changes.push_back("logging.redactSensitive=off -> \"tools\"");
        }
    }
    
    // Fix groupPolicy=open for all channels
    for (const auto& channel : {"telegram", "whatsapp", "discord", "signal", "imessage", "slack", "msteams"}) {
        set_group_policy_allowlist(result.cfg, channel, result.changes, result.policy_flips);
    }
    
    return result;
}

} // anonymous namespace

/**
 * fix_security_footguns - 自动修复常见安全问题
 * 对齐 OpenClaw: fixSecurityFootguns(opts?)
 *
 * 修复项:
 * 1. State dir 权限 -> 700
 * 2. Config file 权限 -> 600
 * 3. groupPolicy=open -> allowlist
 * 4. logging.redactSensitive=off -> "tools"
 * 5. Credentials dir 权限 -> 700
 * 6. Auth profiles 权限 -> 600
 * 7. Sessions store 权限 -> 600
 */
SecurityFixResult fix_security_footguns(
    const std::string& state_dir_override,
    const std::string& config_path_override
) {
    SecurityFixResult result;
    
    // Resolve paths
    const char* home = getenv("HOME");
    std::string default_state = std::string(home ? home : "/tmp") + "/.openclaw/state";
    result.state_dir = state_dir_override.empty() ? default_state : state_dir_override;
    result.config_path = config_path_override.empty() 
        ? (std::string(home ? home : "/tmp") + "/.openclaw/config.yaml")
        : config_path_override;
    
    // 1. Fix state dir permissions
    result.actions.push_back(safe_chmod(result.state_dir, 0700, true));
    
    // 2. Fix config file permissions
    result.actions.push_back(safe_chmod(result.config_path, 0600, false));
    
    // 3. Fix config content (groupPolicy, redactSensitive)
    if (fs::exists(result.config_path)) {
        try {
            std::ifstream file(result.config_path);
            nlohmann::json config = nlohmann::json::parse(file);
            
            auto fixed = apply_config_fixes(config);
            result.changes = fixed.changes;
            
            if (!fixed.changes.empty()) {
                std::ofstream out(result.config_path);
                out << fixed.cfg.dump(2);
                result.config_written = true;
            }
        } catch (const std::exception& e) {
            result.errors.push_back(std::string("Config fix failed: ") + e.what());
        }
    }
    
    // 4. Fix credentials directory
    std::string creds_dir = result.state_dir + "/credentials";
    result.actions.push_back(safe_chmod(creds_dir, 0700, true));
    
    // Scan credential files
    if (fs::exists(creds_dir) && fs::is_directory(creds_dir)) {
        std::error_code ec;
        for (auto& entry : fs::directory_iterator(creds_dir, ec)) {
            if (ec) break;
            if (entry.is_regular_file() && entry.path().extension() == ".json") {
                result.actions.push_back(safe_chmod(entry.path().string(), 0600, false));
            }
        }
    }
    
    // 5. Fix agent state directories
    std::string agents_dir = result.state_dir + "/agents";
    if (fs::exists(agents_dir) && fs::is_directory(agents_dir)) {
        std::error_code ec;
        for (auto& agent_entry : fs::directory_iterator(agents_dir, ec)) {
            if (ec || !agent_entry.is_directory()) continue;
            
            std::string agent_root = agent_entry.path().string();
            result.actions.push_back(safe_chmod(agent_root, 0700, true));
            
            std::string agent_dir = agent_root + "/agent";
            result.actions.push_back(safe_chmod(agent_dir, 0700, true));
            
            // Auth profiles
            std::string auth_path = agent_dir + "/auth-profiles.json";
            result.actions.push_back(safe_chmod(auth_path, 0600, false));
            
            // Sessions dir
            std::string sessions_dir = agent_root + "/sessions";
            result.actions.push_back(safe_chmod(sessions_dir, 0700, true));
            
            // Sessions store
            std::string store_path = sessions_dir + "/sessions.json";
            result.actions.push_back(safe_chmod(store_path, 0600, false));
            
            // Session transcript files
            if (fs::exists(sessions_dir) && fs::is_directory(sessions_dir)) {
                std::error_code ec2;
                for (auto& se : fs::directory_iterator(sessions_dir, ec2)) {
                    if (ec2) break;
                    if (se.is_regular_file() && se.path().extension() == ".jsonl") {
                        result.actions.push_back(safe_chmod(se.path().string(), 0600, false));
                    }
                }
            }
        }
    }
    
    // Check overall result
    result.ok = result.errors.empty();
    return result;
}

}  // namespace security
}  // namespace hunterclaw
