// HunterClaw: commands/doctor_diagnostics.cc
// 严格对齐 OC: commands/doctor-config-flow.ts (221行) + doctor-auth.ts (359行)
//              + doctor-security.ts (359行) + doctor-state-integrity.ts (951行)
// 统一的 Doctor 自诊断核心模块
//
// OC 对应导出:
//   doctor-config-flow.ts    → loadAndMaybeMigrateDoctorConfig
//   doctor-auth.ts           → maybeRepairLegacyOAuthProfileIds, noteLegacyCodexProviderOverride,
//                               resolveUnusableProfileHint, formatOAuthRefreshFailureDoctorLine
//   doctor-security.ts       → noteSecurityWarnings
//   doctor-state-integrity.ts → detectLinuxSdBackedStateDir, formatLinuxSdBackedStateDirWarning,
//                                detectMacCloudSyncedStateDir, noteStateIntegrity, noteWorkspaceBackupTip

// 对齐 OC: commands/doctor 系列诊断模块的 C++ 实现
// 头文件依赖已有模块
#include <stdexcept>
#include <string>
#include <vector>
#include "hunterclaw/config/config_system.h"
#include "hunterclaw/security/audit.h"
#include "hunterclaw/infra/utilities.h"
#include <nlohmann/json.hpp>
#include <filesystem>
#include <fstream>
#include <algorithm>
#include <regex>
#include <sstream>
#include <chrono>
#include <optional>

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace hunterclaw {
namespace commands {
namespace doctor {

// =========================================================================
// 对齐 OC doctor-state-integrity.ts 辅助函数
// =========================================================================

static std::string count_label(int count, const std::string& singular,
                               const std::string& plural = "") {
    std::string p = plural.empty() ? singular + "s" : plural;
    return std::to_string(count) + " " + (count == 1 ? singular : p);
}

static std::string format_file_preview(const std::vector<std::string>& paths, int limit = 3) {
    std::vector<std::string> names;
    int n = std::min(static_cast<int>(paths.size()), limit);
    for (int i = 0; i < n; ++i) {
        names.push_back(fs::path(paths[i]).filename().string());
    }
    int remaining = static_cast<int>(paths.size()) - n;
    std::string joined;
    for (size_t i = 0; i < names.size(); ++i) {
        if (i > 0) joined += ", ";
        joined += names[i];
    }
    if (remaining > 0) {
        joined += ", and " + std::to_string(remaining) + " more";
    }
    return joined;
}

static bool exists_dir(const std::string& dir) {
    try {
        return fs::exists(dir) && fs::is_directory(dir);
    } catch (...) {
        return false;
    }
}

static bool exists_file(const std::string& file_path) {
    try {
        return fs::exists(file_path) && fs::is_regular_file(file_path);
    } catch (...) {
        return false;
    }
}

// =========================================================================
// 对齐 OC doctor-state-integrity.ts: LinuxSdBackedStateDir + detect
// =========================================================================

// 对齐 OC: parseLinuxMountInfo
struct LinuxMountInfoEntry {
    std::string mount_point;
    std::string fs_type;
    std::string source;
};

static std::vector<LinuxMountInfoEntry> parse_linux_mount_info(const std::string& raw) {
    std::vector<LinuxMountInfoEntry> entries;
    std::istringstream stream(raw);
    std::string line;
    while (std::getline(stream, line)) {
        auto sep_pos = line.find(" - ");
        if (sep_pos == std::string::npos) continue;
        // Before separator: fields[4] is mount_point
        std::string before = line.substr(0, sep_pos);
        std::string after = line.substr(sep_pos + 3);
        std::istringstream bs(before);
        std::vector<std::string> before_fields;
        std::string token;
        while (bs >> token) before_fields.push_back(token);
        if (before_fields.size() < 5) continue;
        std::string mount_point = before_fields[4];
        std::istringstream as(after);
        std::vector<std::string> after_fields;
        while (as >> token) after_fields.push_back(token);
        if (after_fields.size() < 2) continue;
        LinuxMountInfoEntry entry;
        entry.mount_point = mount_point;
        entry.fs_type = after_fields[0];
        entry.source = after_fields[1];
        entries.push_back(entry);
    }
    return entries;
}

static std::string try_read_linux_mount_info() {
    std::ifstream ifs("/proc/self/mountinfo");
    if (!ifs.is_open()) return "";
    std::string content((std::istreambuf_iterator<char>(ifs)),
                        std::istreambuf_iterator<char>());
    return content;
}

// 对齐 OC: SD/eMMC detection patterns
static const std::vector<std::string> SD_EMMC_DEVICE_PATTERNS = {
    "mmcblk", "mmc"
};

static bool is_sd_emmc_device(const std::string& source) {
    for (const auto& pattern : SD_EMMC_DEVICE_PATTERNS) {
        if (source.find(pattern) != std::string::npos) return true;
    }
    return false;
}

LinuxSdBackedStateDir detect_linux_sd_backed_state_dir(
    const std::string& state_dir,
    const std::string& platform
) {
    if (platform != "linux") return {};
    std::string mount_info_raw = try_read_linux_mount_info();
    if (mount_info_raw.empty()) return {};

    auto entries = parse_linux_mount_info(mount_info_raw);
    std::string resolved_path;
    try {
        resolved_path = fs::canonical(state_dir).string();
    } catch (...) {
        resolved_path = fs::absolute(state_dir).string();
    }

    // Find best mount for the path (longest prefix match)
    LinuxMountInfoEntry best_mount;
    size_t best_len = 0;
    for (const auto& entry : entries) {
        if (resolved_path.find(entry.mount_point) == 0 &&
            entry.mount_point.size() > best_len) {
            best_mount = entry;
            best_len = entry.mount_point.size();
        }
    }
    if (best_len == 0) return {};

    // Resolve device real path
    std::string device_real;
    try {
        device_real = fs::canonical(best_mount.source).string();
    } catch (...) {
        device_real = best_mount.source;
    }

    if (!is_sd_emmc_device(device_real) && !is_sd_emmc_device(best_mount.source)) {
        return {};
    }

    LinuxSdBackedStateDir result;
    result.path = state_dir;
    result.mount_point = best_mount.mount_point;
    result.fs_type = best_mount.fs_type;
    result.source = best_mount.source;
    return result;
}

std::string format_linux_sd_backed_state_dir_warning(
    const std::string& display_state_dir,
    const LinuxSdBackedStateDir& info
) {
    std::string display_mount = info.mount_point == "/" ? "/" : info.mount_point;
    std::vector<std::string> lines = {
        "- State directory appears to be on SD/eMMC storage (" + display_state_dir +
            "; device " + info.source + ", fs " + info.fs_type +
            ", mount " + display_mount + ").",
        "- SD/eMMC media can be slower for random I/O and wear faster under session/log churn.",
        "- For better startup and state durability, prefer SSD/NVMe for state dir."
    };
    std::string result;
    for (size_t i = 0; i < lines.size(); ++i) {
        if (i > 0) result += "\n";
        result += lines[i];
    }
    return result;
}

// 对齐 OC: detectMacCloudSyncedStateDir
bool detect_mac_cloud_synced_state_dir(const std::string& state_dir) {
#ifdef __APPLE__
    std::string home = infra::get_home_dir();
    // iCloud patterns
    std::vector<std::string> cloud_patterns = {
        "/Library/Mobile Documents/",
        "/Library/CloudStorage/",
        "/Google Drive/",
        "/Dropbox/",
        "/OneDrive/"
    };
    std::string resolved;
    try {
        resolved = fs::canonical(state_dir).string();
    } catch (...) {
        resolved = state_dir;
    }
    for (const auto& pattern : cloud_patterns) {
        if (resolved.find(pattern) != std::string::npos) return true;
    }
#else
    (void)state_dir;
#endif
    return false;
}

// =========================================================================
// 对齐 OC doctor-security.ts: noteSecurityWarnings
// =========================================================================

// 对齐 OC: execSecurityRank
static int exec_security_rank(const std::string& value) {
    if (value == "deny") return 0;
    if (value == "allowlist") return 1;
    if (value == "full") return 2;
    return -1;
}

// 对齐 OC: execAskRank
static int exec_ask_rank(const std::string& value) {
    if (value == "off") return 0;
    if (value == "on-miss") return 1;
    if (value == "always") return 2;
    return -1;
}

// 对齐 OC: collectImplicitHeartbeatDirectPolicyWarnings
static std::vector<std::string> collect_implicit_heartbeat_direct_policy_warnings(
    const json& cfg
) {
    std::vector<std::string> warnings;
    auto check = [&](const std::string& label, const json& heartbeat, const std::string& path_hint) {
        if (heartbeat.is_null()) return;
        auto target = heartbeat.value("target", "none");
        if (target == "none") return;
        if (heartbeat.contains("directPolicy")) return;
        warnings.push_back("- " + label + ": heartbeat delivery is configured while " +
                           path_hint + " is unset.");
        warnings.push_back("  Heartbeat now allows direct/DM targets by default. "
                           "Set it explicitly to \"allow\" or \"block\" to pin upgrade behavior.");
    };

    if (cfg.contains("agents") && cfg["agents"].contains("defaults") &&
        cfg["agents"]["defaults"].contains("heartbeat")) {
        check("Heartbeat defaults", cfg["agents"]["defaults"]["heartbeat"],
              "agents.defaults.heartbeat.directPolicy");
    }
    if (cfg.contains("agents") && cfg["agents"].contains("list")) {
        for (const auto& agent : cfg["agents"]["list"]) {
            if (!agent.contains("heartbeat")) continue;
            std::string agent_id = agent.value("id", "unknown");
            check("Heartbeat agent \"" + agent_id + "\"", agent["heartbeat"],
                  "heartbeat.directPolicy for agent \"" + agent_id + "\"");
        }
    }
    return warnings;
}

// 对齐 OC: collectExecPolicyConflictWarnings
static std::vector<std::string> collect_exec_policy_conflict_warnings(const json& cfg) {
    std::vector<std::string> warnings;
    // Check for global vs per-agent exec policy conflicts
    std::string global_security = "allowlist";
    std::string global_ask = "on-miss";
    if (cfg.contains("approvals") && cfg["approvals"].contains("exec")) {
        global_security = cfg["approvals"]["exec"].value("security", "allowlist");
        global_ask = cfg["approvals"]["exec"].value("ask", "on-miss");
    }
    if (cfg.contains("agents") && cfg["agents"].contains("list")) {
        for (const auto& agent : cfg["agents"]["list"]) {
            if (!agent.contains("exec")) continue;
            auto agent_security = agent["exec"].value("security", global_security);
            auto agent_ask = agent["exec"].value("ask", global_ask);
            int sec_rank = exec_security_rank(agent_security);
            int global_rank = exec_security_rank(global_security);
            if (sec_rank > global_rank) {
                warnings.push_back("- Agent \"" + agent.value("id", "?") +
                    "\": exec.security=\"" + agent_security +
                    "\" is more permissive than global \"" + global_security + "\".");
            }
        }
    }
    return warnings;
}

// 对齐 OC: collectDurableExecApprovalWarnings
static std::vector<std::string> collect_durable_exec_approval_warnings(const json& cfg) {
    std::vector<std::string> warnings;
    // Check if durable approvals are enabled (persistent exec approval rules)
    if (cfg.contains("approvals") && cfg["approvals"].contains("exec")) {
        auto& exec = cfg["approvals"]["exec"];
        if (exec.value("persist", false) && exec.value("security", "allowlist") != "deny") {
            warnings.push_back("- Persistent exec approval rules are enabled. "
                "Approved commands remain trusted across sessions.");
            warnings.push_back("  Review stored approvals periodically.");
        }
    }
    return warnings;
}

std::vector<DoctorDiagnosticNote> note_security_warnings(const json& cfg) {
    std::vector<DoctorDiagnosticNote> notes;
    std::vector<std::string> warnings;

    // 对齐 OC: approvals.exec.enabled=false check
    if (cfg.contains("approvals") && cfg["approvals"].contains("exec") &&
        cfg["approvals"]["exec"].value("enabled", true) == false) {
        warnings.push_back("- Note: approvals.exec.enabled=false disables approval forwarding only.");
        warnings.push_back("  Host exec gating still comes from local exec-approvals.json.");
        warnings.push_back("  Check local policy with: hunterclaw approvals get --gateway");
    }

    auto heartbeat_warnings = collect_implicit_heartbeat_direct_policy_warnings(cfg);
    warnings.insert(warnings.end(), heartbeat_warnings.begin(), heartbeat_warnings.end());

    auto exec_warnings = collect_exec_policy_conflict_warnings(cfg);
    warnings.insert(warnings.end(), exec_warnings.begin(), exec_warnings.end());

    auto durable_warnings = collect_durable_exec_approval_warnings(cfg);
    warnings.insert(warnings.end(), durable_warnings.begin(), durable_warnings.end());

    // 对齐 OC: GATEWAY NETWORK EXPOSURE CHECK
    std::string bind_mode = "loopback";
    if (cfg.contains("gateway") && cfg["gateway"].contains("bind")) {
        bind_mode = cfg["gateway"].value("bind", "loopback");
    }
    bool is_loopback = (bind_mode == "loopback");
    bool has_auth = cfg.contains("gateway") &&
                    cfg["gateway"].contains("auth") &&
                    !cfg["gateway"]["auth"].value("token", "").empty();

    if (!is_loopback && !has_auth) {
        warnings.push_back("- Gateway is bound to non-loopback (" + bind_mode +
                           ") without auth token configured.");
        warnings.push_back("  Anyone on the network can access the gateway. "
                           "Set gateway.auth.token or bind to loopback.");
    }

    if (!warnings.empty()) {
        DoctorDiagnosticNote note;
        note.title = "Security";
        note.severity = "warn";
        for (const auto& w : warnings) {
            if (!note.message.empty()) note.message += "\n";
            note.message += w;
        }
        notes.push_back(note);
    }
    return notes;
}

// =========================================================================
// 对齐 OC doctor-auth.ts
// =========================================================================

std::string resolve_unusable_profile_hint(const std::string& kind, const std::string& reason) {
    if (kind == "disabled") {
        if (reason == "billing") {
            return "Top up credits (provider billing) or switch provider.";
        }
        if (reason == "auth_permanent" || reason == "auth") {
            return "Refresh or replace credentials, then retry.";
        }
    }
    return "Wait for cooldown or switch provider.";
}

std::string format_oauth_refresh_failure_doctor_line(
    const std::string& profile_id,
    const std::string& provider,
    const std::string& message
) {
    // 对齐 OC: classifyOAuthRefreshFailure → format
    std::string reason = "refresh failed";
    if (message.find("refresh_token_reused") != std::string::npos) reason = "refresh_token_reused";
    else if (message.find("invalid_grant") != std::string::npos) reason = "invalid_grant";
    else if (message.find("sign in again") != std::string::npos) reason = "sign in again";
    else if (message.find("revoked") != std::string::npos) reason = "revoked";
    else if (message.find("invalid refresh token") != std::string::npos) reason = "invalid refresh token";

    return "- Auth profile \"" + profile_id + "\" (" + provider + "): " + reason +
           ". Run `hunterclaw login " + provider + "` to re-authenticate.";
}

// =========================================================================
// 对齐 OC doctor-config-flow.ts: loadAndMaybeMigrateDoctorConfig
// =========================================================================

// 对齐 OC: hasLegacyInternalHookHandlers
static bool has_legacy_internal_hook_handlers(const json& raw) {
    if (!raw.contains("hooks")) return false;
    if (!raw["hooks"].contains("internal")) return false;
    if (!raw["hooks"]["internal"].contains("handlers")) return false;
    auto& handlers = raw["hooks"]["internal"]["handlers"];
    return handlers.is_array() && !handlers.empty();
}

DoctorConfigFlowResult load_and_maybe_migrate_doctor_config(
    const DoctorOptions& options
) {
    DoctorConfigFlowResult result;
    result.success = true;

    // Step 1: Load config snapshot
    json cfg;
    std::string config_path = config::get_config_path();
    try {
        cfg = config::load_config_snapshot(config_path);
    } catch (const std::exception& e) {
        result.success = false;
        result.error = std::string("Failed to load config: ") + e.what();
        return result;
    }
    result.config = cfg;

    bool should_repair = options.repair || options.yes;

    // Step 2: 对齐 OC: legacy compatibility step
    auto legacy_issues = config::find_legacy_issues(cfg);
    for (const auto& issue : legacy_issues) {
        result.notes.push_back({"Legacy config keys detected", "info",
            "- " + issue.path + ": " + issue.message});
    }

    // Step 3: 对齐 OC: hasLegacyInternalHookHandlers
    if (has_legacy_internal_hook_handlers(cfg)) {
        result.notes.push_back({"Legacy config keys detected", "warn",
            "- hooks.internal.handlers: legacy inline hook modules are no longer part of the public config surface.\n"
            "- Migrate each entry to a managed hook directory with HOOK.md + handler, then enable through hooks.internal.entries."});
    }

    // Step 4: 对齐 OC: normalizeCompatibilityConfigValues
    auto normalized = config::normalize_compat_values(cfg);
    if (!normalized.changes.empty()) {
        std::string changes_text;
        for (const auto& c : normalized.changes) {
            if (!changes_text.empty()) changes_text += "\n";
            changes_text += c;
        }
        result.notes.push_back({"Doctor changes", "info", changes_text});
        if (should_repair) {
            cfg = normalized.config;
            result.config = cfg;
            result.pending_changes = true;
        }
    }

    // Step 5: 对齐 OC: noteSecurityWarnings
    auto security_notes = note_security_warnings(cfg);
    result.notes.insert(result.notes.end(), security_notes.begin(), security_notes.end());

    // Step 6: 对齐 OC: noteStateIntegrity
    auto state_notes = note_state_integrity(cfg, config_path);
    result.notes.insert(result.notes.end(), state_notes.begin(), state_notes.end());

    return result;
}

// =========================================================================
// 对齐 OC doctor-state-integrity.ts: noteStateIntegrity
// =========================================================================

std::vector<DoctorDiagnosticNote> note_state_integrity(
    const json& cfg,
    const std::string& config_path
) {
    std::vector<DoctorDiagnosticNote> notes;

    std::string state_dir = infra::resolve_state_dir();
    std::string sessions_dir = state_dir + "/sessions";
    std::string oauth_dir = state_dir + "/oauth";

    // 对齐 OC: State directory exists check
    if (!exists_dir(state_dir)) {
        notes.push_back({"State directory", "warn",
            "- State directory does not exist: " + state_dir + "\n"
            "- Run `hunterclaw doctor --fix` to create it."});
    }

    // 对齐 OC: detectLinuxSdBackedStateDir
#ifdef __linux__
    auto sd_info = detect_linux_sd_backed_state_dir(state_dir, "linux");
    if (!sd_info.path.empty()) {
        notes.push_back({"Storage", "warn",
            format_linux_sd_backed_state_dir_warning(state_dir, sd_info)});
    }
#endif

    // 对齐 OC: detectMacCloudSyncedStateDir
    if (detect_mac_cloud_synced_state_dir(state_dir)) {
        notes.push_back({"Storage", "warn",
            "- State directory appears to be inside a cloud-synced folder.\n"
            "- Cloud sync can cause lock contention, session corruption, and bandwidth waste.\n"
            "- Move state dir outside cloud-synced paths."});
    }

    // 对齐 OC: OAuth directory check
    if (exists_dir(oauth_dir)) {
        // Count stored profiles
        int profile_count = 0;
        try {
            for (const auto& entry : fs::directory_iterator(oauth_dir)) {
                if (entry.is_regular_file()) profile_count++;
            }
        } catch (...) {}
        if (profile_count > 10) {
            notes.push_back({"OAuth", "info",
                "- " + std::to_string(profile_count) + " OAuth profile files in " + oauth_dir + ".\n"
                "- Consider cleaning up unused profiles."});
        }
    }

    // 对齐 OC: Orphan agent directory detection
    if (exists_dir(sessions_dir)) {
        std::vector<std::string> configured_agents;
        if (cfg.contains("agents") && cfg["agents"].contains("list")) {
            for (const auto& agent : cfg["agents"]["list"]) {
                configured_agents.push_back(agent.value("id", ""));
            }
        }
        std::vector<std::string> orphan_dirs;
        try {
            for (const auto& entry : fs::directory_iterator(sessions_dir)) {
                if (!entry.is_directory()) continue;
                std::string dir_name = entry.path().filename().string();
                bool found = false;
                for (const auto& agent_id : configured_agents) {
                    if (dir_name == agent_id || dir_name.find(agent_id) == 0) {
                        found = true;
                        break;
                    }
                }
                if (!found && dir_name != "default") {
                    orphan_dirs.push_back(dir_name);
                }
            }
        } catch (...) {}
        if (!orphan_dirs.empty()) {
            notes.push_back({"State integrity", "info",
                "- Found " + count_label(static_cast<int>(orphan_dirs.size()), "orphan agent directory") +
                " in sessions: " + format_file_preview(orphan_dirs) + ".\n"
                "- These may be from deleted agents. Run `hunterclaw doctor --fix` to clean up."});
        }
    }

    // 对齐 OC: noteWorkspaceBackupTip
    std::string workspace_dir = infra::get_workspace_dir();
    if (exists_dir(workspace_dir) && !exists_dir(workspace_dir + "/.git")) {
        notes.push_back({"Workspace", "info",
            "- Workspace " + workspace_dir + " is not a Git repository.\n"
            "- Consider initializing version control for backup and history tracking."});
    }

    return notes;
}

} // namespace doctor
} // namespace commands
} // namespace hunterclaw
