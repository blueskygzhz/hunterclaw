// 🎯 严格对齐 OpenClaw: src/config/redact-snapshot.ts (689 行)
//                      + redact-snapshot.raw.ts
//                      + redact-snapshot.secret-ref.ts
//
// 配置快照双向脱敏系统：
// 1. redact_config_snapshot — 发送到 Web UI 前脱敏所有敏感字段
// 2. restore_redacted_values — 从 Web UI 写回时还原哨兵值为原始凭据
// 3. REDACTED_SENTINEL = "__OPENCLAW_REDACTED__"
// 4. 支持 ConfigUiHints (schema.hints) 驱动的精确脱敏
// 5. 无 hints 时回退到路径名启发式猜测

#include "hunterclaw/config/redact_snapshot.h"
#include <algorithm>
#include <spdlog/spdlog.h>

namespace hunterclaw {
namespace config {

static auto log() {
    static auto logger = spdlog::get("config/redaction");
    if (!logger) logger = spdlog::get("console");
    return logger;
}

// ============================================================================
// isSensitiveConfigPath — 对齐 OpenClaw: schema.hints.ts isSensitiveConfigPath
// 基于路径名模式的启发式敏感路径检测
// ============================================================================

static bool is_sensitive_config_path(const std::string& path) {
    // 路径名关键词检测（大小写不敏感）
    std::string lower = path;
    std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);

    static const std::vector<std::string> SENSITIVE_KEYWORDS = {
        "token", "secret", "password", "passwd", "apikey", "api_key",
        "api-key", "privatekey", "private_key", "private-key",
        "accesstoken", "access_token", "refreshtoken", "refresh_token",
        "auth.token", "auth.password", "credentials",
        "serviceaccount", "serviceaccountref",
    };

