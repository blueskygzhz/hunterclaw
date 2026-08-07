// 🎯 严格对齐 OpenClaw: src/agents/sandbox/sanitize-env-vars.ts (111 lines)
// 环境变量净化：阻止 API key/token/密码等敏感变量注入容器

#include "hunterclaw/sandbox/sanitize_env_vars.h"
#include <string>
#include <vector>
#include <map>
#include <set>
#include <regex>
#include <algorithm>
#include <cctype>

namespace hunterclaw {
namespace sandbox {

// ============================================================================
// Blocked patterns - 对齐 OpenClaw BLOCKED_ENV_VAR_PATTERNS
// ============================================================================

static const std::vector<std::regex> BLOCKED_ENV_VAR_PATTERNS = {
    std::regex("^ANTHROPIC_API_KEY$", std::regex::icase),
    std::regex("^OPENAI_API_KEY$", std::regex::icase),
    std::regex("^GEMINI_API_KEY$", std::regex::icase),
    std::regex("^OPENROUTER_API_KEY$", std::regex::icase),
    std::regex("^MINIMAX_API_KEY$", std::regex::icase),
    std::regex("^ELEVENLABS_API_KEY$", std::regex::icase),
    std::regex("^SYNTHETIC_API_KEY$", std::regex::icase),
    std::regex("^TELEGRAM_BOT_TOKEN$", std::regex::icase),
    std::regex("^DISCORD_BOT_TOKEN$", std::regex::icase),
    std::regex("^SLACK_(BOT|APP)_TOKEN$", std::regex::icase),
    std::regex("^LINE_CHANNEL_SECRET$", std::regex::icase),
    std::regex("^LINE_CHANNEL_ACCESS_TOKEN$", std::regex::icase),
    std::regex("^HUNTERCLAW_GATEWAY_(TOKEN|PASSWORD)$", std::regex::icase),
    std::regex("^AWS_(SECRET_ACCESS_KEY|SECRET_KEY|SESSION_TOKEN)$", std::regex::icase),
    std::regex("^(GH|GITHUB)_TOKEN$", std::regex::icase),
    std::regex("^(AZURE|AZURE_OPENAI|COHERE|AI_GATEWAY|OPENROUTER)_API_KEY$", std::regex::icase),
    std::regex("_?(API_KEY|TOKEN|PASSWORD|PRIVATE_KEY|SECRET)$", std::regex::icase),
};

// 🎯 对齐 OpenClaw: ALLOWED_ENV_VAR_PATTERNS
static const std::vector<std::regex> ALLOWED_ENV_VAR_PATTERNS = {
    std::regex("^LANG$"),
    std::regex("^LC_.*$", std::regex::icase),
    std::regex("^PATH$", std::regex::icase),
    std::regex("^HOME$", std::regex::icase),
    std::regex("^USER$", std::regex::icase),
    std::regex("^SHELL$", std::regex::icase),
    std::regex("^TERM$", std::regex::icase),
    std::regex("^TZ$", std::regex::icase),
    std::regex("^NODE_ENV$", std::regex::icase),
};

// 🎯 对齐 OpenClaw: MCP_EXPLICIT_CREDENTIAL_ENV_KEYS
// 显式配置的 MCP server凭据属于运维输入的授权信息，不是被继承的宿主
// 配置枢纽，故放行；仅拦截被继承的危险宿主变量。
static const std::set<std::string> MCP_EXPLICIT_CREDENTIAL_ENV_KEYS = {
    "AWS_ACCESS_KEY_ID",
    "GITHUB_TOKEN",
    "GH_TOKEN",
    "MONGODB_URI",
    "NODE_AUTH_TOKEN",
    "NPM_TOKEN",
    "REDIS_URL",
};

static std::string upper_copy(const std::string& s) {
    std::string out = s;
    std::transform(out.begin(), out.end(), out.begin(),
                   [](unsigned char c) { return std::toupper(c); });
    return out;
}

static bool matches_any(const std::string& key, const std::vector<std::regex>& patterns) {
    for (const auto& p : patterns) {
        if (std::regex_search(key, p)) return true;
    }
    return false;
}

// 🎯 对齐 OpenClaw: validateEnvVarValue
static std::string validate_env_var_value(const std::string& value) {
    if (value.find('\0') != std::string::npos) return "Contains null bytes";
    if (value.size() > 32768) return "Value exceeds maximum length";
    // 🎯 对齐 OpenClaw: base64 credential detection
    static const std::regex base64_re("^[A-Za-z0-9+/=]{80,}$");
    if (std::regex_match(value, base64_re)) return "Value looks like base64-encoded credential data";
    return "";
}

// 🎯 对齐 OpenClaw: sanitizeEnvVars
EnvVarSanitizationResult sanitize_env_vars(
    const std::map<std::string, std::string>& env_vars,
    bool strict_mode
) {
    EnvVarSanitizationResult result;

    for (const auto& [raw_key, value] : env_vars) {
        std::string key = raw_key;
        // Trim
        size_t s = key.find_first_not_of(" \t");
        if (s == std::string::npos) continue;
        size_t e = key.find_last_not_of(" \t");
        key = key.substr(s, e - s + 1);
        if (key.empty()) continue;

        // 🎯 对齐 OpenClaw: 显式配置的 MCP 凭据白名单 (绕过 blocked 检查)
        bool is_explicit_credential =
            MCP_EXPLICIT_CREDENTIAL_ENV_KEYS.count(upper_copy(key)) > 0;

        // 🎯 对齐 OpenClaw: blocked pattern check
        if (!is_explicit_credential && matches_any(key, BLOCKED_ENV_VAR_PATTERNS)) {
            result.blocked.push_back(key);
            continue;
        }

        // 🎯 对齐 OpenClaw: strict mode - only allow known-safe vars
        if (strict_mode && !is_explicit_credential &&
            !matches_any(key, ALLOWED_ENV_VAR_PATTERNS)) {
            result.blocked.push_back(key);
            continue;
        }

        // 🎯 对齐 OpenClaw: value validation
        std::string warning = validate_env_var_value(value);
        if (!warning.empty()) {
            if (warning == "Contains null bytes") {
                result.blocked.push_back(key);
                continue;
            }
            result.warnings.push_back(key + ": " + warning);
        }

        result.allowed[key] = value;
    }

    return result;
}

}  // namespace sandbox
}  // namespace hunterclaw
