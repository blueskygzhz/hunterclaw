#pragma once
// Aligned with openclaw: src/config/validation.ts (612 lines) + backup-rotation.ts (126 lines)
// Config validation, schema checking, backup rotation

#include "hunterclaw/core/result.h"
#include <nlohmann/json.hpp>
#include <string>
#include <vector>
#include <optional>
#include <functional>
#include <filesystem>

namespace hunterclaw {
namespace config {

// ============================================================================
// Config Validation Issue (对齐 validation.ts)
// ============================================================================
struct ConfigValidationIssue {
    std::string path;
    std::string message;
    std::vector<std::string> allowed_values;
    int allowed_values_hidden_count = 0;
};

// ============================================================================
// Config Validation Result (对齐 validation.ts)
// ============================================================================
struct ConfigValidationResult {
    bool ok = false;
    nlohmann::json config;
    std::vector<ConfigValidationIssue> issues;
    std::vector<ConfigValidationIssue> warnings;
};

// ============================================================================
// Config Validator (对齐 validation.ts)
// ============================================================================
class ConfigValidator {
public:
    // Aligned with openclaw: validateConfigObjectRaw
    ConfigValidationResult validate_raw(const nlohmann::json& raw);

    // Aligned with openclaw: validateConfigObject (with defaults applied)
    ConfigValidationResult validate(const nlohmann::json& raw);

    // Aligned with openclaw: validateConfigObjectWithPlugins
    ConfigValidationResult validate_with_plugins(const nlohmann::json& raw);

private:
    // Channel validation
    std::vector<ConfigValidationIssue> validate_channels(const nlohmann::json& config);

    // Agent validation
    std::vector<ConfigValidationIssue> validate_agents(const nlohmann::json& config);

    // Gateway validation
    std::vector<ConfigValidationIssue> validate_gateway(const nlohmann::json& config);

    // Heartbeat target validation
    std::vector<ConfigValidationIssue> validate_heartbeat_targets(const nlohmann::json& config);

    // Identity avatar validation (对齐 validateIdentityAvatar)
    std::vector<ConfigValidationIssue> validate_identity_avatar(const nlohmann::json& config);

    // Gateway Tailscale bind validation (对齐 validateGatewayTailscaleBind)
    std::vector<ConfigValidationIssue> validate_gateway_tailscale_bind(const nlohmann::json& config);

    // Telegram-specific validation
    std::vector<ConfigValidationIssue> validate_telegram_config(const nlohmann::json& config);

    // WhatsApp-specific validation
    std::vector<ConfigValidationIssue> validate_whatsapp_config(const nlohmann::json& config);

    // Discord-specific validation
    std::vector<ConfigValidationIssue> validate_discord_config(const nlohmann::json& config);

    // TTS validation
    std::vector<ConfigValidationIssue> validate_tts_config(const nlohmann::json& config);

    // Sandbox validation
    std::vector<ConfigValidationIssue> validate_sandbox_config(const nlohmann::json& config);

    // Duplicate agent directory check (对齐 findDuplicateAgentDirs)
    std::vector<ConfigValidationIssue> check_duplicate_agent_dirs(const nlohmann::json& config);
};

// ============================================================================
// Backup Rotation (对齐 backup-rotation.ts)
// ============================================================================
static constexpr int CONFIG_BACKUP_COUNT = 5;

// Aligned with openclaw: rotateConfigBackups
void rotate_config_backups(const std::string& config_path);

// Aligned with openclaw: hardenBackupPermissions
void harden_backup_permissions(const std::string& config_path);

// Aligned with openclaw: cleanOrphanBackups
void clean_orphan_backups(const std::string& config_path);

// Aligned with openclaw: maintainConfigBackups
void maintain_config_backups(const std::string& config_path);

// ============================================================================
// Known Channel IDs (对齐 channels/registry.ts CHANNEL_IDS)
// ============================================================================
inline const std::vector<std::string>& known_channel_ids() {
    static const std::vector<std::string> ids = {
        "telegram", "whatsapp", "discord", "slack", "signal",
        "imessage", "irc", "matrix", "msteams", "line",
        "googlechat", "viber", "nostr", "twitch", "mattermost",
        "nextcloud-talk", "feishu", "synology-chat", "bluebubbles",
    };
    return ids;
}

} // namespace config
} // namespace hunterclaw
