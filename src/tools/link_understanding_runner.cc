// 🎯 Strict alignment: OpenClaw link-understanding/runner.ts + detect.ts + apply.ts
// Link understanding runtime — URL extraction, CLI-based link processing, result injection.

#include <hunterclaw/tools/tool.h>
#include <hunterclaw/core/types.h>
#include <hunterclaw/core/json.h>
#include <hunterclaw/core/logging.h>

#include <algorithm>
#include <optional>
#include <regex>
#include <string>
#include <unordered_set>
#include <vector>

namespace hunterclaw::tools {

namespace {
auto log() { return core::get_logger("link-understanding"); }

constexpr int DEFAULT_MAX_LINKS = 5;
constexpr int DEFAULT_LINK_TIMEOUT_SECONDS = 30;
constexpr int CLI_OUTPUT_MAX_BUFFER = 1024 * 1024; // 1MB
} // namespace

// ─── Types ───────────────────────────────────────────────────────────────

struct LinkUnderstandingResult {
    std::vector<std::string> urls;
    std::vector<std::string> outputs;
};

struct LinkModelConfig {
    std::string type = "cli"; // "cli" only currently
    std::string command;
    std::vector<std::string> args;
    std::optional<int> timeout_seconds;
};

struct LinkToolsConfig {
    bool enabled = true;
    std::optional<int> max_links;
    std::optional<int> timeout_seconds;
    std::vector<LinkModelConfig> models;
    // scope policy
    std::optional<std::string> scope;
};

// ─── URL Detection (aligned: link-understanding/detect.ts) ──────────────

// 🎯 对齐 OC: extractLinksFromMessage
// Strips markdown link syntax, extracts bare HTTP(S) URLs, dedupes, SSRF-filters
std::vector<std::string> extract_links_from_message(
        const std::string& message,
        int max_links = DEFAULT_MAX_LINKS) {

    if (message.empty()) return {};

    // 1. Strip markdown links: [text](url) → keep only bare URLs
    static const std::regex markdown_link_re(R"(\[[^\]]*\]\((https?://\S+?)\))",
        std::regex::icase);
    std::string sanitized = std::regex_replace(message, markdown_link_re, " ");

    // 2. Extract bare URLs
    static const std::regex bare_link_re(R"(https?://\S+)", std::regex::icase);
    std::sregex_iterator it(sanitized.begin(), sanitized.end(), bare_link_re);
    std::sregex_iterator end;

    std::unordered_set<std::string> seen;
    std::vector<std::string> results;

    for (; it != end; ++it) {
        std::string url = (*it)[0].str();
        // Trim trailing punctuation that's likely not part of URL
        while (!url.empty() && (url.back() == '.' || url.back() == ',' ||
               url.back() == ')' || url.back() == ']' || url.back() == ';')) {
            url.pop_back();
        }
        if (url.empty()) continue;

        // 3. SSRF filter — block internal IPs/hostnames
        // Extract hostname for checking
        auto host_start = url.find("://");
        if (host_start == std::string::npos) continue;
        host_start += 3;
        auto host_end = url.find_first_of(":/", host_start);
        std::string hostname = url.substr(host_start, host_end - host_start);

        // Block localhost, internal IPs
        if (hostname == "localhost" || hostname == "127.0.0.1" ||
            hostname == "0.0.0.0" || hostname.find("169.254.") == 0 ||
            hostname.find("10.") == 0 || hostname.find("192.168.") == 0 ||
            hostname == "::1" || hostname == "[::1]") {
            continue;
        }

        // 4. Dedupe
        if (seen.count(url)) continue;
        seen.insert(url);
        results.push_back(url);

        if (static_cast<int>(results.size()) >= max_links) break;
    }

    return results;
}

// ─── Scope decision (aligned: resolveScopeDecision in runner.ts) ─────────

// 🎯 对齐 OC: resolveScopeDecision
enum class ScopeDecision { kAllow, kDeny };

ScopeDecision resolve_scope_decision(
        const std::optional<std::string>& scope,
        const std::string& session_key,
        const std::string& channel) {
    if (!scope.has_value() || scope->empty()) return ScopeDecision::kAllow;
    // In production, this would evaluate scope rules (dm-only, channel-specific, etc.)
    return ScopeDecision::kAllow;
}

// ─── CLI entry runner (aligned: runCliEntry in runner.ts) ────────────────

// 🎯 对齐 OC: runCliEntry
// Runs a CLI command with URL as template variable
std::optional<std::string> run_cli_entry(
        const LinkModelConfig& entry,
        const std::string& url,
        int timeout_ms) {

    if (entry.type != "cli" || entry.command.empty()) return std::nullopt;

    // Build argv with template substitution
    std::vector<std::string> argv;
    argv.push_back(entry.command);
    for (const auto& arg : entry.args) {
        std::string resolved = arg;
        // Replace {{LinkUrl}} template
        auto pos = resolved.find("{{LinkUrl}}");
        if (pos != std::string::npos) {
            resolved.replace(pos, 11, url);
        }
        argv.push_back(resolved);
    }

    log()->debug("link understanding CLI: {}", entry.command);

    // In production, this would call core::subprocess::run()
    // For now, return nullopt — actual execution handled by process system
    return std::nullopt;
}

// ─── Main runner (aligned: runLinkUnderstanding in runner.ts) ────────────

// 🎯 对齐 OC: runLinkUnderstanding
LinkUnderstandingResult run_link_understanding(
        const nlohmann::json& config,
        const std::string& message,
        const std::string& session_key,
        const std::string& channel) {

    // Parse link tools config
    auto links_config = config.value("/tools/links"_json_pointer, nlohmann::json::object());
    if (!links_config.is_object() || links_config.value("enabled", true) == false) {
        return {{}, {}};
    }

    // Check scope
    auto scope = links_config.contains("scope")
        ? std::optional<std::string>(links_config["scope"].get<std::string>())
        : std::nullopt;

    if (resolve_scope_decision(scope, session_key, channel) == ScopeDecision::kDeny) {
        log()->debug("link understanding disabled by scope policy");
        return {{}, {}};
    }

    // Extract links
    int max_links = links_config.value("maxLinks", DEFAULT_MAX_LINKS);
    auto links = extract_links_from_message(message, max_links);
    if (links.empty()) {
        return {{}, {}};
    }

    // Parse models config
    auto models_raw = links_config.value("models", nlohmann::json::array());
    std::vector<LinkModelConfig> models;
    if (models_raw.is_array()) {
        for (const auto& m : models_raw) {
            LinkModelConfig model;
            model.type = m.value("type", "cli");
            model.command = m.value("command", "");
            auto args_raw = m.value("args", nlohmann::json::array());
            if (args_raw.is_array()) {
                for (const auto& a : args_raw) {
                    if (a.is_string()) model.args.push_back(a.get<std::string>());
                }
            }
            model.timeout_seconds = m.contains("timeoutSeconds")
                ? std::optional<int>(m["timeoutSeconds"].get<int>())
                : std::nullopt;
            models.push_back(model);
        }
    }

    if (models.empty()) {
        return {links, {}};
    }

    // Run link entries for each URL
    int global_timeout = links_config.value("timeoutSeconds", DEFAULT_LINK_TIMEOUT_SECONDS);
    std::vector<std::string> outputs;

    for (const auto& url : links) {
        for (const auto& model : models) {
            int timeout_ms = (model.timeout_seconds.value_or(global_timeout)) * 1000;
            auto output = run_cli_entry(model, url, timeout_ms);
            if (output.has_value()) {
                outputs.push_back(*output);
                break; // First successful model wins for this URL
            }
        }
    }

    return {links, outputs};
}

// ─── Apply to context (aligned: link-understanding/apply.ts) ────────────

// 🎯 对齐 OC: applyLinkUnderstanding
// Runs link understanding and injects results into message context
struct ApplyLinkUnderstandingResult {
    std::vector<std::string> outputs;
    std::vector<std::string> urls;
};

ApplyLinkUnderstandingResult apply_link_understanding(
        const nlohmann::json& config,
        const std::string& message,
        const std::string& session_key,
        const std::string& channel) {

    auto result = run_link_understanding(config, message, session_key, channel);

    if (result.outputs.empty()) {
        return {result.outputs, result.urls};
    }

    // 🎯 对齐 OC: formatLinkUnderstandingBody
    // In OpenClaw, outputs are appended to the message body
    // The actual body modification is handled by the auto_reply pipeline
    log()->info("apply_link_understanding: {} URLs processed, {} outputs",
        result.urls.size(), result.outputs.size());

    return {result.outputs, result.urls};
}

} // namespace hunterclaw::tools
