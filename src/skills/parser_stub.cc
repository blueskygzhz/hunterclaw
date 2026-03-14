#include "hunterclaw/skills/skill.h"
#include <spdlog/spdlog.h>
#include <spdlog/sinks/stdout_sinks.h>

namespace hunterclaw::skills {

namespace {

// 获取 Skills logger
auto get_logger() {
    static auto logger = []() {
        auto log = spdlog::get("skills");
        if (!log) {
            log = spdlog::stdout_logger_mt("skills");
        }
        return log;
    }();
    return logger;
}

} // namespace

core::Result<Skill> SkillParser::parse_file(const fs::path&) {
    get_logger()->warn("SkillParser::parse_file not implemented (requires yaml-cpp)");
    
    core::Result<Skill> result;
    result.error = "Not implemented";
    return result;
}

core::Result<SkillFrontmatter> SkillParser::parse_frontmatter(const std::string&) {
    get_logger()->warn("SkillParser::parse_frontmatter not implemented");
    
    core::Result<SkillFrontmatter> result;
    result.error = "Not implemented";
    return result;
}

core::Result<bool> SkillParser::validate_skill(const Skill&) {
    get_logger()->warn("SkillParser::validate_skill not implemented");
    
    core::Result<bool> result;
    result.error = "Not implemented";
    return result;
}

std::pair<std::string, std::string> SkillParser::split_frontmatter(const std::string& content) {
    return {"", content};
}

} // namespace hunterclaw::skills
