// Aligned with openclaw: src/agents/skills-status.ts (255 lines)
// Builds workspace skill status reports with eligibility, requirements, install options.
#include "hunterclaw/skills/skill.h"
#include "hunterclaw/core/utils.h"
#include <spdlog/spdlog.h>
#include <filesystem>

namespace fs = std::filesystem;

namespace hunterclaw {
namespace skills {

namespace {
    auto log() {
        static auto logger = spdlog::get("skills");
        if (!logger) logger = spdlog::stdout_logger_mt("skills");
        return logger;
    }

    // 对齐 OC: hasBinary (config.ts:20)
    bool has_binary(const std::string& bin) {
        std::string path_env = std::getenv("PATH") ? std::getenv("PATH") : "";
        std::istringstream ss(path_env);
        std::string dir;
        while (std::getline(ss, dir, ':')) {
            if (fs::exists(fs::path(dir) / bin)) return true;
        }
        return false;
    }

    // 对齐 OC: resolveSkillKey (frontmatter.ts:221-223)
    std::string resolve_skill_key(const SkillEntry& entry) {
        return entry.metadata.skill_key.empty() ? entry.skill.name : entry.metadata.skill_key;
    }

    // 对齐 OC: selectPreferredInstallSpec (skills-status.ts:62-104)
    const SkillInstallSpec* select_preferred_install_spec(
        const std::vector<SkillInstallSpec>& install,
        bool prefer_brew
    ) {
        if (install.empty()) return nullptr;

        auto find_kind = [&](SkillInstallSpec::Kind k) -> const SkillInstallSpec* {
            for (const auto& s : install) {
                if (s.kind == k) return &s;
            }
            return nullptr;
        };

        bool brew_available = has_binary("brew");
        auto brew = find_kind(SkillInstallSpec::Kind::Brew);
        auto node = find_kind(SkillInstallSpec::Kind::Node);
        auto go = find_kind(SkillInstallSpec::Kind::Go);
        auto uv = find_kind(SkillInstallSpec::Kind::Uv);
        auto download = find_kind(SkillInstallSpec::Kind::Download);

        // Table-driven preference chain (对齐 OC skills-status.ts:82-94)
        if (prefer_brew && brew_available && brew) return brew;
        if (uv) return uv;
        if (node) return node;
        if (brew_available && brew) return brew;
        if (go) return go;
        if (download) return download;
        if (brew) return brew;
        return &install[0];
    }

    // 对齐 OC: normalizeInstallOptions (skills-status.ts:106-168)
    std::vector<SkillInstallOption> normalize_install_options(
        const SkillEntry& entry, bool prefer_brew
    ) {
        auto& install = entry.metadata.install;
        if (install.empty()) return {};

        // Filter by current OS
        std::string current_os;
        #ifdef __APPLE__
        current_os = "darwin";
        #elif defined(__linux__)
        current_os = "linux";
        #elif defined(_WIN32)
        current_os = "win32";
        #endif

        std::vector<SkillInstallSpec> filtered;
        for (const auto& spec : install) {
            if (spec.os.empty() || std::find(spec.os.begin(), spec.os.end(), current_os) != spec.os.end()) {
                filtered.push_back(spec);
            }
        }
        if (filtered.empty()) return {};

        auto to_option = [](const SkillInstallSpec& spec, int idx) -> SkillInstallOption {
            SkillInstallOption opt;
            opt.id = spec.id.empty() ? (std::string(spec.kind == SkillInstallSpec::Kind::Brew ? "brew" :
                spec.kind == SkillInstallSpec::Kind::Node ? "node" :
                spec.kind == SkillInstallSpec::Kind::Go ? "go" :
                spec.kind == SkillInstallSpec::Kind::Uv ? "uv" : "download") + "-" + std::to_string(idx)) : spec.id;
            opt.kind = spec.kind;
            opt.bins = spec.bins;
            // Build label
            if (!spec.label.empty()) {
                opt.label = spec.label;
            } else if (spec.kind == SkillInstallSpec::Kind::Brew && !spec.formula.empty()) {
                opt.label = "Install " + spec.formula + " (brew)";
            } else if (spec.kind == SkillInstallSpec::Kind::Node && !spec.package.empty()) {
                opt.label = "Install " + spec.package + " (npm)";
            } else if (spec.kind == SkillInstallSpec::Kind::Go && !spec.module.empty()) {
                opt.label = "Install " + spec.module + " (go)";
            } else if (spec.kind == SkillInstallSpec::Kind::Uv && !spec.package.empty()) {
                opt.label = "Install " + spec.package + " (uv)";
            } else {
                opt.label = "Run installer";
            }
            return opt;
        };

        // All downloads → return all
        bool all_download = std::all_of(filtered.begin(), filtered.end(),
            [](const SkillInstallSpec& s) { return s.kind == SkillInstallSpec::Kind::Download; });
        if (all_download) {
            std::vector<SkillInstallOption> opts;
            for (int i = 0; i < static_cast<int>(filtered.size()); i++)
                opts.push_back(to_option(filtered[i], i));
            return opts;
        }

        auto* preferred = select_preferred_install_spec(filtered, prefer_brew);
        if (!preferred) return {};
        int idx = 0;
        for (int i = 0; i < static_cast<int>(filtered.size()); i++) {
            if (&filtered[i] == preferred) { idx = i; break; }
        }
        return {to_option(*preferred, idx)};
    }

