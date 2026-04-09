// ============================================================================
// skills_cli.cc
// 🎯 严格对齐 OpenClaw: src/cli/skills-cli.ts (82 行)
//                        src/cli/skills-cli.format.ts (333 行)
//
// CLI 命令: skills list / skills info <name> / skills check
// ============================================================================

#include "hunterclaw/cli/skills_cli.h"
#include "hunterclaw/skills/skill.h"
#include <spdlog/spdlog.h>
#include <algorithm>
#include <iomanip>
#include <iostream>
#include <sstream>

namespace hunterclaw {
namespace cli {

// ============================================================================
// Formatting helpers (对齐 OpenClaw skills-cli.format.ts)
// ============================================================================

// 🎯 对齐 OpenClaw: normalizeSkillEmoji(emoji?)
static std::string normalize_skill_emoji(const std::string& emoji) {
    if (emoji.empty()) return "\xF0\x9F\x93\xA6";  // 📦
    // Replace text-style U+FE0E with emoji-style U+FE0F
    std::string result = emoji;
    size_t pos = result.find("\xEF\xB8\x8E");
    while (pos != std::string::npos) {
        result.replace(pos, 3, "\xEF\xB8\x8F");
        pos = result.find("\xEF\xB8\x8E", pos + 3);
    }
    return result;
}

// 🎯 对齐 OpenClaw: formatSkillStatus(skill)
static std::string format_skill_status(const SkillStatusEntry& skill) {
    if (skill.eligible) return "\xE2\x9C\x93 ready";      // ✓
    if (skill.disabled) return "\xE2\x8F\xB8 disabled";   // ⏸
    if (skill.blocked_by_allowlist) return "\xF0\x9F\x9A\xAB blocked"; // 🚫
    return "\xE2\x9C\x97 missing";  // ✗
}

// 🎯 对齐 OpenClaw: formatSkillMissingSummary(skill)
static std::string format_skill_missing_summary(const SkillStatusEntry& skill) {
    std::vector<std::string> parts;
    if (!skill.missing_bins.empty()) {
        std::ostringstream ss;
        ss << "bins: ";
        for (size_t i = 0; i < skill.missing_bins.size(); ++i) {
            if (i > 0) ss << ", ";
            ss << skill.missing_bins[i];
        }
        parts.push_back(ss.str());
    }
    if (!skill.missing_any_bins.empty()) {
        std::ostringstream ss;
        ss << "anyBins: ";
        for (size_t i = 0; i < skill.missing_any_bins.size(); ++i) {
            if (i > 0) ss << ", ";
            ss << skill.missing_any_bins[i];
        }
        parts.push_back(ss.str());
    }
    if (!skill.missing_env.empty()) {
        std::ostringstream ss;
        ss << "env: ";
        for (size_t i = 0; i < skill.missing_env.size(); ++i) {
            if (i > 0) ss << ", ";
            ss << skill.missing_env[i];
        }
        parts.push_back(ss.str());
    }
    if (!skill.missing_config.empty()) {
        std::ostringstream ss;
        ss << "config: ";
        for (size_t i = 0; i < skill.missing_config.size(); ++i) {
            if (i > 0) ss << ", ";
            ss << skill.missing_config[i];
        }
        parts.push_back(ss.str());
    }
    if (!skill.missing_os.empty()) {
        std::ostringstream ss;
        ss << "os: ";
        for (size_t i = 0; i < skill.missing_os.size(); ++i) {
            if (i > 0) ss << ", ";
            ss << skill.missing_os[i];
        }
        parts.push_back(ss.str());
    }
    std::ostringstream ss;
    for (size_t i = 0; i < parts.size(); ++i) {
        if (i > 0) ss << "; ";
        ss << parts[i];
    }
    return ss.str();
}

// ============================================================================
// Format functions (对齐 OpenClaw 3 个导出函数)
// ============================================================================

// 🎯 对齐 OpenClaw: formatSkillsList(report, opts)
std::string format_skills_list(const SkillStatusReport& report,
                                const SkillsListOptions& opts) {
    auto skills = report.skills;
    if (opts.eligible_only) {
        skills.erase(
            std::remove_if(skills.begin(), skills.end(),
                [](const SkillStatusEntry& s) { return !s.eligible; }),
            skills.end());
    }

    if (opts.json) {
        // JSON output
        std::ostringstream ss;
        ss << "{\n";
        ss << "  \"workspaceDir\": \"" << report.workspace_dir << "\",\n";
        ss << "  \"skills\": [\n";
        for (size_t i = 0; i < skills.size(); ++i) {
            const auto& s = skills[i];
            ss << "    {\"name\": \"" << s.name << "\", "
               << "\"eligible\": " << (s.eligible ? "true" : "false") << ", "
               << "\"source\": \"" << s.source << "\"}";
            if (i + 1 < skills.size()) ss << ",";
            ss << "\n";
        }
        ss << "  ]\n}\n";
        return ss.str();
    }

    if (skills.empty()) {
        std::string msg = opts.eligible_only
            ? "No eligible skills found. Run `hunterclaw skills list` to see all skills."
            : "No skills found.";
        return msg + "\n\nTip: use clawhub to search, install, and sync skills.";
    }

    int eligible_count = 0;
    for (const auto& s : skills) {
        if (s.eligible) eligible_count++;
    }

    std::ostringstream ss;
    ss << "Skills (" << eligible_count << "/" << skills.size() << " ready)\n";
    ss << std::string(60, '-') << "\n";
    ss << std::left << std::setw(12) << "Status"
       << std::setw(24) << "Skill"
       << std::setw(30) << "Description";
    if (opts.verbose) {
        ss << "Missing";
    }
    ss << "\n" << std::string(60, '-') << "\n";

    for (const auto& skill : skills) {
        std::string emoji = normalize_skill_emoji(skill.emoji);
        ss << std::left << std::setw(12) << format_skill_status(skill)
           << std::setw(24) << (emoji + " " + skill.name)
           << std::setw(30) << skill.description.substr(0, 28);
        if (opts.verbose) {
            ss << format_skill_missing_summary(skill);
        }
        ss << "\n";
    }

    ss << "\nTip: use clawhub to search, install, and sync skills.";
    return ss.str();
}

// 🎯 对齐 OpenClaw: formatSkillInfo(report, skillName, opts)
std::string format_skill_info(const SkillStatusReport& report,
                               const std::string& skill_name,
                               const SkillInfoOptions& opts) {
    const SkillStatusEntry* found = nullptr;
    for (const auto& s : report.skills) {
        if (s.name == skill_name || s.skill_key == skill_name) {
            found = &s;
            break;
        }
    }

    if (!found) {
        if (opts.json) {
            return "{\"error\": \"not found\", \"skill\": \"" + skill_name + "\"}\n";
        }
        return "Skill \"" + skill_name + "\" not found. Run `hunterclaw skills list` to see available skills.\n\nTip: use clawhub to search, install, and sync skills.";
    }

    if (opts.json) {
        std::ostringstream ss;
        ss << "{\n";
        ss << "  \"name\": \"" << found->name << "\",\n";
        ss << "  \"eligible\": " << (found->eligible ? "true" : "false") << ",\n";
        ss << "  \"source\": \"" << found->source << "\",\n";
        ss << "  \"description\": \"" << found->description << "\"\n";
        ss << "}\n";
        return ss.str();
    }

    std::ostringstream ss;
    std::string emoji = normalize_skill_emoji(found->emoji);
    std::string status = found->eligible ? "\xE2\x9C\x93 Ready"
                       : found->disabled ? "\xE2\x8F\xB8 Disabled"
                       : found->blocked_by_allowlist ? "\xF0\x9F\x9A\xAB Blocked by allowlist"
                       : "\xE2\x9C\x97 Missing requirements";

    ss << emoji << " " << found->name << " " << status << "\n\n";
    ss << found->description << "\n\n";
    ss << "Details:\n";
    ss << "  Source: " << found->source << "\n";
    ss << "  Path: " << found->file_path << "\n";
    if (!found->homepage.empty()) {
        ss << "  Homepage: " << found->homepage << "\n";
    }
    if (!found->primary_env.empty()) {
        ss << "  Primary env: " << found->primary_env << "\n";
    }

    // Requirements
    bool has_reqs = !found->required_bins.empty() || !found->required_any_bins.empty() ||
                    !found->required_env.empty() || !found->required_config.empty() ||
                    !found->required_os.empty();
    if (has_reqs) {
        ss << "\nRequirements:\n";
        if (!found->required_bins.empty()) {
            ss << "  Binaries:";
            for (const auto& bin : found->required_bins) {
                bool missing = std::find(found->missing_bins.begin(), found->missing_bins.end(), bin) != found->missing_bins.end();
                ss << " " << (missing ? "\xE2\x9C\x97 " : "\xE2\x9C\x93 ") << bin;
            }
            ss << "\n";
        }
        if (!found->required_env.empty()) {
            ss << "  Environment:";
            for (const auto& env : found->required_env) {
                bool missing = std::find(found->missing_env.begin(), found->missing_env.end(), env) != found->missing_env.end();
                ss << " " << (missing ? "\xE2\x9C\x97 " : "\xE2\x9C\x93 ") << env;
            }
            ss << "\n";
        }
    }

    ss << "\nTip: use clawhub to search, install, and sync skills.";
    return ss.str();
}

// 🎯 对齐 OpenClaw: formatSkillsCheck(report, opts)
std::string format_skills_check(const SkillStatusReport& report,
                                 const SkillsCheckOptions& opts) {
    int eligible = 0, disabled = 0, blocked = 0, missing = 0;
    std::vector<const SkillStatusEntry*> eligible_skills, missing_skills;

    for (const auto& s : report.skills) {
        if (s.eligible) { eligible++; eligible_skills.push_back(&s); }
        else if (s.disabled) { disabled++; }
        else if (s.blocked_by_allowlist) { blocked++; }
        else { missing++; missing_skills.push_back(&s); }
    }

    if (opts.json) {
        std::ostringstream ss;
        ss << "{\n";
        ss << "  \"summary\": {\"total\": " << report.skills.size()
           << ", \"eligible\": " << eligible
           << ", \"disabled\": " << disabled
           << ", \"blocked\": " << blocked
           << ", \"missingRequirements\": " << missing << "},\n";
        ss << "  \"eligible\": [";
        for (size_t i = 0; i < eligible_skills.size(); ++i) {
            if (i > 0) ss << ", ";
            ss << "\"" << eligible_skills[i]->name << "\"";
        }
        ss << "]\n}\n";
        return ss.str();
    }

    std::ostringstream ss;
    ss << "Skills Status Check\n\n";
    ss << "Total: " << report.skills.size() << "\n";
    ss << "\xE2\x9C\x93 Eligible: " << eligible << "\n";
    ss << "\xE2\x8F\xB8 Disabled: " << disabled << "\n";
    ss << "\xF0\x9F\x9A\xAB Blocked by allowlist: " << blocked << "\n";
    ss << "\xE2\x9C\x97 Missing requirements: " << missing << "\n";

    if (!eligible_skills.empty()) {
        ss << "\nReady to use:\n";
        for (const auto* s : eligible_skills) {
            ss << "  " << normalize_skill_emoji(s->emoji) << " " << s->name << "\n";
        }
    }

    if (!missing_skills.empty()) {
        ss << "\nMissing requirements:\n";
        for (const auto* s : missing_skills) {
            ss << "  " << normalize_skill_emoji(s->emoji) << " " << s->name
               << " (" << format_skill_missing_summary(*s) << ")\n";
        }
    }

    ss << "\nTip: use clawhub to search, install, and sync skills.";
    return ss.str();
}

// ============================================================================
// CLI command registration
// 🎯 对齐 OpenClaw: registerSkillsCli(program)
// ============================================================================

void register_skills_cli(CliProgram& program) {
    program.register_command("skills", "List and inspect available skills", [](const CliArgs& args) {
        // Dispatch to subcommand
        if (args.subcommand == "list" || args.subcommand.empty()) {
            // skills list [--json] [--eligible] [-v]
            SkillsListOptions opts;
            opts.json = args.has_flag("json");
            opts.eligible_only = args.has_flag("eligible");
            opts.verbose = args.has_flag("verbose") || args.has_flag("v");

            auto report = build_skills_status_report(args.workspace_dir, args.config);
            std::cout << format_skills_list(report, opts) << std::endl;
        } else if (args.subcommand == "info") {
            // skills info <name> [--json]
            if (args.positional.empty()) {
                std::cerr << "Usage: hunterclaw skills info <name>" << std::endl;
                return 1;
            }
            SkillInfoOptions opts;
            opts.json = args.has_flag("json");

            auto report = build_skills_status_report(args.workspace_dir, args.config);
            std::cout << format_skill_info(report, args.positional[0], opts) << std::endl;
        } else if (args.subcommand == "check") {
            // skills check [--json]
            SkillsCheckOptions opts;
            opts.json = args.has_flag("json");

            auto report = build_skills_status_report(args.workspace_dir, args.config);
            std::cout << format_skills_check(report, opts) << std::endl;
        } else {
            std::cerr << "Unknown subcommand: " << args.subcommand << std::endl;
            return 1;
        }
        return 0;
    });
}

}  // namespace cli
}  // namespace hunterclaw
