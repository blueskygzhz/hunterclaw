// Aligned with openclaw: src/cli/skills-cli.ts (204 lines) + skills-cli.format.ts (354 lines)
// CLI `hunterclaw skills` command: list, info, check, search, install, update.
#include "hunterclaw/skills/skill.h"
#include "hunterclaw/core/utils.h"
#include <spdlog/spdlog.h>
#include <iostream>
#include <iomanip>
#include <algorithm>

namespace hunterclaw {
namespace skills {

namespace {
    auto log() {
        static auto logger = spdlog::get("skills-cli");
        if (!logger) logger = spdlog::stdout_logger_mt("skills-cli");
        return logger;
    }

    // Terminal width (simplified)
    int get_terminal_width() {
        const char* cols = std::getenv("COLUMNS");
        if (cols) {
            int w = std::atoi(cols);
            if (w > 0) return w;
        }
        return 120;
    }

    // 对齐 OC: formatSkillStatus (skills-cli.format.ts:29-40)
    std::string format_status(const SkillStatusEntry& s) {
        if (s.eligible) return "\033[32m✓ ready\033[0m";
        if (s.disabled) return "\033[33m⏸ disabled\033[0m";
        if (s.blocked_by_allowlist) return "\033[33m🚫 blocked\033[0m";
        return "\033[33m△ needs setup\033[0m";
    }

    // 对齐 OC: normalizeSkillEmoji (skills-cli.format.ts:42-43)
    std::string normalize_emoji(const std::string& emoji) {
        return emoji.empty() ? "📦" : emoji;
    }

