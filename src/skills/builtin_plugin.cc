#include "hunterclaw/skills/builtin_plugin.h"
#include "hunterclaw/skills/skill.h"
#include "hunterclaw/core/utils.h"
#include <spdlog/spdlog.h>
#include <fstream>

namespace hunterclaw {
namespace skills {

namespace {
    auto log() {
        static auto logger = spdlog::get("skills");
        if (!logger) {
            logger = spdlog::stdout_logger_mt("skills");
        }
        return logger;
    }
}

// ============================================================================
// BuiltinSkillsPlugin::Impl
// ============================================================================

struct BuiltinSkillsPlugin::Impl {
    std::map<std::string, Skill> skills_;
    mutable std::shared_mutex mutex_;
    
    std::vector<fs::path> skill_dirs_;
    
    void add_skill(const Skill& skill) {
        std::unique_lock lock(mutex_);
        skills_[skill.name] = skill;
    }
    
    void clear_skills() {
        std::unique_lock lock(mutex_);
        skills_.clear();
    }
};

// ============================================================================
// BuiltinSkillsPlugin 实现
// ============================================================================

BuiltinSkillsPlugin::BuiltinSkillsPlugin()
    : impl_(std::make_unique<Impl>()) {}

BuiltinSkillsPlugin::~BuiltinSkillsPlugin() = default;

core::Result<bool> BuiltinSkillsPlugin::init(
    const std::map<std::string, std::string>& config
) {
    log()->info("Initializing Builtin Skills Plugin...");
    
    // 1. ~/.hunterclaw/skills-bundled/ (Bundled)
    auto bundled_dir = core::utils::expand_path("~/.hunterclaw/skills-bundled");
    if (fs::exists(bundled_dir)) {
        impl_->skill_dirs_.push_back(bundled_dir);
        auto bundled_skills = load_skills_from_dir(bundled_dir, SkillSource::Bundled);
        for (const auto& skill : bundled_skills) {
            impl_->add_skill(skill);
        }
    }
    
    // 2. ~/.openclaw/skills/ (Managed - OpenClaw 兼容)
    auto openclaw_dir = core::utils::expand_path("~/.openclaw/skills");
    if (fs::exists(openclaw_dir)) {
        impl_->skill_dirs_.push_back(openclaw_dir);
        auto openclaw_skills = load_skills_from_dir(openclaw_dir, SkillSource::Managed);
        for (const auto& skill : openclaw_skills) {
            impl_->add_skill(skill);
        }
    }
    
    // 3. ~/.hunterclaw/skills/ (Managed)
    auto managed_dir = core::utils::expand_path("~/.hunterclaw/skills");
    if (fs::exists(managed_dir)) {
        impl_->skill_dirs_.push_back(managed_dir);
        auto managed_skills = load_skills_from_dir(managed_dir, SkillSource::Managed);
        for (const auto& skill : managed_skills) {
            impl_->add_skill(skill);
        }
    }
    
    log()->info("Loaded {} builtin skill(s)", impl_->skills_.size());
    return core::Result<bool>::Ok(true);
}

void BuiltinSkillsPlugin::shutdown() {
    log()->info("Shutting down Builtin Skills Plugin");
    impl_->clear_skills();
}

std::vector<Skill> BuiltinSkillsPlugin::list_skills() {
    std::shared_lock lock(impl_->mutex_);
    
    std::vector<Skill> result;
    result.reserve(impl_->skills_.size());
    
    for (const auto& [name, skill] : impl_->skills_) {
        result.push_back(skill);
    }
    
    return result;
}

std::optional<Skill> BuiltinSkillsPlugin::get_skill(const std::string& name) {
    std::shared_lock lock(impl_->mutex_);
    
    auto it = impl_->skills_.find(name);
    if (it != impl_->skills_.end()) {
        return it->second;
    }
    
    return std::nullopt;
}

core::Result<bool> BuiltinSkillsPlugin::refresh_skills() {
    log()->info("Refreshing builtin skills...");
    
    impl_->clear_skills();
    
    // 重新加载所有 Skill 目录
    for (const auto& dir : impl_->skill_dirs_) {
        SkillSource source = SkillSource::Managed;
        
        if (dir.string().find("skills-bundled") != std::string::npos) {
            source = SkillSource::Bundled;
        }
        
        auto skills = load_skills_from_dir(dir, source);
        for (const auto& skill : skills) {
            impl_->add_skill(skill);
        }
    }
    
    log()->info("Refreshed {} builtin skill(s)", impl_->skills_.size());
    return core::Result<bool>::Ok(true);
}

std::vector<Skill> BuiltinSkillsPlugin::load_skills_from_dir(
    const fs::path& skill_dir,
    SkillSource source
) {
    std::vector<Skill> skills;
    
    if (!fs::exists(skill_dir) || !fs::is_directory(skill_dir)) {
        return skills;
    }
    
    log()->debug("Loading skills from: {}", skill_dir.string());
    
    try {
        // 遍历 Skill 目录
        for (const auto& entry : fs::directory_iterator(skill_dir)) {
            if (!entry.is_directory()) {
                continue;
            }
            
            // 查找 SKILL.md
            fs::path skill_file = entry.path() / "SKILL.md";
            if (!fs::exists(skill_file)) {
                continue;
            }
            
            // 解析 SKILL.md
            auto result = SkillParser::parse_file(skill_file);
            if (result.is_err()) {
                log()->warn("Failed to parse skill {}: {}", 
                           skill_file.string(), result.error);
                continue;
            }
            
            Skill skill = result.get();
            skill.source = source;
            skill.base_dir = entry.path();
            
            skills.push_back(skill);
            log()->debug("Loaded skill: {} from {}", skill.name, skill_file.string());
        }
    } catch (const std::exception& e) {
        log()->error("Error loading skills: {}", e.what());
    }
    
    return skills;
}

// ============================================================================
// C ABI 导出函数
// ============================================================================

extern "C" {

SkillPlugin* create_skill_plugin() {
    return new BuiltinSkillsPlugin();
}

void destroy_skill_plugin(SkillPlugin* plugin) {
    delete plugin;
}

} // extern "C"

} // namespace skills
} // namespace hunterclaw
