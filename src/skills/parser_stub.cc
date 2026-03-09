#include "hunterclaw/skills/skill.h"
#include <spdlog/spdlog.h>
#include <spdlog/sinks/stdout_sinks.h>

namespace hunterclaw::skills {

core::Result<Skill> SkillParser::parse_file(const fs::path&) {
    static auto logger = spdlog::get("skills");
    if (!logger) {
        logger = spdlog::stdout_logger_mt("skills");
    }
    logger->warn("SkillParser::parse_file not implemented (requires yaml-cpp)");
    
    core::Result<Skill> result;
    result.error = "Not implemented";
    return result;
}

core::Result<SkillFrontmatter> SkillParser::parse_frontmatter(const std::string&) {
    static auto logger = spdlog::get("skills");
    if (!logger) {
        logger = spdlog::stdout_logger_mt("skills");
    }
    logger->warn("SkillParser::parse_frontmatter not implemented");
    
    core::Result<SkillFrontmatter> result;
    result.error = "Not implemented";
    return result;
}

core::Result<bool> SkillParser::validate_skill(const Skill&) {
    static auto logger = spdlog::get("skills");
    if (!logger) {
        logger = spdlog::stdout_logger_mt("skills");
    }
    logger->warn("SkillParser::validate_skill not implemented");
    
    core::Result<bool> result;
    result.error = "Not implemented";
    return result;
}

std::pair<std::string, std::string> SkillParser::split_frontmatter(const std::string& content) {
    return {"", content};
}

} // namespace hunterclaw::skills
