#include "hunterclaw/skills/skill.h"
#include "hunterclaw/core/utils.h"
#include <yaml-cpp/yaml.h>
#include <regex>
#include <sstream>

namespace hunterclaw {
namespace skills {

class SkillParserImpl : public SkillParser {
public:
    Result<SkillMetadata> parse(const std::string& skill_md_content) override {
        try {
            // Split YAML frontmatter and markdown content
            auto parts = split_frontmatter(skill_md_content);
            if (parts.first.empty()) {
                return Result<SkillMetadata>::Err("No YAML frontmatter found");
            }
            
            // Parse YAML
            YAML::Node yaml = YAML::Load(parts.first);
            
            SkillMetadata metadata;
            
            // Basic fields
            if (yaml["name"]) {
                metadata.name = yaml["name"].as<std::string>();
            }
            if (yaml["description"]) {
                metadata.description = yaml["description"].as<std::string>();
            }
            if (yaml["version"]) {
                metadata.version = yaml["version"].as<std::string>();
            }
            
            // OpenClaw metadata
            if (yaml["metadata"] && yaml["metadata"]["openclaw"]) {
                auto& oc = yaml["metadata"]["openclaw"];
                
                if (oc["emoji"]) {
                    metadata.openclaw_emoji = oc["emoji"].as<std::string>();
                }
                
                // Parse requires
                if (oc["requires"]) {
                    auto& req = oc["requires"];
                    
                    if (req["os"]) {
                        for (const auto& os : req["os"]) {
                            metadata.requires_os.push_back(os.as<std::string>());
                        }
                    }
                    
                    if (req["bins"]) {
                        for (const auto& bin : req["bins"]) {
                            metadata.requires_bins.push_back(bin.as<std::string>());
                        }
                    }
                    
                    if (req["env"]) {
                        for (const auto& env : req["env"]) {
                            metadata.requires_env.push_back(env.as<std::string>());
                        }
                    }
                    
                    if (req["config"]) {
                        for (const auto& cfg : req["config"]) {
                            metadata.requires_config.push_back(cfg.as<std::string>());
                        }
                    }
                }
                
                // Parse install steps
                if (oc["install"]) {
                    for (const auto& step_node : oc["install"]) {
                        InstallStep step;
                        
                        if (step_node["id"]) {
                            step.id = step_node["id"].as<std::string>();
                        }
                        if (step_node["kind"]) {
                            std::string kind_str = step_node["kind"].as<std::string>();
                            step.kind = parse_install_kind(kind_str);
                        }
                        if (step_node["formula"]) {
                            step.formula = step_node["formula"].as<std::string>();
                        }
                        if (step_node["package"]) {
                            step.package = step_node["package"].as<std::string>();
                        }
                        if (step_node["url"]) {
                            step.url = step_node["url"].as<std::string>();
                        }
                        if (step_node["path"]) {
                            step.path = step_node["path"].as<std::string>();
                        }
                        if (step_node["command"]) {
                            step.command = step_node["command"].as<std::string>();
                        }
                        
                        metadata.install_steps.push_back(step);
                    }
                }
            }
            
            // Parse prompt (markdown content)
            metadata.prompt_md = parts.second;
            
            return Result<SkillMetadata>::Ok(metadata);
            
        } catch (const std::exception& e) {
            return Result<SkillMetadata>::Err(
                std::string("Failed to parse skill: ") + e.what());
        }
    }
    
private:
    std::pair<std::string, std::string> split_frontmatter(const std::string& content) {
        std::regex frontmatter_regex(R"(^---\s*\n(.*?)\n---\s*\n(.*))", 
                                     std::regex::extended | std::regex::dotall);
        std::smatch match;
        
        if (std::regex_search(content, match, frontmatter_regex)) {
            return {match[1].str(), match[2].str()};
        }
        
        return {"", content};
    }
    
    InstallKind parse_install_kind(const std::string& kind_str) {
        if (kind_str == "brew") return InstallKind::Brew;
        if (kind_str == "npm") return InstallKind::Npm;
        if (kind_str == "go") return InstallKind::Go;
        if (kind_str == "download") return InstallKind::Download;
        if (kind_str == "command") return InstallKind::Command;
        return InstallKind::Brew;
    }
};

std::shared_ptr<SkillParser> SkillParser::create() {
    return std::make_shared<SkillParserImpl>();
}

} // namespace skills
} // namespace hunterclaw