    // 对齐 OC: formatSkillMissingSummary (skills-cli.format.ts:77-95)
    std::string format_missing_summary(const SkillStatusEntry& s) {
        std::string result;
        if (!s.missing.bins.empty()) {
            result += "bins: ";
            for (size_t i = 0; i < s.missing.bins.size(); i++) {
                if (i > 0) result += ", ";
                result += s.missing.bins[i];
            }
        }
        if (!s.missing.env.empty()) {
            if (!result.empty()) result += "; ";
            result += "env: ";
            for (size_t i = 0; i < s.missing.env.size(); i++) {
                if (i > 0) result += ", ";
                result += s.missing.env[i];
            }
        }
        return result;
    }
}

// ============================================================================
// formatSkillsList (对齐 OC skills-cli.format.ts:97-164)
// ============================================================================

std::string format_skills_list(
    const SkillStatusReport& report,
    bool eligible_only,
    bool verbose
) {
    auto skills = report.skills;
    if (eligible_only) {
        skills.erase(std::remove_if(skills.begin(), skills.end(),
            [](const SkillStatusEntry& s) { return !s.eligible; }), skills.end());
    }

    int eligible_count = std::count_if(report.skills.begin(), report.skills.end(),
        [](const SkillStatusEntry& s) { return s.eligible; });

    std::ostringstream ss;
    ss << "\033[1mSkills\033[0m \033[90m(" << eligible_count << "/" << report.skills.size() << " ready)\033[0m\n\n";

    if (skills.empty()) {
        ss << (eligible_only
            ? "No eligible skills found. Run `hunterclaw skills list` to see all skills."
            : "No skills found.");
        return ss.str();
    }

    // Table header
    ss << std::left
       << std::setw(14) << "Status"
       << std::setw(26) << "Skill"
       << std::setw(40) << "Description"
       << std::setw(16) << "Source";
    if (verbose) ss << std::setw(30) << "Missing";
    ss << "\n";

    // Separator
    int width = verbose ? 126 : 96;
    ss << std::string(std::min(width, get_terminal_width()), '-') << "\n";

    for (const auto& skill : skills) {
        ss << std::left
           << std::setw(14) << format_status(skill)
           << std::setw(26) << (normalize_emoji(skill.emoji) + " " + skill.name)
           << std::setw(40) << (skill.description.size() > 38
                ? skill.description.substr(0, 37) + "…" : skill.description)
           << std::setw(16) << skill.source;
        if (verbose) {
            ss << std::setw(30) << format_missing_summary(skill);
        }
        ss << "\n";
    }

    ss << "\nTip: use `hunterclaw skills search`, `hunterclaw skills install`, "
       << "and `hunterclaw skills update` for ClawHub-backed skills.";
    return ss.str();
}

// ============================================================================
// formatSkillInfo (对齐 OC skills-cli.format.ts:166-290)
// ============================================================================

std::string format_skill_info(const SkillStatusReport& report, const std::string& skill_name) {
    const SkillStatusEntry* skill = nullptr;
    for (const auto& s : report.skills) {
        if (s.name == skill_name || s.skill_key == skill_name) {
            skill = &s;
            break;
        }
    }
    if (!skill) {
        return "Skill \"" + skill_name + "\" not found. Run `hunterclaw skills list` to see available skills.";
    }

    std::ostringstream ss;
    ss << normalize_emoji(skill->emoji) << " \033[1m" << skill->name << "\033[0m "
       << format_status(*skill) << "\n\n"
       << skill->description << "\n\n"
       << "\033[1mDetails:\033[0m\n"
       << "  Source: " << skill->source << "\n"
       << "  Path: " << skill->file_path << "\n";
    if (!skill->homepage.empty()) {
        ss << "  Homepage: " << skill->homepage << "\n";
    }
    if (!skill->primary_env.empty()) {
        ss << "  Primary env: " << skill->primary_env << "\n";
    }

    // Requirements
    bool has_req = !skill->requirements.bins.empty() || !skill->requirements.env.empty() ||
                   !skill->requirements.any_bins.empty() || !skill->requirements.config.empty();
    if (has_req) {
        ss << "\n\033[1mRequirements:\033[0m\n";
        if (!skill->requirements.bins.empty()) {
            ss << "  Binaries: ";
            for (size_t i = 0; i < skill->requirements.bins.size(); i++) {
                if (i > 0) ss << ", ";
                bool missing = std::find(skill->missing.bins.begin(), skill->missing.bins.end(),
                    skill->requirements.bins[i]) != skill->missing.bins.end();
                ss << (missing ? "\033[31m✗ " : "\033[32m✓ ") << skill->requirements.bins[i] << "\033[0m";
            }
            ss << "\n";
        }
        if (!skill->requirements.env.empty()) {
            ss << "  Environment: ";
            for (size_t i = 0; i < skill->requirements.env.size(); i++) {
                if (i > 0) ss << ", ";
                bool missing = std::find(skill->missing.env.begin(), skill->missing.env.end(),
                    skill->requirements.env[i]) != skill->missing.env.end();
                ss << (missing ? "\033[31m✗ " : "\033[32m✓ ") << skill->requirements.env[i] << "\033[0m";
            }
            ss << "\n";
        }
    }

    // Install options
    if (!skill->install.empty() && !skill->eligible) {
        ss << "\n\033[1mInstall options:\033[0m\n";
        for (const auto& inst : skill->install) {
            ss << "  \033[33m→\033[0m " << inst.label << "\n";
        }
    }

    return ss.str();
}

// ============================================================================
// formatSkillsCheck (对齐 OC skills-cli.format.ts:292-353)
// ============================================================================

std::string format_skills_check(const SkillStatusReport& report) {
    int eligible = 0, disabled = 0, blocked = 0, missing = 0;
    for (const auto& s : report.skills) {
        if (s.eligible) eligible++;
        else if (s.disabled) disabled++;
        else if (s.blocked_by_allowlist) blocked++;
        else missing++;
    }

    std::ostringstream ss;
    ss << "\033[1mSkills Status Check\033[0m\n\n"
       << "Total: " << report.skills.size() << "\n"
       << "\033[32m✓\033[0m Eligible: " << eligible << "\n"
       << "\033[33m⏸\033[0m Disabled: " << disabled << "\n"
       << "\033[33m🚫\033[0m Blocked by allowlist: " << blocked << "\n"
       << "\033[31m✗\033[0m Missing requirements: " << missing << "\n";

    if (eligible > 0) {
        ss << "\n\033[1mReady to use:\033[0m\n";
        for (const auto& s : report.skills) {
            if (s.eligible) {
                ss << "  " << normalize_emoji(s.emoji) << " " << s.name << "\n";
            }
        }
    }

    if (missing > 0) {
        ss << "\n\033[1mMissing requirements:\033[0m\n";
        for (const auto& s : report.skills) {
            if (!s.eligible && !s.disabled && !s.blocked_by_allowlist) {
                ss << "  " << normalize_emoji(s.emoji) << " " << s.name
                   << " (" << format_missing_summary(s) << ")\n";
            }
        }
    }

    return ss.str();
}

}  // namespace skills
}  // namespace hunterclaw
