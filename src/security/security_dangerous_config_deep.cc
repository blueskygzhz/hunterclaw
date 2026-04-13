// 🎯 Strict alignment: OpenClaw security/dangerous-config-flags.ts (82)
//   + context-visibility.ts (59) + config-regex.ts (79) + secret-equal.ts (13)
// Dangerous config flag detection, context visibility control, safe regex compilation.

#include "hunterclaw/security/audit.h"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <openssl/sha.h>
#include <regex>
#include <vector>

namespace hunterclaw {
namespace security {
namespace config_deep {

using json = nlohmann::json;

// ─── dangerous-config-flags.ts (对齐 82 lines) ──────────────────────────

// 🎯 collectEnabledInsecureOrDangerousFlags (对齐 dangerous-config-flags.ts:13-81)
std::vector<std::string> collect_enabled_insecure_or_dangerous_flags(const json& cfg) {
    std::vector<std::string> flags;
    
    auto gw = cfg.value("gateway", json::object());
    auto cui = gw.value("controlUi", json::object());
    
    if (cui.value("allowInsecureAuth", false))
        flags.push_back("gateway.controlUi.allowInsecureAuth=true");
    if (cui.value("dangerouslyAllowHostHeaderOriginFallback", false))
        flags.push_back("gateway.controlUi.dangerouslyAllowHostHeaderOriginFallback=true");
    if (cui.value("dangerouslyDisableDeviceAuth", false))
        flags.push_back("gateway.controlUi.dangerouslyDisableDeviceAuth=true");
    
    auto hooks = cfg.value("hooks", json::object());
    if (hooks.value("gmail", json::object()).value("allowUnsafeExternalContent", false))
        flags.push_back("hooks.gmail.allowUnsafeExternalContent=true");
    
    auto mappings = hooks.value("mappings", json::array());
    for (size_t i = 0; i < mappings.size(); i++) {
        if (mappings[i].is_object() && mappings[i].value("allowUnsafeExternalContent", false))
            flags.push_back("hooks.mappings[" + std::to_string(i) + "].allowUnsafeExternalContent=true");
    }
    
    auto tools = cfg.value("tools", json::object());
    auto exec = tools.value("exec", json::object());
    auto ap = exec.value("applyPatch", json::object());
    if (ap.contains("workspaceOnly") && ap["workspaceOnly"] == false)
        flags.push_back("tools.exec.applyPatch.workspaceOnly=false");
    
    // Plugin config contract dangerous flags (对齐 dangerous-config-flags.ts:38-79)
    auto plugins = cfg.value("plugins", json::object());
    auto entries = plugins.value("entries", json::object());
    for (auto& [plugin_id, entry] : entries.items()) {
        if (!entry.is_object()) continue;
        auto config = entry.value("config", json::object());
        // Check for known dangerous patterns in plugin config
        if (config.value("dangerouslyAllowExternalContent", false))
            flags.push_back("plugins.entries." + plugin_id + ".config.dangerouslyAllowExternalContent=true");
        if (config.value("dangerouslySkipAuth", false))
            flags.push_back("plugins.entries." + plugin_id + ".config.dangerouslySkipAuth=true");
        if (config.value("dangerouslyAllowHttp", false))
            flags.push_back("plugins.entries." + plugin_id + ".config.dangerouslyAllowHttp=true");
    }
    
    return flags;
}

// ─── context-visibility.ts (对齐 59 lines) ──────────────────────────────

enum class ContextVisibilityMode { All, Allowlist, AllowlistQuote };
enum class ContextVisibilityKind { History, Thread, Quote, Forwarded };

struct ContextVisibilityDecision {
    bool include = false;
    std::string reason;  // "mode_all"|"sender_allowed"|"quote_override"|"blocked"
};

// 🎯 evaluateSupplementalContextVisibility (对齐 context-visibility.ts:16-31)
ContextVisibilityDecision evaluate_supplemental_context_visibility(
    ContextVisibilityMode mode, ContextVisibilityKind kind, bool sender_allowed)
{
    if (mode == ContextVisibilityMode::All)
        return {true, "mode_all"};
    if (sender_allowed)
        return {true, "sender_allowed"};
    if (mode == ContextVisibilityMode::AllowlistQuote && kind == ContextVisibilityKind::Quote)
        return {true, "quote_override"};
    return {false, "blocked"};
}

// 🎯 shouldIncludeSupplementalContext (对齐 context-visibility.ts:33-39)
bool should_include_supplemental_context(
    ContextVisibilityMode mode, ContextVisibilityKind kind, bool sender_allowed)
{
    return evaluate_supplemental_context_visibility(mode, kind, sender_allowed).include;
}

// ─── config-regex.ts (对齐 79 lines) ────────────────────────────────────

enum class ConfigRegexRejectReason { UnsafeNestedRepetition, InvalidRegex };

struct CompiledConfigRegex {
    std::optional<std::regex> regex;
    std::string pattern;
    std::string flags;
    std::optional<ConfigRegexRejectReason> reason;
};

// Forward declaration from safe_regex.cc
extern bool has_nested_repetition(const std::string& source);

// 🎯 compileConfigRegex (对齐 config-regex.ts:30-41)
std::optional<CompiledConfigRegex> compile_config_regex(const std::string& pattern, const std::string& flags = "") {
    auto trimmed = pattern;
    while (!trimmed.empty() && trimmed.front() == ' ') trimmed.erase(trimmed.begin());
    while (!trimmed.empty() && trimmed.back() == ' ') trimmed.pop_back();
    if (trimmed.empty()) return std::nullopt;
    
    if (has_nested_repetition(trimmed)) {
        return CompiledConfigRegex{std::nullopt, trimmed, flags, ConfigRegexRejectReason::UnsafeNestedRepetition};
    }
    try {
        auto rx_flags = std::regex_constants::ECMAScript;
        if (flags.find('i') != std::string::npos) rx_flags |= std::regex_constants::icase;
        return CompiledConfigRegex{std::regex(trimmed, rx_flags), trimmed, flags, std::nullopt};
    } catch (...) {
        return CompiledConfigRegex{std::nullopt, trimmed, flags, ConfigRegexRejectReason::InvalidRegex};
    }
}

// 🎯 compileConfigRegexes (对齐 config-regex.ts:43-78)
struct CompiledConfigRegexes {
    std::vector<std::regex> regexes;
    std::vector<CompiledConfigRegex> rejected;
};

CompiledConfigRegexes compile_config_regexes(const std::vector<std::string>& patterns, const std::string& flags = "") {
    CompiledConfigRegexes result;
    for (const auto& p : patterns) {
        auto compiled = compile_config_regex(p, flags);
        if (!compiled) continue;
        if (compiled->regex) result.regexes.push_back(*compiled->regex);
        else result.rejected.push_back(*compiled);
    }
    return result;
}

// ─── secret-equal.ts (对齐 13 lines) ────────────────────────────────────

// 🎯 safeEqualSecret (对齐 secret-equal.ts:3-12)
// Uses SHA-256 + constant-time comparison to prevent timing attacks
bool safe_equal_secret(const std::string& provided, const std::string& expected) {
    if (provided.empty() || expected.empty()) return false;
    
    unsigned char hash_a[SHA256_DIGEST_LENGTH];
    unsigned char hash_b[SHA256_DIGEST_LENGTH];
    SHA256(reinterpret_cast<const unsigned char*>(provided.c_str()), provided.size(), hash_a);
    SHA256(reinterpret_cast<const unsigned char*>(expected.c_str()), expected.size(), hash_b);
    
    // Constant-time comparison
    unsigned char diff = 0;
    for (int i = 0; i < SHA256_DIGEST_LENGTH; i++) diff |= hash_a[i] ^ hash_b[i];
    return diff == 0;
}

}  // namespace config_deep
}  // namespace security
}  // namespace hunterclaw
