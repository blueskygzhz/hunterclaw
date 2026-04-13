// 🎯 Strict alignment: OpenClaw security/audit-extra.sync.ts (1139)
//   + audit-extra.async.ts (1366) + audit-extra.summary.ts (298)
//   + audit-deep-code-safety.ts (34) + audit-deep-probe-findings.ts (29)
// Deep security audit collectors: sandbox, hooks, gateway, model hygiene, exposure matrix.

#include "hunterclaw/security/audit.h"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <regex>
#include <set>

namespace hunterclaw {
namespace security {
namespace audit_deep {

using json = nlohmann::json;
namespace fs = std::filesystem;

// ─── Helpers ─────────────────────────────────────────────────────────────

static bool is_probably_synced_path(const std::string& p) {
    std::string s = p;
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c){ return std::tolower(c); });
    return s.find("icloud") != std::string::npos ||
           s.find("dropbox") != std::string::npos ||
           s.find("google drive") != std::string::npos ||
           s.find("onedrive") != std::string::npos;
}

static bool looks_like_env_ref(const std::string& v) {
    auto t = v;
    while (!t.empty() && t.front() == ' ') t.erase(t.begin());
    while (!t.empty() && t.back() == ' ') t.pop_back();
    return t.size() >= 3 && t.substr(0,2) == "${" && t.back() == '}';
}

static std::string norm_lc(const std::string& s) {
    std::string r;
    for (char c : s) r += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    auto a = r.find_first_not_of(" \t");
    if (a == std::string::npos) return "";
    return r.substr(a, r.find_last_not_of(" \t") - a + 1);
}

// ─── Sync Collectors (对齐 audit-extra.sync.ts) ─────────────────────────

// 🎯 collectSyncedFolderFindings (对齐 audit-extra.sync.ts:467-482)
std::vector<SecurityAuditFinding> collect_synced_folder_findings(
    const std::string& state_dir, const std::string& config_path)
{
    std::vector<SecurityAuditFinding> f;
    if (is_probably_synced_path(state_dir) || is_probably_synced_path(config_path)) {
        f.push_back({"fs.synced_dir", SecurityAuditSeverity::Warn,
            "State/config path looks like a synced folder",
            "stateDir=" + state_dir + ", configPath=" + config_path +
            ". Synced folders can leak tokens and transcripts.",
            "Keep HUNTERCLAW_STATE_DIR on a local-only volume."});
    }
    return f;
}

// 🎯 collectSecretsInConfigFindings (对齐 audit-extra.sync.ts:484-511)
std::vector<SecurityAuditFinding> collect_secrets_in_config_findings(const json& cfg) {
    std::vector<SecurityAuditFinding> f;
    auto gw = cfg.value("gateway", json::object());
    auto auth = gw.value("auth", json::object());
    auto pw = auth.value("password", "");
    if (!pw.empty() && !looks_like_env_ref(pw)) {
        f.push_back({"config.secrets.gateway_password_in_config", SecurityAuditSeverity::Warn,
            "Gateway password is stored in config",
            "gateway.auth.password is set in the config file; prefer environment variables.",
            "Prefer HUNTERCLAW_GATEWAY_PASSWORD (env) and remove gateway.auth.password."});
    }
    auto hooks = cfg.value("hooks", json::object());
    auto htok = hooks.value("token", "");
    if (hooks.value("enabled", false) && !htok.empty() && !looks_like_env_ref(htok)) {
        f.push_back({"config.secrets.hooks_token_in_config", SecurityAuditSeverity::Info,
            "Hooks token is stored in config",
            "hooks.token is set in config; keep config perms tight."});
    }
    return f;
}

// 🎯 collectHooksHardeningFindings (对齐 audit-extra.sync.ts:513-629)
std::vector<SecurityAuditFinding> collect_hooks_hardening_findings(const json& cfg) {
    std::vector<SecurityAuditFinding> f;
    auto hooks = cfg.value("hooks", json::object());
    if (hooks.value("enabled", false) != true) return f;
    
    auto token = hooks.value("token", "");
    if (!token.empty() && token.size() < 24) {
        f.push_back({"hooks.token_too_short", SecurityAuditSeverity::Warn,
            "Hooks token looks short",
            "hooks.token is " + std::to_string(token.size()) + " chars; prefer a long random token."});
    }
    auto path = hooks.value("path", "");
    if (path == "/") {
        f.push_back({"hooks.path_root", SecurityAuditSeverity::Critical,
            "Hooks base path is '/'",
            "hooks.path='/' would shadow other HTTP endpoints and is unsafe.",
            "Use a dedicated path like '/hooks'."});
    }
    if (hooks.value("allowRequestSessionKey", false)) {
        f.push_back({"hooks.request_session_key_enabled", SecurityAuditSeverity::Warn,
            "External hook payloads may override sessionKey",
            "hooks.allowRequestSessionKey=true allows callers to choose the session key.",
            "Set hooks.allowRequestSessionKey=false or constrain allowedSessionKeyPrefixes."});
    }
    return f;
}

// 🎯 collectSandboxDockerNoopFindings (对齐 audit-extra.sync.ts:692-740)
std::vector<SecurityAuditFinding> collect_sandbox_docker_noop_findings(const json& cfg) {
    std::vector<SecurityAuditFinding> f;
    auto agents = cfg.value("agents", json::object());
    auto defaults = agents.value("defaults", json::object());
    auto sandbox = defaults.value("sandbox", json::object());
    auto mode = sandbox.value("mode", "off");
    auto docker = sandbox.value("docker", json::object());
    
    if (mode == "off" && !docker.empty()) {
        f.push_back({"sandbox.docker_config_mode_off", SecurityAuditSeverity::Warn,
            "Sandbox docker settings configured while sandbox mode is off",
            "Docker settings will not take effect until sandbox mode is enabled.",
            "Enable sandbox mode or remove unused docker settings."});
    }
    return f;
}

// 🎯 collectSandboxDangerousConfigFindings (对齐 audit-extra.sync.ts:742-852)
std::vector<SecurityAuditFinding> collect_sandbox_dangerous_config_findings(const json& cfg) {
    std::vector<SecurityAuditFinding> f;
    auto agents = cfg.value("agents", json::object());
    auto defaults = agents.value("defaults", json::object());
    auto sandbox = defaults.value("sandbox", json::object());
    auto docker = sandbox.value("docker", json::object());
    
    // Check binds
    auto binds = docker.value("binds", std::vector<std::string>{});
    for (const auto& bind : binds) {
        // Non-absolute source path check
        auto colon = bind.find(':');
        if (colon == std::string::npos) continue;
        auto source = bind.substr(0, colon);
        while (!source.empty() && source.front() == ' ') source.erase(source.begin());
        if (!source.empty() && source[0] != '/') {
            f.push_back({"sandbox.bind_mount_non_absolute", SecurityAuditSeverity::Warn,
                "Sandbox bind mount uses a non-absolute source path",
                "agents.defaults.sandbox.docker.binds contains \"" + bind + "\" with non-absolute source.",
                "Use absolute host paths."});
        }
    }
    
    // Check network mode
    auto network = norm_lc(docker.value("network", ""));
    if (network == "host") {
        f.push_back({"sandbox.dangerous_network_mode", SecurityAuditSeverity::Critical,
            "Dangerous network mode in sandbox config",
            "agents.defaults.sandbox.docker.network is \"host\" which bypasses container network isolation.",
            "Set to \"bridge\", \"none\", or a custom bridge network."});
    }
    
    // Check seccomp/apparmor
    auto seccomp = norm_lc(docker.value("seccompProfile", ""));
    if (seccomp == "unconfined") {
        f.push_back({"sandbox.dangerous_seccomp_profile", SecurityAuditSeverity::Critical,
            "Seccomp unconfined in sandbox config",
            "agents.defaults.sandbox.docker.seccompProfile is \"unconfined\".",
            "Remove or use a custom seccomp profile."});
    }
    auto apparmor = norm_lc(docker.value("apparmorProfile", ""));
    if (apparmor == "unconfined") {
        f.push_back({"sandbox.dangerous_apparmor_profile", SecurityAuditSeverity::Critical,
            "AppArmor unconfined in sandbox config",
            "agents.defaults.sandbox.docker.apparmorProfile is \"unconfined\".",
            "Remove or use a named profile."});
    }
    return f;
}

// 🎯 collectModelHygieneFindings (对齐 audit-extra.sync.ts:984-1067)
std::vector<SecurityAuditFinding> collect_model_hygiene_findings(const json& cfg) {
    std::vector<SecurityAuditFinding> f;
    auto agents = cfg.value("agents", json::object());
    auto defaults = agents.value("defaults", json::object());
    auto model = defaults.value("model", json());
    
    if (model.is_string()) {
        std::string m = model.get<std::string>();
        // Legacy model detection
        static const std::regex gpt35_re("\\bgpt-3\\.5\\b", std::regex::icase);
        static const std::regex claude2_re("\\bclaude-(instant|2)\\b", std::regex::icase);
        if (std::regex_search(m, gpt35_re) || std::regex_search(m, claude2_re)) {
            f.push_back({"models.legacy", SecurityAuditSeverity::Warn,
                "Some configured models look legacy",
                "Model \"" + m + "\" is a legacy model with weaker prompt injection resistance.",
                "Prefer modern, instruction-hardened models."});
        }
        // Weak tier detection (Haiku)
        static const std::regex haiku_re("\\bhaiku\\b", std::regex::icase);
        if (std::regex_search(m, haiku_re)) {
            f.push_back({"models.weak_tier", SecurityAuditSeverity::Warn,
                "Some configured models are below recommended tiers",
                "Model \"" + m + "\" is a smaller model more susceptible to prompt injection.",
                "Use top-tier models for bots with tools or untrusted inboxes."});
        }
    }
    return f;
}

// 🎯 collectExposureMatrixFindings (对齐 audit-extra.sync.ts:1069-1106)
std::vector<SecurityAuditFinding> collect_exposure_matrix_findings(const json& cfg) {
    std::vector<SecurityAuditFinding> f;
    auto channels = cfg.value("channels", json::object());
    std::vector<std::string> open_groups;
    for (auto& [ch_id, ch_val] : channels.items()) {
        if (!ch_val.is_object()) continue;
        if (ch_val.value("groupPolicy", "") == "open")
            open_groups.push_back("channels." + ch_id + ".groupPolicy");
    }
    if (open_groups.empty()) return f;
    
    auto tools = cfg.value("tools", json::object());
    bool elevated = tools.value("elevated", json::object()).value("enabled", true);
    if (elevated) {
        std::string paths;
        for (const auto& p : open_groups) paths += "- " + p + "\n";
        f.push_back({"security.exposure.open_groups_with_elevated", SecurityAuditSeverity::Critical,
            "Open groupPolicy with elevated tools enabled",
            "Found groupPolicy=\"open\" at:\n" + paths +
            "With tools.elevated enabled, prompt injection can escalate.",
            "Set groupPolicy=\"allowlist\" and keep elevated allowlists tight."});
    }
    return f;
}

// 🎯 collectLikelyMultiUserSetupFindings (对齐 audit-extra.sync.ts:1108-1138)
std::vector<SecurityAuditFinding> collect_likely_multi_user_findings(const json& cfg) {
    std::vector<SecurityAuditFinding> f;
    auto channels = cfg.value("channels", json::object());
    std::vector<std::string> signals;
    for (auto& [ch_id, ch_val] : channels.items()) {
        if (!ch_val.is_object()) continue;
        if (ch_val.value("groupPolicy", "") == "open")
            signals.push_back("channels." + ch_id + ".groupPolicy=\"open\"");
        if (ch_val.value("dmPolicy", "") == "open")
            signals.push_back("channels." + ch_id + ".dmPolicy=\"open\"");
        auto af = ch_val.value("allowFrom", std::vector<std::string>{});
        for (const auto& e : af) if (e == "*")
            signals.push_back("channels." + ch_id + ".allowFrom includes \"*\"");
    }
    if (signals.empty()) return f;
    std::string sig_str;
    for (const auto& s : signals) sig_str += "- " + s + "\n";
    f.push_back({"security.trust_model.multi_user_heuristic", SecurityAuditSeverity::Warn,
        "Potential multi-user setup detected",
        "Heuristic signals indicate multiple users:\n" + sig_str +
        "HunterClaw's default model is personal-assistant, not multi-tenant.",
        "Split trust boundaries or enable sandbox.mode=\"all\"."});
    return f;
}

// 🎯 collectGatewayHttpNoAuthFindings (对齐 audit-extra.sync.ts:658-690)
std::vector<SecurityAuditFinding> collect_gateway_http_no_auth_findings(const json& cfg) {
    std::vector<SecurityAuditFinding> f;
    auto gw = cfg.value("gateway", json::object());
    auto auth = gw.value("auth", json::object());
    auto mode = auth.value("mode", "token");
    if (mode != "none") return f;
    f.push_back({"gateway.http.no_auth", SecurityAuditSeverity::Warn,
        "Gateway HTTP APIs are reachable without auth",
        "gateway.auth.mode=\"none\" leaves APIs callable without a shared secret.",
        "Set gateway.auth.mode to token/password (recommended)."});
    return f;
}

// 🎯 collectMinimalProfileOverrideFindings (对齐 audit-extra.sync.ts:948-982)
std::vector<SecurityAuditFinding> collect_minimal_profile_override_findings(const json& cfg) {
    std::vector<SecurityAuditFinding> f;
    auto tools = cfg.value("tools", json::object());
    if (tools.value("profile", "") != "minimal") return f;
    auto agents = cfg.value("agents", json::object());
    auto list = agents.value("list", json::array());
    std::vector<std::string> overrides;
    for (const auto& agent : list) {
        if (!agent.is_object()) continue;
        auto at = agent.value("tools", json::object());
        auto profile = at.value("profile", "");
        if (!profile.empty() && profile != "minimal") {
            overrides.push_back(agent.value("id","?") + "=" + profile);
        }
    }
    if (overrides.empty()) return f;
    std::string detail = "Global minimal profile overridden by:\n";
    for (const auto& o : overrides) detail += "- agents.list." + o + "\n";
    f.push_back({"tools.profile_minimal_overridden", SecurityAuditSeverity::Warn,
        "Global tools.profile=minimal is overridden by agent profiles",
        detail, "Set those agents to tools.profile=\"minimal\" or remove the override."});
    return f;
}

// 🎯 collectAttackSurfaceSummaryFindings (对齐 audit-extra.summary.ts:177-204)
std::vector<SecurityAuditFinding> collect_attack_surface_summary(const json& cfg) {
    auto channels = cfg.value("channels", json::object());
    int open = 0, allowlist = 0;
    for (auto& [_, v] : channels.items()) {
        if (!v.is_object()) continue;
        auto gp = v.value("groupPolicy", "");
        if (gp == "open") open++;
        else if (gp == "allowlist") allowlist++;
    }
    auto tools = cfg.value("tools", json::object());
    bool elevated = tools.value("elevated", json::object()).value("enabled", true);
    auto hooks = cfg.value("hooks", json::object());
    bool webhooks = hooks.value("enabled", false);
    
    std::string detail = "groups: open=" + std::to_string(open) + ", allowlist=" + std::to_string(allowlist) +
        "\ntools.elevated: " + (elevated ? "enabled" : "disabled") +
        "\nhooks.webhooks: " + (webhooks ? "enabled" : "disabled") +
        "\ntrust model: personal assistant";
    return {{"summary.attack_surface", SecurityAuditSeverity::Info,
             "Attack surface summary", detail}};
}

// 🎯 collectSmallModelRiskFindings (对齐 audit-extra.summary.ts:207-297)
std::vector<SecurityAuditFinding> collect_small_model_risk_findings(const json& cfg) {
    std::vector<SecurityAuditFinding> f;
    auto agents = cfg.value("agents", json::object());
    auto defaults = agents.value("defaults", json::object());
    auto model_json = defaults.value("model", json());
    if (!model_json.is_string()) return f;
    std::string model = model_json.get<std::string>();
    // Simple param-B extraction from model name
    static const std::regex param_re("(\\d+)[bB]\\b");
    std::smatch match;
    if (!std::regex_search(model, match, param_re)) return f;
    int param_b = std::stoi(match[1].str());
    if (param_b > 300) return f;
    
    auto sandbox = defaults.value("sandbox", json::object());
    auto sandbox_mode = sandbox.value("mode", "off");
    bool safe = sandbox_mode == "all";
    f.push_back({"models.small_params", safe ? SecurityAuditSeverity::Info : SecurityAuditSeverity::Critical,
        "Small models require sandboxing and web tools disabled",
        "Model \"" + model + "\" (~" + std::to_string(param_b) + "B params) detected. " +
        (safe ? "Sandbox=all is configured." : "Sandbox is not enabled for all sessions."),
        "Enable agents.defaults.sandbox.mode=\"all\" and deny web tools for small models."});
    return f;
}

// 🎯 collectDeepProbeFindings (对齐 audit-deep-probe-findings.ts:4-28)
std::vector<SecurityAuditFinding> collect_deep_probe_findings(
    bool attempted, bool ok, const std::string& error, const std::string& auth_warning)
{
    std::vector<SecurityAuditFinding> f;
    if (attempted && !ok) {
        f.push_back({"gateway.probe_failed", SecurityAuditSeverity::Warn,
            "Gateway probe failed (deep)",
            error.empty() ? "gateway unreachable" : error,
            "Run status --all to debug connectivity, then re-run security audit --deep."});
    }
    if (!auth_warning.empty()) {
        f.push_back({"gateway.probe_auth_secretref_unavailable", SecurityAuditSeverity::Warn,
            "Gateway probe auth SecretRef is unavailable", auth_warning,
            "Set HUNTERCLAW_GATEWAY_TOKEN/PASSWORD in this shell."});
    }
    return f;
}

// ─── Master deep audit runner ────────────────────────────────────────────

// 🎯 Aggregate all deep audit findings (对齐 audit.ts:1273-1384 runSecurityAudit)
std::vector<SecurityAuditFinding> run_deep_audit_collectors(
    const json& cfg, const std::string& state_dir, const std::string& config_path)
{
    std::vector<SecurityAuditFinding> all;
    auto append = [&](std::vector<SecurityAuditFinding>&& v) {
        all.insert(all.end(), std::make_move_iterator(v.begin()), std::make_move_iterator(v.end()));
    };
    
    append(collect_attack_surface_summary(cfg));
    append(collect_synced_folder_findings(state_dir, config_path));
    append(collect_secrets_in_config_findings(cfg));
    append(collect_hooks_hardening_findings(cfg));
    append(collect_gateway_http_no_auth_findings(cfg));
    append(collect_sandbox_docker_noop_findings(cfg));
    append(collect_sandbox_dangerous_config_findings(cfg));
    append(collect_minimal_profile_override_findings(cfg));
    append(collect_model_hygiene_findings(cfg));
    append(collect_small_model_risk_findings(cfg));
    append(collect_exposure_matrix_findings(cfg));
    append(collect_likely_multi_user_findings(cfg));
    
    return all;
}

}  // namespace audit_deep
}  // namespace security
}  // namespace hunterclaw
