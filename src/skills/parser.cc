#include "hunterclaw/skills/skill.h"
#include "hunterclaw/core/types.h"
#include <yaml-cpp/yaml.h>
#include <regex>
#include <sstream>
#include <fstream>
#include <spdlog/spdlog.h>

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
// SkillParser Implementation
// ============================================================================

core::Result<Skill> SkillParser::parse_file(const fs::path& skill_path) {
    if (!fs::exists(skill_path)) {
        return core::Result<Skill>::Err("Skill file not found: " + skill_path.string());
    }
    
    try {
        // Read file
        std::ifstream file(skill_path);
        std::string content((std::istreambuf_iterator<char>(file)),
                           std::istreambuf_iterator<char>());
        
        // Split frontmatter and body
        auto [frontmatter_str, body] = split_frontmatter(content);
        
        // Parse frontmatter
        auto fm_result = parse_frontmatter(frontmatter_str);
        if (fm_result.is_err()) {
            return core::Result<Skill>::Err(fm_result.error);
        }
        
        // Build Skill object
        Skill skill;
        skill.name = fm_result.get().name;
        skill.description = fm_result.get().description;
        skill.content = body;
        skill.file_path = skill_path;
        skill.base_dir = skill_path.parent_path();
        skill.frontmatter = fm_result.get();
        
        // Determine source (simplified)
        if (skill_path.string().find("/.hunterclaw/") != std::string::npos) {
            skill.source = SkillSource::Managed;
        } else if (skill_path.string().find("/.agents/") != std::string::npos) {
            skill.source = SkillSource::Personal;
        } else {
            skill.source = SkillSource::Project;
        }
        
        return core::Result<Skill>::Ok(skill);
        
    } catch (const std::exception& e) {
        return core::Result<Skill>::Err(
            "Failed to parse skill file: " + std::string(e.what()));
    }
}

core::Result<SkillFrontmatter> SkillParser::parse_frontmatter(const std::string& content) {
    try {
        YAML::Node yaml = YAML::Load(content);
        
        SkillFrontmatter fm;
        
        // Basic fields
        if (yaml["name"]) {
            fm.name = yaml["name"].as<std::string>();
        }
        if (yaml["description"]) {
            fm.description = yaml["description"].as<std::string>();
        }
        
        // OpenClaw metadata
        if (yaml["metadata"] && yaml["metadata"]["openclaw"]) {
            const auto oc = yaml["metadata"]["openclaw"];
            
            if (oc["always"]) {
                fm.metadata.always = oc["always"].as<bool>();
            }
            if (oc["skillKey"]) {
                fm.metadata.skill_key = oc["skillKey"].as<std::string>();
            }
            if (oc["primaryEnv"]) {
                fm.metadata.primary_env = oc["primaryEnv"].as<std::string>();
            }
            if (oc["emoji"]) {
                fm.metadata.emoji = oc["emoji"].as<std::string>();
            }
            if (oc["homepage"]) {
                fm.metadata.homepage = oc["homepage"].as<std::string>();
            }
            if (oc["os"]) {
                for (const auto& os : oc["os"]) {
                    fm.metadata.os.push_back(os.as<std::string>());
                }
            }
            
            // Parse requires
            if (oc["requires"]) {
                const auto req = oc["requires"];
                
                if (req["bins"]) {
                    for (const auto& bin : req["bins"]) {
                        fm.metadata.requires.bins.push_back(bin.as<std::string>());
                    }
                }
                if (req["anyBins"]) {
                    for (const auto& bin : req["anyBins"]) {
                        fm.metadata.requires.any_bins.push_back(bin.as<std::string>());
                    }
                }
                if (req["env"]) {
                    for (const auto& env : req["env"]) {
                        fm.metadata.requires.env.push_back(env.as<std::string>());
                    }
                }
                if (req["config"]) {
                    for (const auto& cfg : req["config"]) {
                        fm.metadata.requires.config.push_back(cfg.as<std::string>());
                    }
                }
            }
            
            // Parse install specs
            if (oc["install"]) {
                for (const auto& spec_node : oc["install"]) {
                    SkillInstallSpec spec;
                    
                    if (spec_node["id"]) {
                        spec.id = spec_node["id"].as<std::string>();
                    }
                    if (spec_node["kind"]) {
                        std::string kind_str = spec_node["kind"].as<std::string>();
                        
                        // Parse kind
                        if (kind_str == "brew") {
                            spec.kind = SkillInstallSpec::Kind::Brew;
                        } else if (kind_str == "node" || kind_str == "npm") {
                            spec.kind = SkillInstallSpec::Kind::Node;
                        } else if (kind_str == "go") {
                            spec.kind = SkillInstallSpec::Kind::Go;
                        } else if (kind_str == "uv") {
                            spec.kind = SkillInstallSpec::Kind::Uv;
                        } else {
                            spec.kind = SkillInstallSpec::Kind::Download;
                        }
                    }
                    
                    if (spec_node["label"]) {
                        spec.label = spec_node["label"].as<std::string>();
                    }
                    if (spec_node["bins"]) {
                        for (const auto& bin : spec_node["bins"]) {
                            spec.bins.push_back(bin.as<std::string>());
                        }
                    }
                    if (spec_node["os"]) {
                        for (const auto& os : spec_node["os"]) {
                            spec.os.push_back(os.as<std::string>());
                        }
                    }
                    if (spec_node["formula"]) {
                        spec.formula = spec_node["formula"].as<std::string>();
                    }
                    if (spec_node["package"]) {
                        spec.package = spec_node["package"].as<std::string>();
                    }
                    if (spec_node["module"]) {
                        spec.module = spec_node["module"].as<std::string>();
                    }
                    if (spec_node["url"]) {
                        spec.url = spec_node["url"].as<std::string>();
                    }
                    if (spec_node["archive"]) {
                        spec.archive = spec_node["archive"].as<std::string>();
                    }
                    if (spec_node["extract"]) {
                        spec.extract = spec_node["extract"].as<bool>();
                    }
                    if (spec_node["stripComponents"]) {
                        spec.strip_components = spec_node["stripComponents"].as<int>();
                    }
                    if (spec_node["targetDir"]) {
                        spec.target_dir = spec_node["targetDir"].as<std::string>();
                    }
                    
                    fm.metadata.install.push_back(spec);
                }
            }
        }
        
        return core::Result<SkillFrontmatter>::Ok(fm);
        
    } catch (const std::exception& e) {
        return core::Result<SkillFrontmatter>::Err(
            "Failed to parse frontmatter: " + std::string(e.what()));
    }
}

core::Result<bool> SkillParser::validate_skill(const Skill& skill) {
    if (skill.name.empty()) {
        return core::Result<bool>::Err("Skill name is required");
    }
    if (skill.description.empty()) {
        return core::Result<bool>::Err("Skill description is required");
    }
    if (skill.content.empty()) {
        return core::Result<bool>::Err("Skill content is required");
    }
    
    return core::Result<bool>::Ok(true);
}

std::pair<std::string, std::string> SkillParser::split_frontmatter(const std::string& content) {
    // Match YAML frontmatter: ---\n...\n---\n
    std::regex frontmatter_regex(R"(^---\s*\n(.*?)\n---\s*\n(.*))", 
                                 std::regex::extended);
    std::smatch match;
    
    if (std::regex_search(content, match, frontmatter_regex)) {
        return {match[1].str(), match[2].str()};
    }
    
    // No frontmatter found
    return {"", content};
}

} // namespace skills
} // namespace hunterclaw
