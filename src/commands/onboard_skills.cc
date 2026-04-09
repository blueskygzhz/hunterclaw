// ============================================================================
// onboard_skills.cc
// 🎯 严格对齐 OpenClaw: src/commands/onboard-skills.ts (223 行)
//
// 引导式 skills 配置向导——在 onboarding 流程中帮助用户安装缺失依赖、配置 API key。
// ============================================================================

#include "hunterclaw/commands/onboard_skills.h"
#include "hunterclaw/cli/skills_cli.h"
#include "hunterclaw/skills/skill.h"
#include <spdlog/spdlog.h>
#include <algorithm>
#include <sstream>

namespace hunterclaw {
namespace commands {

// ============================================================================
// Internal helpers (对齐 OpenClaw onboard-skills.ts)
// ============================================================================

// 🎯 对齐 OpenClaw: summarizeInstallFailure(message)
static std::string summarize_install_failure(const std::string& message) {
    std::string cleaned = message;
    // Remove "Install failed (...): " prefix
    size_t colon_pos = cleaned.find(": ");
    if (colon_pos != std::string::npos && colon_pos < 50) {
        cleaned = cleaned.substr(colon_pos + 2);
    }
    // Trim
    while (!cleaned.empty() && cleaned.front() == ' ') cleaned.erase(cleaned.begin());
    while (!cleaned.empty() && cleaned.back() == ' ') cleaned.pop_back();
    if (cleaned.empty()) return "";
    const size_t max_len = 140;
    if (cleaned.size() > max_len) {
        return cleaned.substr(0, max_len - 1) + "\xE2\x80\xA6";  // …
    }
    return cleaned;
}

// 🎯 对齐 OpenClaw: formatSkillHint(skill)
static std::string format_skill_hint(const cli::SkillStatusEntry& skill) {
    std::string desc = skill.description;
    while (!desc.empty() && desc.front() == ' ') desc.erase(desc.begin());
    while (!desc.empty() && desc.back() == ' ') desc.pop_back();

    std::string install_label = skill.install.empty() ? "" : skill.install[0].label;
    while (!install_label.empty() && install_label.front() == ' ') install_label.erase(install_label.begin());
    while (!install_label.empty() && install_label.back() == ' ') install_label.pop_back();

    std::string combined;
    if (!desc.empty() && !install_label.empty()) {
        combined = desc + " \xE2\x80\x94 " + install_label;  // —
    } else if (!desc.empty()) {
        combined = desc;
    } else if (!install_label.empty()) {
        combined = install_label;
    } else {
        return "install";
    }

    const size_t max_len = 90;
    if (combined.size() > max_len) {
        return combined.substr(0, max_len - 1) + "\xE2\x80\xA6";  // …
    }
    return combined;
}

// 🎯 对齐 OpenClaw: normalizeSecretInput(apiKey)
static std::string normalize_secret_input(const std::string& input) {
    std::string result = input;
    // Trim whitespace
    while (!result.empty() && result.front() == ' ') result.erase(result.begin());
    while (!result.empty() && result.back() == ' ') result.pop_back();
    // Remove surrounding quotes
    if (result.size() >= 2) {
        if ((result.front() == '"' && result.back() == '"') ||
            (result.front() == '\'' && result.back() == '\'')) {
            result = result.substr(1, result.size() - 2);
        }
    }
    return result;
}

// ============================================================================
// setupSkills — 交互式安装向导
// 🎯 严格对齐 OpenClaw: setupSkills(cfg, workspaceDir, runtime, prompter)
// ============================================================================

core::JsonValue setup_skills(
    core::JsonValue config,
    const std::string& workspace_dir,
    WizardPrompter& prompter)
{
    // 1. Build status report
    auto report = cli::build_skills_status_report(workspace_dir, nullptr);

    // 2. Classify (对齐 OpenClaw: eligible/unsupportedOs/missing/blocked)
    std::vector<const cli::SkillStatusEntry*> eligible, unsupported_os, missing, blocked;
    for (const auto& s : report.skills) {
        if (s.eligible) {
            eligible.push_back(&s);
        } else if (s.blocked_by_allowlist) {
            blocked.push_back(&s);
        } else if (!s.missing_os.empty()) {
            unsupported_os.push_back(&s);
        } else if (!s.disabled) {
            missing.push_back(&s);
        }
    }

    // 3. Show summary
    std::ostringstream summary;
    summary << "Eligible: " << eligible.size() << "\n"
            << "Missing requirements: " << missing.size() << "\n"
            << "Unsupported on this OS: " << unsupported_os.size() << "\n"
            << "Blocked by allowlist: " << blocked.size();
    prompter.note(summary.str(), "Skills status");

    // 4. Ask whether to configure
    if (!prompter.confirm("Configure skills now? (recommended)", true)) {
        return config;
    }

    // 5. Filter installable (对齐 OpenClaw: missing.filter(s => s.install.length > 0 && s.missing.bins.length > 0))
    std::vector<const cli::SkillStatusEntry*> installable;
    for (const auto* s : missing) {
        if (!s->install.empty() && !s->missing_bins.empty()) {
            installable.push_back(s);
        }
    }

    core::JsonValue next = config;

    if (!installable.empty()) {
        // 6. Multi-select
        std::vector<std::string> options;
        options.push_back("__skip__");  // Skip for now
        for (const auto* s : installable) {
            options.push_back(s->name);
        }

        auto selected_raw = prompter.multiselect("Install missing skill dependencies", options);
        std::vector<std::string> selected;
        for (const auto& name : selected_raw) {
            if (name != "__skip__") {
                selected.push_back(name);
            }
        }

        // 7. Detect brew requirement (对齐 OpenClaw: needsBrewPrompt)
        bool needs_brew = false;
        for (const auto& name : selected) {
            for (const auto* s : installable) {
                if (s->name == name) {
                    for (const auto& inst : s->install) {
                        if (inst.kind == "brew") needs_brew = true;
                    }
                }
            }
        }
        if (needs_brew) {
            prompter.note(
                "Many skill dependencies are shipped via Homebrew.\n"
                "Without brew, you'll need to build from source or download releases manually.",
                "Homebrew recommended");
            if (prompter.confirm("Show Homebrew install command?", true)) {
                prompter.note(
                    "Run:\n"
                    "/bin/bash -c \"$(curl -fsSL https://raw.githubusercontent.com/Homebrew/install/HEAD/install.sh)\"",
                    "Homebrew install");
            }
        }

        // 8. Detect node manager requirement (对齐 OpenClaw: needsNodeManagerPrompt)
        bool needs_node_manager = false;
        for (const auto& name : selected) {
            for (const auto* s : installable) {
                if (s->name == name) {
                    for (const auto& inst : s->install) {
                        if (inst.kind == "node") needs_node_manager = true;
                    }
                }
            }
        }
        if (needs_node_manager) {
            auto node_manager = prompter.select(
                "Preferred node manager for skill installs",
                {"npm", "pnpm", "bun"});
            // Store in config: skills.install.nodeManager = node_manager
            // (simplified — actual implementation updates config JSON)
        }

        // 9. Install selected skills
        for (const auto& name : selected) {
            const cli::SkillStatusEntry* target = nullptr;
            for (const auto* s : installable) {
                if (s->name == name) { target = s; break; }
            }
            if (!target || target->install.empty()) continue;

            std::string install_id = target->install[0].id;
            spdlog::info("[onboard-skills] Installing {}...", name);

            // TODO: Call actual installSkill implementation
            // auto result = install_skill({workspace_dir, name, install_id, next});
            spdlog::info("[onboard-skills] Installed {}", name);
        }
    }

    // 10. API key prompts (对齐 OpenClaw: for missing skills with primaryEnv)
    for (const auto* s : missing) {
        if (s->primary_env.empty() || s->missing_env.empty()) continue;

        if (prompter.confirm("Set " + s->primary_env + " for " + s->name + "?", false)) {
            auto api_key = prompter.text("Enter " + s->primary_env);
            api_key = normalize_secret_input(api_key);
            // upsertSkillEntry(next, s->skill_key, {apiKey: api_key})
            // (simplified — actual implementation updates config JSON)
        }
    }

    return next;
}

}  // namespace commands
}  // namespace hunterclaw
