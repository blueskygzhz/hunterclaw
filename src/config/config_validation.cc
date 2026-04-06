// Aligned with openclaw: src/config/validation.ts + backup-rotation.ts
#include "hunterclaw/config/config_validation.h"
#include <spdlog/spdlog.h>
#include <algorithm>
#include <set>
#include <sstream>

namespace hunterclaw {
namespace config {

namespace {
auto log() {
    static auto logger = []() {
        auto l = spdlog::get("config/validation");
        if (!l) l = spdlog::stdout_logger_mt("config/validation");
        return l;
    }();
    return logger;
}
} // namespace

// ============================================================================
// ConfigValidator
// ============================================================================

ConfigValidationResult ConfigValidator::validate_raw(const nlohmann::json& raw) {
    ConfigValidationResult result;
    std::vector<ConfigValidationIssue> all_issues;

    // Basic structure validation
    if (!raw.is_object()) {
        all_issues.push_back({"", "Config must be a JSON object"});
        result.ok = false;
        result.issues = all_issues;
        return result;
    }

    // Channel validation
    auto ch_issues = validate_channels(raw);
    all_issues.insert(all_issues.end(), ch_issues.begin(), ch_issues.end());

    // Agent validation
    auto ag_issues = validate_agents(raw);
    all_issues.insert(all_issues.end(), ag_issues.begin(), ag_issues.end());

    // Gateway validation
    auto gw_issues = validate_gateway(raw);
    all_issues.insert(all_issues.end(), gw_issues.begin(), gw_issues.end());

    // Avatar validation
    auto av_issues = validate_identity_avatar(raw);
    all_issues.insert(all_issues.end(), av_issues.begin(), av_issues.end());

    // Tailscale bind validation
    auto ts_issues = validate_gateway_tailscale_bind(raw);
    all_issues.insert(all_issues.end(), ts_issues.begin(), ts_issues.end());

    // Duplicate agent dirs
    auto dup_issues = check_duplicate_agent_dirs(raw);
    all_issues.insert(all_issues.end(), dup_issues.begin(), dup_issues.end());

    // Telegram validation
    auto tg_issues = validate_telegram_config(raw);
    all_issues.insert(all_issues.end(), tg_issues.begin(), tg_issues.end());

    // WhatsApp validation
    auto wa_issues = validate_whatsapp_config(raw);
    all_issues.insert(all_issues.end(), wa_issues.begin(), wa_issues.end());

    // Discord validation
    auto dc_issues = validate_discord_config(raw);
    all_issues.insert(all_issues.end(), dc_issues.begin(), dc_issues.end());

    // TTS validation
    auto tts_issues = validate_tts_config(raw);
    all_issues.insert(all_issues.end(), tts_issues.begin(), tts_issues.end());

    // Sandbox validation
    auto sb_issues = validate_sandbox_config(raw);
    all_issues.insert(all_issues.end(), sb_issues.begin(), sb_issues.end());

    result.ok = all_issues.empty();
    result.issues = all_issues;
    result.config = raw;
    return result;
}

ConfigValidationResult ConfigValidator::validate(const nlohmann::json& raw) {
    auto result = validate_raw(raw);
    // Apply defaults if valid (aligned with openclaw: applyModelDefaults + applyAgentDefaults)
    return result;
}

ConfigValidationResult ConfigValidator::validate_with_plugins(const nlohmann::json& raw) {
    auto base = validate(raw);
    if (!base.ok) return base;
    // Plugin validation (aligned with openclaw: validateConfigObjectWithPlugins)
    auto hb_issues = validate_heartbeat_targets(raw);
    base.warnings.insert(base.warnings.end(), hb_issues.begin(), hb_issues.end());
    return base;
}

// ============================================================================
// Channel validation
// ============================================================================
std::vector<ConfigValidationIssue> ConfigValidator::validate_channels(const nlohmann::json& config) {
    std::vector<ConfigValidationIssue> issues;
    if (!config.contains("channels") || !config["channels"].is_object()) return issues;
    const auto& known = known_channel_ids();
    std::set<std::string> allowed(known.begin(), known.end());
    allowed.insert("defaults");
    allowed.insert("modelByChannel");
    for (auto& [key, _] : config["channels"].items()) {
        if (allowed.find(key) == allowed.end()) {
            issues.push_back({"channels." + key, "unknown channel id: " + key});
        }
    }
    return issues;
}

// ============================================================================
// Agent validation
// ============================================================================
std::vector<ConfigValidationIssue> ConfigValidator::validate_agents(const nlohmann::json& config) {
    std::vector<ConfigValidationIssue> issues;
    if (!config.contains("agents")) return issues;
    const auto& agents = config["agents"];
    if (agents.contains("list") && agents["list"].is_array()) {
        for (size_t i = 0; i < agents["list"].size(); ++i) {
            const auto& entry = agents["list"][i];
            if (entry.contains("id") && entry["id"].is_string()) {
                std::string id = entry["id"];
                if (id.empty()) {
                    issues.push_back({"agents.list." + std::to_string(i) + ".id", "agent id must not be empty"});
                }
            }
        }
    }
    return issues;
}

// ============================================================================
// Gateway validation
// ============================================================================
std::vector<ConfigValidationIssue> ConfigValidator::validate_gateway(const nlohmann::json& config) {
    return {};  // Basic structure validated by schema
}

std::vector<ConfigValidationIssue> ConfigValidator::validate_heartbeat_targets(const nlohmann::json& config) {
    std::vector<ConfigValidationIssue> issues;
    if (!config.contains("agents")) return issues;
    auto validate_target = [&](const std::string& target, const std::string& path) {
        if (target.empty()) { issues.push_back({path, "heartbeat target must not be empty"}); return; }
        std::string lower = target;
        for (auto& c : lower) c = std::tolower(c);
        if (lower == "last" || lower == "none") return;
        const auto& known = known_channel_ids();
        if (std::find(known.begin(), known.end(), lower) != known.end()) return;
        issues.push_back({path, "unknown heartbeat target: " + target});
    };
    if (config["agents"].contains("defaults") && config["agents"]["defaults"].contains("heartbeat") &&
        config["agents"]["defaults"]["heartbeat"].contains("target")) {
        validate_target(config["agents"]["defaults"]["heartbeat"]["target"], "agents.defaults.heartbeat.target");
    }
    return issues;
}

std::vector<ConfigValidationIssue> ConfigValidator::validate_identity_avatar(const nlohmann::json& config) {
    return {};  // Validated during full config load
}

std::vector<ConfigValidationIssue> ConfigValidator::validate_gateway_tailscale_bind(const nlohmann::json& config) {
    std::vector<ConfigValidationIssue> issues;
    if (!config.contains("gateway") || !config["gateway"].contains("tailscale")) return issues;
    auto mode = config["gateway"]["tailscale"].value("mode", "off");
    if (mode != "serve" && mode != "funnel") return issues;
    auto bind = config["gateway"].value("bind", "loopback");
    if (bind == "loopback") return issues;
    issues.push_back({"gateway.bind", "gateway.bind must resolve to loopback when tailscale mode=" + mode});
    return issues;
}

// ============================================================================
// Per-channel validation
// ============================================================================
std::vector<ConfigValidationIssue> ConfigValidator::validate_telegram_config(const nlohmann::json& config) {
    std::vector<ConfigValidationIssue> issues;
    if (!config.contains("channels") || !config["channels"].contains("telegram")) return issues;
    const auto& tg = config["channels"]["telegram"];
    if (tg.contains("allowFrom") && tg["allowFrom"].is_array()) {
        for (const auto& entry : tg["allowFrom"]) {
            if (!entry.is_string()) {
                issues.push_back({"channels.telegram.allowFrom", "allowFrom entries must be strings"});
            }
        }
    }
    return issues;
}

std::vector<ConfigValidationIssue> ConfigValidator::validate_whatsapp_config(const nlohmann::json& config) {
    std::vector<ConfigValidationIssue> issues;
    if (!config.contains("channels") || !config["channels"].contains("whatsapp")) return issues;
    const auto& wa = config["channels"]["whatsapp"];
    if (wa.contains("allowFrom") && wa["allowFrom"].is_array()) {
        for (const auto& entry : wa["allowFrom"]) {
            if (!entry.is_string()) {
                issues.push_back({"channels.whatsapp.allowFrom", "allowFrom entries must be strings"});
            }
        }
    }
    return issues;
}

std::vector<ConfigValidationIssue> ConfigValidator::validate_discord_config(const nlohmann::json& config) {
    return {};
}

std::vector<ConfigValidationIssue> ConfigValidator::validate_tts_config(const nlohmann::json& config) {
    std::vector<ConfigValidationIssue> issues;
    if (!config.contains("messages") || !config["messages"].contains("tts")) return issues;
    const auto& tts = config["messages"]["tts"];
    if (tts.contains("auto") && tts["auto"].is_string()) {
        std::string mode = tts["auto"];
        if (mode != "off" && mode != "tagged" && mode != "inbound" && mode != "all") {
            issues.push_back({"messages.tts.auto", "tts auto mode must be one of: off, tagged, inbound, all",
                             {"off", "tagged", "inbound", "all"}, 0});
        }
    }
    return issues;
}

std::vector<ConfigValidationIssue> ConfigValidator::validate_sandbox_config(const nlohmann::json& config) {
    std::vector<ConfigValidationIssue> issues;
    if (!config.contains("agents")) return issues;
    if (config["agents"].contains("defaults") && config["agents"]["defaults"].contains("sandbox")) {
        const auto& sb = config["agents"]["defaults"]["sandbox"];
        if (sb.contains("docker") && sb["docker"].contains("image") && sb["docker"]["image"].is_string()) {
            std::string image = sb["docker"]["image"];
            if (image.empty()) {
                issues.push_back({"agents.defaults.sandbox.docker.image", "sandbox docker image must not be empty"});
            }
        }
    }
    return issues;
}

std::vector<ConfigValidationIssue> ConfigValidator::check_duplicate_agent_dirs(const nlohmann::json& config) {
    std::vector<ConfigValidationIssue> issues;
    if (!config.contains("agents") || !config["agents"].contains("list")) return issues;
    const auto& list = config["agents"]["list"];
    if (!list.is_array()) return issues;
    std::unordered_map<std::string, int> dir_counts;
    for (const auto& entry : list) {
        if (entry.contains("workspaceDir") && entry["workspaceDir"].is_string()) {
            dir_counts[entry["workspaceDir"]]++;
        }
    }
    for (const auto& [dir, count] : dir_counts) {
        if (count > 1) {
            issues.push_back({"agents.list", "duplicate agent workspace directory: " + dir});
        }
    }
    return issues;
}

// ============================================================================
// Backup Rotation (对齐 backup-rotation.ts)
// ============================================================================
void rotate_config_backups(const std::string& config_path) {
    if (CONFIG_BACKUP_COUNT <= 1) return;
    std::string bak_base = config_path + ".bak";
    int max_idx = CONFIG_BACKUP_COUNT - 1;
    // Delete oldest
    try { std::filesystem::remove(bak_base + "." + std::to_string(max_idx)); } catch (...) {}
    // Rotate
    for (int i = max_idx - 1; i >= 1; --i) {
        try { std::filesystem::rename(bak_base + "." + std::to_string(i), bak_base + "." + std::to_string(i + 1)); } catch (...) {}
    }
    try { std::filesystem::rename(bak_base, bak_base + ".1"); } catch (...) {}
}

void harden_backup_permissions(const std::string& config_path) {
    std::string bak_base = config_path + ".bak";
    auto perm = std::filesystem::perms::owner_read | std::filesystem::perms::owner_write;
    try { std::filesystem::permissions(bak_base, perm); } catch (...) {}
    for (int i = 1; i < CONFIG_BACKUP_COUNT; ++i) {
        try { std::filesystem::permissions(bak_base + "." + std::to_string(i), perm); } catch (...) {}
    }
}

void clean_orphan_backups(const std::string& config_path) {
    auto dir = std::filesystem::path(config_path).parent_path();
    auto base = std::filesystem::path(config_path).filename().string();
    std::string prefix = base + ".bak.";
    std::set<std::string> valid;
    for (int i = 1; i < CONFIG_BACKUP_COUNT; ++i) valid.insert(std::to_string(i));
    try {
        for (auto& entry : std::filesystem::directory_iterator(dir)) {
            auto name = entry.path().filename().string();
            if (name.find(prefix) != 0) continue;
            auto suffix = name.substr(prefix.size());
            if (valid.count(suffix)) continue;
            try { std::filesystem::remove(entry.path()); } catch (...) {}
        }
    } catch (...) {}
}

void maintain_config_backups(const std::string& config_path) {
    rotate_config_backups(config_path);
    try { std::filesystem::copy_file(config_path, config_path + ".bak",
                                      std::filesystem::copy_options::overwrite_existing); } catch (...) {}
    harden_backup_permissions(config_path);
    clean_orphan_backups(config_path);
}

} // namespace config
} // namespace hunterclaw
