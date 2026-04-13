// ============================================================================
// gateway_hooks_mapping.cc
// ✅ 严格对齐 OpenClaw: src/gateway/hooks-mapping.ts (528行)
//
// Webhook Hook 映射引擎：
//   - HookMappingResolved: 解析后的映射规则 (23字段)
//   - HookAction: wake/agent 两种动作
//   - resolveHookMappings: 配置→映射列表 (含 gmail 预设)
//   - applyHookMappings: 遍历匹配→构建动作→运行变换→合并结果
//   - renderTemplate: {{expr}} 模板引擎
//   - getByPath: 安全属性路径遍历 (__proto__/prototype/constructor 防护)
//   - resolveContainedPath: symlink 逃逸检测
// ============================================================================

#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>
#include <algorithm>
#include <filesystem>
#include <functional>
#include <mutex>
#include <optional>
#include <regex>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

using json = nlohmann::json;
namespace fs = std::filesystem;

namespace hunterclaw {
namespace gateway {

namespace {
auto log() { return spdlog::get("gateway/hooks-mapping"); }
} // namespace

// ============================================================================
// 🎯 BLOCKED_PATH_KEYS (对齐 hooks-mapping.ts:489)
// ============================================================================

static const std::set<std::string> BLOCKED_PATH_KEYS = {
    "__proto__", "prototype", "constructor"
};

// ============================================================================
// 🎯 HookMappingResolved (对齐 hooks-mapping.ts:8-27)
// ============================================================================

struct HookMappingResolved {
    std::string id;
    std::optional<std::string> match_path;
    std::optional<std::string> match_source;
    std::string action = "agent";         // "wake" | "agent"
    std::string wake_mode = "now";        // "now" | "next-heartbeat"
    std::optional<std::string> name;
    std::optional<std::string> agent_id;
    std::optional<std::string> session_key;
    std::optional<std::string> message_template;
    std::optional<std::string> text_template;
    std::optional<bool> deliver;
    std::optional<bool> allow_unsafe_external_content;
    std::optional<std::string> channel;
    std::optional<std::string> to;
    std::optional<std::string> model;
    std::optional<std::string> thinking;
    std::optional<int> timeout_seconds;
    // transform
    std::optional<std::string> transform_module_path;
    std::optional<std::string> transform_export_name;
};

// ============================================================================
// 🎯 HookMappingContext (对齐 hooks-mapping.ts:34-39)
// ============================================================================

struct HookMappingContext {
    json payload;
    std::unordered_map<std::string, std::string> headers;
    std::string url;
    std::string path;
    std::unordered_map<std::string, std::string> query_params;
};

// ============================================================================
// 🎯 HookAction (对齐 hooks-mapping.ts:41-61)
// ============================================================================

struct HookAction {
    std::string kind;     // "wake" | "agent"
    std::string text;     // for wake
    std::string message;  // for agent
    std::string mode;     // "now" | "next-heartbeat"
    std::string wake_mode;
    std::optional<std::string> name;
    std::optional<std::string> agent_id;
    std::optional<std::string> session_key;
    std::optional<bool> deliver;
    std::optional<bool> allow_unsafe_external_content;
    std::optional<std::string> channel;
    std::optional<std::string> to;
    std::optional<std::string> model;
    std::optional<std::string> thinking;
    std::optional<int> timeout_seconds;
};

struct HookMappingResult {
    bool ok = false;
    std::optional<HookAction> action;
    bool skipped = false;
    std::string error;
};

// ============================================================================
// 🎯 getByPath (对齐 hooks-mapping.ts:491-527)
// 安全属性路径遍历（__proto__/prototype/constructor 阻断）
// ============================================================================

static json get_by_path(const json& input, const std::string& path_expr) {
    if (path_expr.empty() || !input.is_object()) return nullptr;

    // 解析路径表达式: foo.bar[0].baz
    std::vector<std::string> parts;
    std::regex re(R"(([^.\[\]]+)|\[(\d+)\])");
    auto begin = std::sregex_iterator(path_expr.begin(), path_expr.end(), re);
    auto end = std::sregex_iterator();

    for (auto it = begin; it != end; ++it) {
        if ((*it)[1].matched) {
            parts.push_back((*it)[1].str());
        } else if ((*it)[2].matched) {
            parts.push_back("[" + (*it)[2].str() + "]");
        }
    }

    json current = input;
    for (const auto& part : parts) {
        if (current.is_null()) return nullptr;

        if (part.front() == '[' && part.back() == ']') {
            // Array index
            int idx = std::stoi(part.substr(1, part.size() - 2));
            if (!current.is_array() || idx < 0 ||
                static_cast<size_t>(idx) >= current.size()) {
                return nullptr;
            }
            current = current[idx];
        } else {
            // 原型链属性阻断 — 对齐 :518-520
            if (BLOCKED_PATH_KEYS.count(part)) return nullptr;
            if (!current.is_object()) return nullptr;
            if (!current.contains(part)) return nullptr;
            current = current[part];
        }
    }
    return current;
}

// ============================================================================
// 🎯 renderTemplate (对齐 hooks-mapping.ts:445-461)
// {{expr}} 模板引擎
// ============================================================================

static std::string resolve_template_expr(const std::string& expr, const HookMappingContext& ctx) {
    if (expr == "path") return ctx.path;
    if (expr == "now") {
        auto now = std::chrono::system_clock::now();
        auto tt = std::chrono::system_clock::to_time_t(now);
        char buf[64];
        std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", std::gmtime(&tt));
        return buf;
    }

    // headers.xxx
    if (expr.rfind("headers.", 0) == 0) {
        std::string key = expr.substr(8);
        auto it = ctx.headers.find(key);
        return it != ctx.headers.end() ? it->second : "";
    }

    // query.xxx
    if (expr.rfind("query.", 0) == 0) {
        std::string key = expr.substr(6);
        auto it = ctx.query_params.find(key);
        return it != ctx.query_params.end() ? it->second : "";
    }

    // payload.xxx or direct path into payload
    std::string path = expr;
    if (expr.rfind("payload.", 0) == 0) {
        path = expr.substr(8);
    }

    json val = get_by_path(ctx.payload, path);
    if (val.is_null()) return "";
    if (val.is_string()) return val.get<std::string>();
    if (val.is_number() || val.is_boolean()) return val.dump();
    return val.dump();
}

static std::string render_template(const std::string& tmpl, const HookMappingContext& ctx) {
    if (tmpl.empty()) return "";

    std::string result;
    std::regex re(R"(\{\{\s*([^}]+)\s*\}\})");
    std::sregex_iterator begin(tmpl.begin(), tmpl.end(), re);
    std::sregex_iterator end;

    size_t last_pos = 0;
    for (auto it = begin; it != end; ++it) {
        result += tmpl.substr(last_pos, it->position() - last_pos);
        std::string expr = (*it)[1].str();
        // trim
        auto a = expr.find_first_not_of(" \t");
        auto b = expr.find_last_not_of(" \t");
        if (a != std::string::npos) expr = expr.substr(a, b - a + 1);
        result += resolve_template_expr(expr, ctx);
        last_pos = it->position() + it->length();
    }
    result += tmpl.substr(last_pos);
    return result;
}

static std::optional<std::string> render_optional(
    const std::optional<std::string>& value,
    const HookMappingContext& ctx
) {
    if (!value.has_value()) return std::nullopt;
    std::string rendered = render_template(value.value(), ctx);
    // trim
    auto a = rendered.find_first_not_of(" \t\n\r");
    if (a == std::string::npos) return std::nullopt;
    return rendered.substr(a, rendered.find_last_not_of(" \t\n\r") - a + 1);
}

// ============================================================================
// 🎯 normalizeMatchPath (对齐 hooks-mapping.ts:426-435)
// ============================================================================

static std::optional<std::string> normalize_match_path(const std::optional<std::string>& raw) {
    if (!raw.has_value()) return std::nullopt;
    std::string s = raw.value();
    auto a = s.find_first_not_of(" \t");
    if (a == std::string::npos) return std::nullopt;
    s = s.substr(a, s.find_last_not_of(" \t") - a + 1);
    // strip leading/trailing slashes
    while (!s.empty() && s.front() == '/') s.erase(0, 1);
    while (!s.empty() && s.back() == '/') s.pop_back();
    return s.empty() ? std::nullopt : std::optional<std::string>(s);
}

// ============================================================================
// 🎯 resolveContainedPath (对齐 hooks-mapping.ts:388-412)
// symlink 逃逸检测
// ============================================================================

static bool escapes_base(const fs::path& base_dir, const fs::path& candidate) {
    auto rel = fs::relative(candidate, base_dir);
    std::string rel_str = rel.string();
    return rel_str == ".." || rel_str.rfind(".." + std::string(1, fs::path::preferred_separator), 0) == 0 ||
           rel.is_absolute();
}

static fs::path resolve_existing_ancestor(const fs::path& candidate) {
    fs::path current = fs::absolute(candidate);
    while (true) {
        if (fs::exists(current)) return current;
        auto parent = current.parent_path();
        if (parent == current) return {};
        current = parent;
    }
}

static fs::path resolve_contained_path(
    const fs::path& base_dir,
    const std::string& target,
    const std::string& label
) {
    fs::path base = fs::absolute(base_dir);
    if (target.empty()) {
        throw std::runtime_error(label + " module path is required");
    }

    fs::path resolved = fs::path(target).is_absolute()
        ? fs::absolute(target)
        : fs::absolute(base / target);

    if (escapes_base(base, resolved)) {
        throw std::runtime_error(label + " module path must be within " +
            base.string() + ": " + target);
    }

    // symlink 逃逸检查 — 对齐 :399-411
    std::error_code ec;
    auto base_realpath = fs::canonical(base, ec);
    if (ec) base_realpath = base;

    auto existing_ancestor = resolve_existing_ancestor(resolved);
    if (!existing_ancestor.empty()) {
        auto ancestor_realpath = fs::canonical(existing_ancestor, ec);
        if (!ec && escapes_base(base_realpath, ancestor_realpath)) {
            throw std::runtime_error(label + " module path must be within " +
                base.string() + ": " + target);
        }
    }

    return resolved;
}

// ============================================================================
// 🎯 hookPresetMappings (对齐 hooks-mapping.ts:68-81)
// ============================================================================

static const std::unordered_map<std::string, std::vector<json>> HOOK_PRESET_MAPPINGS = {
    {"gmail", {json{
        {"id", "gmail"},
        {"match", json{{"path", "gmail"}}},
        {"action", "agent"},
        {"wakeMode", "now"},
        {"name", "Gmail"},
        {"sessionKey", "hook:gmail:{{messages[0].id}}"},
        {"messageTemplate",
         "New email from {{messages[0].from}}\nSubject: {{messages[0].subject}}\n"
         "{{messages[0].snippet}}\n{{messages[0].body}}"}
    }}}
};

// ============================================================================
// 🎯 mappingMatches (对齐 hooks-mapping.ts:225-238)
// ============================================================================

static bool mapping_matches(const HookMappingResolved& mapping, const HookMappingContext& ctx) {
    if (mapping.match_path.has_value()) {
        auto ctx_path = normalize_match_path(std::optional<std::string>(ctx.path));
        if (mapping.match_path != ctx_path) return false;
    }
    if (mapping.match_source.has_value()) {
        std::string source;
        if (ctx.payload.contains("source") && ctx.payload["source"].is_string()) {
            source = ctx.payload["source"].get<std::string>();
        }
        if (source.empty() || source != mapping.match_source.value()) return false;
    }
    return true;
}

// ============================================================================
// 🎯 buildActionFromMapping (对齐 hooks-mapping.ts:240-274)
// ============================================================================

static HookMappingResult build_action_from_mapping(
    const HookMappingResolved& mapping,
    const HookMappingContext& ctx
) {
    if (mapping.action == "wake") {
        std::string text = render_template(
            mapping.text_template.value_or(""), ctx);
        HookAction action;
        action.kind = "wake";
        action.text = text;
        action.mode = mapping.wake_mode;
        return {true, action, false, ""};
    }

    std::string message = render_template(
        mapping.message_template.value_or(""), ctx);
    HookAction action;
    action.kind = "agent";
    action.message = message;
    action.name = render_optional(mapping.name, ctx);
    action.agent_id = mapping.agent_id;
    action.wake_mode = mapping.wake_mode;
    action.session_key = render_optional(mapping.session_key, ctx);
    action.deliver = mapping.deliver;
    action.allow_unsafe_external_content = mapping.allow_unsafe_external_content;
    action.channel = mapping.channel;
    action.to = render_optional(mapping.to, ctx);
    action.model = render_optional(mapping.model, ctx);
    action.thinking = render_optional(mapping.thinking, ctx);
    action.timeout_seconds = mapping.timeout_seconds;
    return {true, action, false, ""};
}

// ============================================================================
// 🎯 validateAction (对齐 hooks-mapping.ts:316-327)
// ============================================================================

static HookMappingResult validate_action(const HookAction& action) {
    if (action.kind == "wake") {
        std::string trimmed = action.text;
        auto a = trimmed.find_first_not_of(" \t\n\r");
        if (a == std::string::npos) {
            return {false, std::nullopt, false, "hook mapping requires text"};
        }
        return {true, action, false, ""};
    }
    std::string trimmed = action.message;
    auto a = trimmed.find_first_not_of(" \t\n\r");
    if (a == std::string::npos) {
        return {false, std::nullopt, false, "hook mapping requires message"};
    }
    return {true, action, false, ""};
}

// ============================================================================
// 🎯 resolveHookMappings (对齐 hooks-mapping.ts:107-146)
// ============================================================================

std::vector<HookMappingResolved> resolve_hook_mappings(
    const json& hooks_config,
    const std::string& config_dir
) {
    if (hooks_config.is_null()) return {};

    std::vector<json> raw_mappings;

    // 合并 mappings + presets
    if (hooks_config.contains("mappings") && hooks_config["mappings"].is_array()) {
        for (const auto& m : hooks_config["mappings"]) {
            raw_mappings.push_back(m);
        }
    }

    if (hooks_config.contains("presets") && hooks_config["presets"].is_array()) {
        bool gmail_allow_unsafe = false;
        if (hooks_config.contains("gmail") &&
            hooks_config["gmail"].contains("allowUnsafeExternalContent")) {
            gmail_allow_unsafe = hooks_config["gmail"]["allowUnsafeExternalContent"].get<bool>();
        }

        for (const auto& preset : hooks_config["presets"]) {
            std::string preset_name = preset.get<std::string>();
            auto it = HOOK_PRESET_MAPPINGS.find(preset_name);
            if (it == HOOK_PRESET_MAPPINGS.end()) continue;

            for (auto pm : it->second) {
                if (preset_name == "gmail" && gmail_allow_unsafe) {
                    pm["allowUnsafeExternalContent"] = true;
                }
                raw_mappings.push_back(pm);
            }
        }
    }

    if (raw_mappings.empty()) return {};

    fs::path transforms_dir = fs::path(config_dir) / "hooks" / "transforms";

    std::vector<HookMappingResolved> result;
    for (size_t i = 0; i < raw_mappings.size(); ++i) {
        const auto& m = raw_mappings[i];
        HookMappingResolved resolved;
        resolved.id = m.value("id", "mapping-" + std::to_string(i + 1));
        if (m.contains("match") && m["match"].contains("path")) {
            resolved.match_path = normalize_match_path(
                std::optional<std::string>(m["match"]["path"].get<std::string>()));
        }
        if (m.contains("match") && m["match"].contains("source")) {
            resolved.match_source = m["match"]["source"].get<std::string>();
        }
        resolved.action = m.value("action", "agent");
        resolved.wake_mode = m.value("wakeMode", "now");
        if (m.contains("name")) resolved.name = m["name"].get<std::string>();
        if (m.contains("agentId")) resolved.agent_id = m["agentId"].get<std::string>();
        if (m.contains("sessionKey")) resolved.session_key = m["sessionKey"].get<std::string>();
        if (m.contains("messageTemplate")) resolved.message_template = m["messageTemplate"].get<std::string>();
        if (m.contains("textTemplate")) resolved.text_template = m["textTemplate"].get<std::string>();
        if (m.contains("deliver")) resolved.deliver = m["deliver"].get<bool>();
        if (m.contains("allowUnsafeExternalContent"))
            resolved.allow_unsafe_external_content = m["allowUnsafeExternalContent"].get<bool>();
        if (m.contains("to")) resolved.to = m["to"].get<std::string>();
        if (m.contains("model")) resolved.model = m["model"].get<std::string>();
        if (m.contains("thinking")) resolved.thinking = m["thinking"].get<std::string>();
        if (m.contains("timeoutSeconds")) resolved.timeout_seconds = m["timeoutSeconds"].get<int>();

        // transform
        if (m.contains("transform") && m["transform"].is_object()) {
            auto module_path = m["transform"].value("module", "");
            resolved.transform_module_path = resolve_contained_path(
                transforms_dir, module_path, "Hook transform").string();
            if (m["transform"].contains("export")) {
                resolved.transform_export_name = m["transform"]["export"].get<std::string>();
            }
        }

        result.push_back(std::move(resolved));
    }
    return result;
}

// ============================================================================
// 🎯 applyHookMappings (对齐 hooks-mapping.ts:148-184)
// ============================================================================

HookMappingResult apply_hook_mappings(
    const std::vector<HookMappingResolved>& mappings,
    const HookMappingContext& ctx
) {
    if (mappings.empty()) return {false, std::nullopt, false, ""};

    for (const auto& mapping : mappings) {
        if (!mapping_matches(mapping, ctx)) continue;

        auto base = build_action_from_mapping(mapping, ctx);
        if (!base.ok) return base;

        // Note: C++ 端不支持动态 JS transform 模块加载
        // transform 逻辑需要通过 plugin system 实现
        if (mapping.transform_module_path.has_value()) {
            if (log()) {
                log()->warn("hook transform modules not supported in C++ runtime, "
                    "transform for mapping '{}' will be skipped", mapping.id);
            }
        }

        if (!base.action.has_value()) {
            return {true, std::nullopt, true, ""};
        }

        return validate_action(base.action.value());
    }

    return {false, std::nullopt, false, ""};
}

} // namespace gateway
} // namespace hunterclaw
