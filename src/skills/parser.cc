#include "hunterclaw/skills/skill.h"
#include "hunterclaw/core/types.h"
#include <yaml-cpp/yaml.h>
#include <openssl/sha.h>
#include <regex>
#include <sstream>
#include <fstream>
#include <cctype>
#include <initializer_list>
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

    // 🎯 对齐 OpenClaw sha256HexPrefix: SHA-256 hex 前 len 个字符
    std::string sha256_hex_prefix(const std::string& value, int len) {
        unsigned char hash[SHA256_DIGEST_LENGTH];
        SHA256(reinterpret_cast<const unsigned char*>(value.data()), value.size(), hash);
        static const char* hex = "0123456789abcdef";
        std::string out;
        out.reserve(SHA256_DIGEST_LENGTH * 2);
        for (unsigned char c : hash) {
            out.push_back(hex[(c >> 4) & 0xF]);
            out.push_back(hex[c & 0xF]);
        }
        if (len > 0 && len < static_cast<int>(out.size())) out = out.substr(0, len);
        return out;
    }

    std::string trim_copy(const std::string& s) {
        size_t a = s.find_first_not_of(" \t\r\n");
        if (a == std::string::npos) return "";
        size_t b = s.find_last_not_of(" \t\r\n");
        return s.substr(a, b - a + 1);
    }

    bool contains_any(const std::string& s, std::initializer_list<const char*> subs) {
        for (const char* sub : subs) {
            if (s.find(sub) != std::string::npos) return true;
        }
        return false;
    }

    // 🎯 对齐 OpenClaw normalizeSafeBrewFormula
    // BREW:^[A-Za-z0-9][A-Za-z0-9@+._/-]*$; 拒绝 前导'-'/反斜杠/'..'
    bool is_safe_brew_formula(const std::string& raw) {
        std::string f = trim_copy(raw);
        if (f.empty() || f.front() == '-' || contains_any(f, {"\\", ".."})) return false;
        static const std::regex re(R"(^[A-Za-z0-9][A-Za-z0-9@+._/-]*$)");
        return std::regex_match(f, re);
    }

    // 🎯 对齐 OpenClaw normalizeSafeGoModule
    bool is_safe_go_module(const std::string& raw) {
        std::string m = trim_copy(raw);
        if (m.empty() || m.front() == '-' || contains_any(m, {"\\", "://"})) return false;
        static const std::regex re(
            R"(^[A-Za-z0-9][A-Za-z0-9._~+\-/]*(?:@[A-Za-z0-9][A-Za-z0-9._~+\-/]*)?$)");
        return std::regex_match(m, re);
    }

    // 🎯 对齐 OpenClaw normalizeSafeUvPackage
    bool is_safe_uv_package(const std::string& raw) {
        std::string p = trim_copy(raw);
        if (p.empty() || p.front() == '-' || contains_any(p, {"\\", "://"})) return false;
        static const std::regex re(R"(^[A-Za-z0-9][A-Za-z0-9._\-\[\]=<>!~+,]*$)");
        return std::regex_match(p, re);
    }

    // 🎯 对齐 OpenClaw normalizeSafeNpmSpec (宽松: 非空 + 不以 '-' 开头)
    bool is_safe_npm_spec(const std::string& raw) {
        std::string s = trim_copy(raw);
        return !s.empty() && s.front() != '-';
    }

    // 🎯 对齐 OpenClaw normalizeSafeDownloadUrl: http/https +无空白
    bool is_safe_download_url(const std::string& raw) {
        std::string v = trim_copy(raw);
        if (v.empty()) return false;
        for (char c : v) if (std::isspace(static_cast<unsigned char>(c))) return false;
        return v.rfind("http://", 0) == 0 || v.rfind("https://", 0) == 0;
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
        // 🎯 对齐 OpenClaw computeSkillPromptVersion: 基于完整 SKILL.md 内容
        skill.prompt_version = compute_skill_prompt_version(content);
        
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
                    
                    // 🎯 对齐 OpenClaw parseInstallSpec: 安全净化 + kind 必填校验,
                    // 非法 spec 丢弃 (对齐返回 undefined 被过滤)。
                    if (sanitize_install_spec(spec)) {
                        fm.metadata.install.push_back(spec);
                    } else {
                        log()->warn("[SkillParser] dropped unsafe/invalid install spec (kind check failed)");
                    }
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

// ============================================================================
// ✅ parse_raw_frontmatter (Aligned with OpenClaw parseFrontmatterBlock)
// Returns raw key-value pairs from YAML frontmatter
// ============================================================================

ParsedSkillFrontmatter SkillParser::parse_raw_frontmatter(const std::string& content) {
    ParsedSkillFrontmatter result;

    auto [frontmatter_str, body] = split_frontmatter(content);
    if (frontmatter_str.empty()) {
        return result;
    }

    // Simple line-by-line YAML key: value parsing
    std::istringstream stream(frontmatter_str);
    std::string line;
    while (std::getline(stream, line)) {
        if (line.empty() || line[0] == '#') continue;

        size_t colon = line.find(':');
        if (colon == std::string::npos) continue;

        std::string key = line.substr(0, colon);
        std::string value = line.substr(colon + 1);

        // Trim
        auto trim = [](std::string& s) {
            s.erase(0, s.find_first_not_of(" \t"));
            s.erase(s.find_last_not_of(" \t") + 1);
            // Remove surrounding quotes
            if (s.size() >= 2 && ((s.front() == '"' && s.back() == '"') ||
                                  (s.front() == '\'' && s.back() == '\''))) {
                s = s.substr(1, s.size() - 2);
            }
        };
        trim(key);
        trim(value);

        if (!key.empty()) {
            result[key] = value;
        }
    }
    return result;
}

// ============================================================================
// ✅ resolve_openclaw_metadata (Aligned with OpenClaw resolveOpenClawMetadata)
// ============================================================================

std::optional<OpenClawSkillMetadata> SkillParser::resolve_openclaw_metadata(
    const ParsedSkillFrontmatter& frontmatter
) {
    // Look for openclaw metadata keys in the raw frontmatter
    // OpenClaw frontmatter stores them as nested YAML, but since we have a flat
    // key-value map, we reconstruct from parsed structured fields
    // Fall back to yaml-cpp-based parse_frontmatter for full nested YAML support
    bool has_openclaw = false;
    for (const auto& [key, val] : frontmatter) {
        if (key.find("openclaw") != std::string::npos ||
            key == "always" || key == "emoji" || key == "homepage") {
            has_openclaw = true;
            break;
        }
    }
    if (!has_openclaw) {
        return std::nullopt;
    }

    OpenClawSkillMetadata meta;
    auto it = frontmatter.find("always");
    if (it != frontmatter.end()) {
        meta.always = (it->second == "true" || it->second == "1");
    }
    it = frontmatter.find("emoji");
    if (it != frontmatter.end()) meta.emoji = it->second;
    it = frontmatter.find("homepage");
    if (it != frontmatter.end()) meta.homepage = it->second;
    it = frontmatter.find("skill-key");
    if (it == frontmatter.end()) it = frontmatter.find("skillKey");
    if (it != frontmatter.end()) meta.skill_key = it->second;
    it = frontmatter.find("primary-env");
    if (it == frontmatter.end()) it = frontmatter.find("primaryEnv");
    if (it != frontmatter.end()) meta.primary_env = it->second;

    return meta;
}

// ============================================================================
// ✅ resolve_invocation_policy (Aligned with OpenClaw resolveSkillInvocationPolicy)
// Reads: user-invocable (default: true), disable-model-invocation (default: false)
// ============================================================================

SkillInvocationPolicy SkillParser::resolve_invocation_policy(
    const ParsedSkillFrontmatter& frontmatter
) {
    SkillInvocationPolicy policy;

    auto parse_bool = [](const std::string& val, bool default_val) -> bool {
        if (val == "true" || val == "1" || val == "yes") return true;
        if (val == "false" || val == "0" || val == "no") return false;
        return default_val;
    };

    // user-invocable: default true
    auto it = frontmatter.find("user-invocable");
    if (it != frontmatter.end()) {
        policy.user_invocable = parse_bool(it->second, true);
    }

    // disable-model-invocation: default false
    it = frontmatter.find("disable-model-invocation");
    if (it != frontmatter.end()) {
        policy.disable_model_invocation = parse_bool(it->second, false);
    }

    return policy;
}

// ============================================================================
// 🎯 compute_skill_prompt_version (对齐 OpenClaw computeSkillPromptVersion)
// ============================================================================
std::string SkillParser::compute_skill_prompt_version(const std::string& content) {
    return "sha256:" + sha256_hex_prefix(content, 16);
}

// ============================================================================
// 🎯 sanitize_install_spec (对齐 OpenClaw parseInstallSpec 的安全校验)
// 就地净化各 kind 的关键字段, 非法字段清空; 最后按 kind 校验必填字段。
// 返回 false 表示 spec 应被丢弃 (对齐 openclaw 返回 undefined)。
// ============================================================================
bool SkillParser::sanitize_install_spec(SkillInstallSpec& spec) {
    // formula: brew (formula 或 cask 都走同一校验)
    if (!spec.formula.empty() && !is_safe_brew_formula(spec.formula)) {
        spec.formula.clear();
    }
    // package: node(npm) / uv 各自的校验规则
    if (!spec.package.empty()) {
        if (spec.kind == SkillInstallSpec::Kind::Node) {
            if (!is_safe_npm_spec(spec.package)) spec.package.clear();
        } else if (spec.kind == SkillInstallSpec::Kind::Uv) {
            if (!is_safe_uv_package(spec.package)) spec.package.clear();
        }
    }
    // module: go
    if (!spec.module.empty() && !is_safe_go_module(spec.module)) {
        spec.module.clear();
    }
    // url: download (仅 http/https + 无空白)
    if (!spec.url.empty() && !is_safe_download_url(spec.url)) {
        spec.url.clear();
    }

    // 🎯 kind 必填字段校验 (对齐 openclaw:缺失即return undefined)
    switch (spec.kind) {
        case SkillInstallSpec::Kind::Brew:return !spec.formula.empty();
        case SkillInstallSpec::Kind::Node:     return !spec.package.empty();
        case SkillInstallSpec::Kind::Go:       return !spec.module.empty();
        case SkillInstallSpec::Kind::Uv:       return !spec.package.empty();
        case SkillInstallSpec::Kind::Download: return !spec.url.empty();
    }
    return false;
}

} // namespace skills
} // namespace hunterclaw