    // 对齐 OC: evaluateEntryRequirementsForCurrentPlatform (simplified)
    SkillRequirements compute_missing(const SkillEntry& entry) {
        SkillRequirements missing;
        for (const auto& bin : entry.metadata.requires.bins) {
            if (!has_binary(bin)) missing.bins.push_back(bin);
        }
        if (!entry.metadata.requires.any_bins.empty()) {
            bool has_any = false;
            for (const auto& bin : entry.metadata.requires.any_bins) {
                if (has_binary(bin)) { has_any = true; break; }
            }
            if (!has_any) missing.any_bins = entry.metadata.requires.any_bins;
        }
        for (const auto& env : entry.metadata.requires.env) {
            const char* val = std::getenv(env.c_str());
            if (!val || std::strlen(val) == 0) missing.env.push_back(env);
        }
        // OS check
        #ifdef __APPLE__
        std::string os = "darwin";
        #elif defined(__linux__)
        std::string os = "linux";
        #else
        std::string os = "unknown";
        #endif
        if (!entry.metadata.os.empty() &&
            std::find(entry.metadata.os.begin(), entry.metadata.os.end(), os) == entry.metadata.os.end()) {
            missing.os = entry.metadata.os;  // OS not supported
        }
        return missing;
    }
}

// ============================================================================
// buildWorkspaceSkillStatus (对齐 OC skills-status.ts:228-254)
// ============================================================================

SkillStatusReport build_workspace_skill_status(
    const std::string& workspace_dir,
    std::shared_ptr<core::Config> config,
    const SkillEligibilityContext* eligibility
) {
    SkillStatusReport report;
    report.workspace_dir = workspace_dir;
    report.managed_skills_dir = core::utils::expand_path("~/.hunterclaw/skills");

    // Load skill entries
    auto manager = SkillManager::create(config ? config->skills : core::SkillConfig{});
    manager->load_skills(workspace_dir);
    auto entries = manager->list_skills();

    bool prefer_brew = true;  // Default preference

    for (const auto& entry : entries) {
        SkillStatusEntry status;
        status.name = entry.skill.name;
        status.description = entry.skill.description;
        status.file_path = entry.skill.file_path.string();
        status.base_dir = entry.skill.base_dir.string();
        status.skill_key = resolve_skill_key(entry);
        status.primary_env = entry.metadata.primary_env;
        status.emoji = entry.metadata.emoji;
        status.homepage = entry.metadata.homepage;
        status.always = entry.metadata.always;

        // Source string
        switch (entry.skill.source) {
            case SkillSource::Bundled:   status.source = "openclaw-bundled"; status.bundled = true; break;
            case SkillSource::Managed:   status.source = "openclaw-managed"; break;
            case SkillSource::Personal:  status.source = "agents-skills-personal"; break;
            case SkillSource::Plugin:    status.source = "openclaw-extra"; break;
            case SkillSource::Project:   status.source = "agents-skills-project"; break;
            case SkillSource::Workspace: status.source = "openclaw-workspace"; break;
        }

        // Requirements
        status.requirements = entry.metadata.requires;
        status.missing = compute_missing(entry);

        // Eligible
        bool has_missing = !status.missing.bins.empty() || !status.missing.any_bins.empty() ||
                          !status.missing.env.empty() || !status.missing.config.empty() ||
                          !status.missing.os.empty();
        status.eligible = !status.disabled && !status.blocked_by_allowlist && !has_missing;

        // Install options
        status.install = normalize_install_options(entry, prefer_brew);

        report.skills.push_back(status);
    }

    log()->info("Built skill status report: {} skills ({} eligible)",
                report.skills.size(),
                std::count_if(report.skills.begin(), report.skills.end(),
                    [](const SkillStatusEntry& s) { return s.eligible; }));

    return report;
}

}  // namespace skills
}  // namespace hunterclaw