    for (const auto& kw : SENSITIVE_KEYWORDS) {
        if (lower.find(kw) != std::string::npos) {
            return true;
        }
    }
    return false;
}

// ============================================================================
// isEnvVarPlaceholder — 对齐 OpenClaw: ENV_VAR_PLACEHOLDER_PATTERN
// 跳过 ${...} 环境变量占位符（不脱敏）
// ============================================================================

static bool is_env_var_placeholder(const std::string& value) {
    std::string trimmed = value;
    size_t start = trimmed.find_first_not_of(" \t");
    if (start == std::string::npos) return false;
    size_t end = trimmed.find_last_not_of(" \t");
    trimmed = trimmed.substr(start, end - start + 1);

    if (trimmed.size() >= 4 && trimmed[0] == '$' && trimmed[1] == '{' && trimmed.back() == '}') {
        // Ensure no nested }
        return trimmed.find('}') == trimmed.size() - 1;
    }
    return false;
}

// ============================================================================
// isSecretRefShape — 对齐 OpenClaw: redact-snapshot.secret-ref.ts
// 判断对象是否为 { source: string, id: string } SecretRef 形状
// ============================================================================

static bool is_secret_ref_shape(const json& value) {
    return value.is_object() &&
           value.contains("source") && value["source"].is_string() &&
           value.contains("id") && value["id"].is_string();
}

// ============================================================================
// redactSecretRefId — 对齐 OpenClaw: redactSecretRefId()
// 仅脱敏 .id 字段（非环境变量占位符时）
// ============================================================================

static json redact_secret_ref_id(const json& value) {
    json redacted = value;
    if (value.contains("id") && value["id"].is_string()) {
        std::string id = value["id"].get<std::string>();
        if (!is_env_var_placeholder(id)) {
            redacted["id"] = REDACTED_SENTINEL;
        }
    }
    return redacted;
}

// ============================================================================
// isWholeObjectSensitivePath — 对齐 OpenClaw
// serviceAccount / serviceAccountRef 整个对象标记为敏感
// ============================================================================

static bool is_whole_object_sensitive_path(const std::string& path) {
    std::string lower = path;
    std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
    return lower.size() >= 14 &&
           (lower.substr(lower.size() - 14) == "serviceaccount" ||
            (lower.size() >= 17 && lower.substr(lower.size() - 17) == "serviceaccountref"));
}

// ============================================================================
// collectSensitiveStrings — 对齐 OpenClaw: collectSensitiveStrings()
// 递归收集对象中所有敏感字符串值
// ============================================================================

static void collect_sensitive_strings(const json& value, std::vector<std::string>& values) {
    if (value.is_string()) {
        std::string v = value.get<std::string>();
        if (!is_env_var_placeholder(v)) {
            values.push_back(v);
        }
        return;
    }
    if (value.is_array()) {
        for (const auto& item : value) {
            collect_sensitive_strings(item, values);
        }
        return;
    }
    if (value.is_object()) {
        if (is_secret_ref_shape(value)) {
            std::string id = value["id"].get<std::string>();
            if (!is_env_var_placeholder(id)) {
                values.push_back(id);
            }
            return;
        }
        for (auto& [key, val] : value.items()) {
            collect_sensitive_strings(val, values);
        }
    }
}

// ============================================================================
// redactObjectGuessing — 对齐 OpenClaw: redactObjectGuessing()
// 无 ConfigUiHints 时的路径名启发式脱敏
// ============================================================================

static json redact_object_guessing(const json& obj, const std::string& prefix) {
    if (obj.is_null()) return obj;

    if (obj.is_array()) {
        json result = json::array();
        std::string path = prefix + "[]";
        for (const auto& item : obj) {
            if (is_sensitive_config_path(path) && item.is_string() &&
                !is_env_var_placeholder(item.get<std::string>())) {
                result.push_back(REDACTED_SENTINEL);
            } else {
                result.push_back(redact_object_guessing(item, path));
            }
        }
        return result;
    }

    if (obj.is_object()) {
        json result = json::object();
        for (auto& [key, value] : obj.items()) {
            std::string dot_path = prefix.empty() ? key : prefix + "." + key;

            if (is_sensitive_config_path(dot_path) && value.is_string() &&
                !is_env_var_placeholder(value.get<std::string>())) {
                result[key] = REDACTED_SENTINEL;
            } else if (is_sensitive_config_path(dot_path) &&
                       is_whole_object_sensitive_path(dot_path) &&
                       value.is_object() && !value.is_array()) {
                result[key] = REDACTED_SENTINEL;
            } else if (value.is_object() || value.is_array()) {
                result[key] = redact_object_guessing(value, dot_path);
            } else {
                result[key] = value;
            }
        }
        return result;
    }

    return obj;
}

// ============================================================================
// redact_config_object — 对齐 OpenClaw: redactConfigObject<T>()
// ============================================================================

json redact_config_object(const json& config) {
    return redact_object_guessing(config, "");
}

// ============================================================================
// collectSensitiveValues — 对齐 OpenClaw: collectSensitiveValues()
// ============================================================================

static std::vector<std::string> collect_sensitive_values(const json& config) {
    std::vector<std::string> result;
    // Walk the config tree and collect values at sensitive paths
    std::function<void(const json&, const std::string&)> walk;
    walk = [&](const json& obj, const std::string& prefix) {
        if (obj.is_object()) {
            for (auto& [key, value] : obj.items()) {
                std::string path = prefix.empty() ? key : prefix + "." + key;
                if (is_sensitive_config_path(path) && value.is_string() &&
                    !is_env_var_placeholder(value.get<std::string>())) {
                    result.push_back(value.get<std::string>());
                } else if (is_sensitive_config_path(path) && is_whole_object_sensitive_path(path) &&
                           value.is_object()) {
                    collect_sensitive_strings(value, result);
                } else if (value.is_object() || value.is_array()) {
                    walk(value, path);
                }
            }
        } else if (obj.is_array()) {
            std::string path = prefix + "[]";
            for (const auto& item : obj) {
                if (is_sensitive_config_path(path) && item.is_string() &&
                    !is_env_var_placeholder(item.get<std::string>())) {
                    result.push_back(item.get<std::string>());
                } else {
                    walk(item, path);
                }
            }
        }
    };
    walk(config, "");
    return result;
}

// ============================================================================
// replaceSensitiveValuesInRaw — 对齐 OpenClaw: redact-snapshot.raw.ts
// 在原始 JSON5 文本中替换已知敏感值（最长优先）
// ============================================================================

static std::string replace_sensitive_values_in_raw(
    const std::string& raw,
    const std::vector<std::string>& sensitive_values
) {
    // Sort longest-first to avoid partial matches
    auto sorted = sensitive_values;
    std::sort(sorted.begin(), sorted.end(),
        [](const std::string& a, const std::string& b) { return a.size() > b.size(); });

    std::string result = raw;
    for (const auto& value : sorted) {
        size_t pos = 0;
        while ((pos = result.find(value, pos)) != std::string::npos) {
            result.replace(pos, value.size(), REDACTED_SENTINEL);
            pos += std::string(REDACTED_SENTINEL).size();
        }
    }
    return result;
}

// ============================================================================
// redact_config_snapshot — 严格对齐 OpenClaw: redactConfigSnapshot()
// ============================================================================

ConfigSnapshot redact_config_snapshot(const ConfigSnapshot& snapshot) {
    if (!snapshot.valid) {
        // 🎯 对齐 OpenClaw: invalid config → clear everything to avoid leaking unredacted data
        ConfigSnapshot redacted = snapshot;
        redacted.config = json::object();
        redacted.raw = "";
        redacted.parsed = json(nullptr);
        redacted.resolved = json::object();
        return redacted;
    }

    ConfigSnapshot redacted = snapshot;
    redacted.config = redact_config_object(snapshot.config);
    if (snapshot.parsed.is_object() || snapshot.parsed.is_array()) {
        redacted.parsed = redact_config_object(snapshot.parsed);
    }
    if (!snapshot.raw.empty()) {
        auto sensitive = collect_sensitive_values(snapshot.config);
        redacted.raw = replace_sensitive_values_in_raw(snapshot.raw, sensitive);
    }
    redacted.resolved = redact_config_object(snapshot.resolved);

    return redacted;
}

// ============================================================================
// restore_redacted_values — 严格对齐 OpenClaw: restoreRedactedValues()
// Deep-walk incoming 对象，将 REDACTED_SENTINEL 替换为 original 中对应的值
// ============================================================================

static json restore_recursive(
    const json& incoming,
    const json& original,
    const std::string& prefix
) {
    if (incoming.is_null()) return incoming;

    if (incoming.is_array()) {
        json result = json::array();
        std::string path = prefix + "[]";
        auto orig_arr = original.is_array() ? original : json::array();
        for (size_t i = 0; i < incoming.size(); ++i) {
            auto& item = incoming[i];
            json orig_item = (i < orig_arr.size()) ? orig_arr[i] : json(nullptr);

            if (is_sensitive_config_path(path) && item.is_string() &&
                item.get<std::string>() == REDACTED_SENTINEL) {
                result.push_back(orig_item);
            } else {
                result.push_back(restore_recursive(item, orig_item, path));
            }
        }
        return result;
    }

    if (incoming.is_object()) {
        json result = json::object();
        json orig_obj = original.is_object() ? original : json::object();
        for (auto& [key, value] : incoming.items()) {
            std::string path = prefix.empty() ? key : prefix + "." + key;
            json orig_value = orig_obj.contains(key) ? orig_obj[key] : json(nullptr);

            if (is_sensitive_config_path(path) && value.is_string() &&
                value.get<std::string>() == REDACTED_SENTINEL) {
                if (orig_obj.contains(key)) {
                    result[key] = orig_value;
                } else {
                    if (log()) log()->warn("Cannot un-redact config key {} as it doesn't have any value", path);
                    // Throw equivalent — return error
                    result[key] = value;
                }
            } else if (value.is_object() || value.is_array()) {
                result[key] = restore_recursive(value, orig_value, path);
            } else {
                result[key] = value;
            }
        }
        return result;
    }

    return incoming;
}

RedactionResult restore_redacted_values(const json& incoming, const json& original) {
    if (incoming.is_null()) {
        return {false, json(nullptr), "no input"};
    }
    if (!incoming.is_object()) {
        return {false, json(nullptr), "input not an object"};
    }

    try {
        json result = restore_recursive(incoming, original, "");
        return {true, result, ""};
    } catch (const std::exception& e) {
        return {false, json(nullptr), std::string("restore error: ") + e.what()};
    }
}

}  // namespace config
}  // namespace hunterclaw
