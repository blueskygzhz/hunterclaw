// 🎯 对齐 OpenClaw: src/cli/security-cli.ts
// 实现 `hunterclaw security audit` CLI 子命令
// 调用 SecurityAuditor::run_audit + fix_security_footguns

#include "hunterclaw/security/audit.h"
#include "hunterclaw/core/config.h"
#include <iostream>
#include <string>
#include <vector>
#include <nlohmann/json.hpp>

namespace hunterclaw {
namespace cli {

using json = nlohmann::json;
using namespace security;

// ============================================================================
// Format helpers - 对齐 OpenClaw security-cli.ts formatSummary
// ============================================================================

static std::string format_summary(const SecurityAuditSummary& summary) {
    return std::to_string(summary.critical) + " critical · " +
           std::to_string(summary.warn) + " warn · " +
           std::to_string(summary.info) + " info";
}

static std::string shorten_home_path(const std::string& path) {
    const char* home = getenv("HOME");
    if (!home) return path;
    std::string home_str(home);
    if (path.rfind(home_str, 0) == 0) {
        return "~" + path.substr(home_str.size());
    }
    return path;
}

// ============================================================================
// register_security_cli - 注册 security 子命令
// 对齐 OpenClaw: registerSecurityCli(program)
// ============================================================================

struct SecurityAuditCliOptions {
    bool json_output = false;
    bool deep = false;
    bool fix = false;
};

int run_security_audit_command(const SecurityAuditCliOptions& opts) {
    // 🎯 对齐 OpenClaw: const fixResult = opts.fix ? await fixSecurityFootguns() : null
    std::optional<SecurityFixResult> fix_result;
    if (opts.fix) {
        fix_result = fix_security_footguns();
    }

    // 🎯 对齐 OpenClaw: const cfg = loadConfig()
    // Load config for audit
    SecurityAuditOptions audit_opts;
    audit_opts.include_filesystem = true;
    audit_opts.include_channel_security = true;

    // 🎯 对齐 OpenClaw: const report = await runSecurityAudit({...})
    SecurityAuditor auditor;
    auto report = auditor.run_audit(audit_opts);

    // 🎯 对齐 OpenClaw: if (opts.json) { ... }
    if (opts.json_output) {
        json output;
        if (fix_result.has_value()) {
            output["fix"] = fix_result->to_json();
            output["report"] = report.to_json();
        } else {
            output = report.to_json();
        }
        std::cout << output.dump(2) << std::endl;
        return 0;
    }

    // 🎯 对齐 OpenClaw: human-readable output
    std::vector<std::string> lines;
    lines.push_back("HunterClaw security audit");
    lines.push_back("Summary: " + format_summary(report.summary));
    lines.push_back("Run deeper: hunterclaw security audit --deep");

    // 🎯 对齐 OpenClaw: fix result output
    if (opts.fix) {
        lines.push_back("Fix: hunterclaw security audit --fix");
        if (!fix_result.has_value()) {
            lines.push_back("Fixes: failed to apply (unexpected error)");
        } else if (fix_result->errors.empty() && fix_result->changes.empty() &&
                   std::all_of(fix_result->actions.begin(), fix_result->actions.end(),
                               [](const SecurityFixAction& a) { return !a.ok; })) {
            lines.push_back("Fixes: no changes applied");
        } else {
            lines.push_back("");
            lines.push_back("FIX");
            for (const auto& change : fix_result->changes) {
                lines.push_back("  " + shorten_home_path(change));
            }
            for (const auto& action : fix_result->actions) {
                if (action.kind == "chmod") {
                    // Format octal mode
                    char mode_buf[8];
                    snprintf(mode_buf, sizeof(mode_buf), "%03o", action.mode);
                    std::string mode_str(mode_buf);
                    if (action.ok) {
                        lines.push_back("  chmod " + mode_str + " " + shorten_home_path(action.path));
                    } else if (!action.skipped.empty()) {
                        lines.push_back("  skip chmod " + mode_str + " " + shorten_home_path(action.path) + " (" + action.skipped + ")");
                    } else if (!action.error.empty()) {
                        lines.push_back("  chmod " + mode_str + " " + shorten_home_path(action.path) + " failed: " + action.error);
                    }
                }
            }
            for (const auto& err : fix_result->errors) {
                lines.push_back("  error: " + shorten_home_path(err));
            }
        }
    }

    // 🎯 对齐 OpenClaw: render findings by severity
    auto render_severity = [&](const std::string& sev_label, SecurityAuditSeverity sev) {
        std::vector<SecurityAuditFinding> filtered;
        for (const auto& f : report.findings) {
            if (f.severity == sev) filtered.push_back(f);
        }
        if (filtered.empty()) return;
        lines.push_back("");
        lines.push_back(sev_label);
        for (const auto& f : filtered) {
            lines.push_back(f.check_id + " " + f.title);
            lines.push_back("  " + f.detail);
            if (f.remediation.has_value() && !f.remediation->empty()) {
                lines.push_back("  Fix: " + *f.remediation);
            }
        }
    };

    render_severity("CRITICAL", SecurityAuditSeverity::Critical);
    render_severity("WARN", SecurityAuditSeverity::Warn);
    render_severity("INFO", SecurityAuditSeverity::Info);

    for (const auto& line : lines) {
        std::cout << line << "\n";
    }

    return 0;
}

// 🎯 对齐 OpenClaw: registerSecurityCli export
void register_security_commands(
    std::function<void(const std::string&, const std::string&,
                       std::function<int(const std::vector<std::string>&)>)> register_command
) {
    register_command(
        "security audit",
        "Audit config + local state for common security foot-guns",
        [](const std::vector<std::string>& args) -> int {
            SecurityAuditCliOptions opts;
            for (size_t i = 0; i < args.size(); ++i) {
                if (args[i] == "--json") opts.json_output = true;
                else if (args[i] == "--deep") opts.deep = true;
                else if (args[i] == "--fix") opts.fix = true;
            }
            return run_security_audit_command(opts);
        }
    );
}

}  // namespace cli
}  // namespace hunterclaw
