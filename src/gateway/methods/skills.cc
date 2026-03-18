#include "hunterclaw/gateway/gateway.h"
#include "hunterclaw/gateway/method_guard.h"
#include "hunterclaw/skills/skill.h"
#include "hunterclaw/core/utils.h"
#include <nlohmann/json.hpp>
#include <filesystem>
#include <set>
#include <algorithm>

using json = nlohmann::json;
namespace fs = std::filesystem;

namespace hunterclaw {
namespace gateway {
namespace methods {

// Helper: Convert SkillSource enum to string
static std::string skill_source_to_string(skills::SkillSource source) {
    switch (source) {
        case skills::SkillSource::Bundled:    return "bundled";
        case skills::SkillSource::Managed:    return "managed";
        case skills::SkillSource::Personal:   return "personal";
        case skills::SkillSource::Plugin:     return "plugin";
        case skills::SkillSource::Project:    return "project";
        case skills::SkillSource::Workspace:  return "workspace";
        default:                               return "unknown";
    }
}

// ============================================================================
// Skills Methods - 🎯 严格对齐 OpenClaw (src/gateway/server-methods/skills.ts)
// ============================================================================

// 🎯 对齐 OpenClaw: collectSkillBins - 收集所有技能需要的二进制文件
static std::vector<std::string> collect_skill_bins(const std::vector<skills::SkillEntry>& skills) {
    std::set<std::string> bins_set;
    
    auto trim = [](std::string s) {
        s.erase(0, s.find_first_not_of(" \t\n\r"));
        s.erase(s.find_last_not_of(" \t\n\r") + 1);
        return s;
    };
    
    for (const auto& skill_entry : skills) {
        const auto& metadata = skill_entry.metadata;
        
        // requires.bins
        for (const auto& bin : metadata.requires.bins) {
            std::string bin_str = trim(bin);
            if (!bin_str.empty()) {
                bins_set.insert(bin_str);
            }
        }
        
        // requires.any_bins
        for (const auto& bin : metadata.requires.any_bins) {
            std::string bin_str = trim(bin);
            if (!bin_str.empty()) {
                bins_set.insert(bin_str);
            }
        }
        
        // install[].bins
        for (const auto& install_spec : metadata.install) {
            for (const auto& bin : install_spec.bins) {
                std::string bin_str = trim(bin);
                if (!bin_str.empty()) {
                    bins_set.insert(bin_str);
                }
            }
        }
    }
    
    // 排序并返回
    std::vector<std::string> result(bins_set.begin(), bins_set.end());
    std::sort(result.begin(), result.end());
    return result;
}

// 🎯 对齐 OpenClaw: skills.status - 获取技能状态、资格、缺失依赖
core::Result<json> method_skill_status(
    std::shared_ptr<GatewayServer> server,
    std::shared_ptr<MethodGuard> method_guard,
    const ClientContext& client_ctx,
    const json& params
) {
    // 参数验证 (对齐 OpenClaw validateSkillsStatusParams)
    std::string agent_id = params.value("agentId", "");
    
    // TODO: 如果提供了 agent_id，验证它存在
    // const auto& known_agents = config->list_agent_ids();
    // if (!agent_id.empty() && !contains(known_agents, agent_id)) {
    //     return core::Result<json>::Err("unknown agent id: " + agent_id);
    // }
    
    auto skill_mgr = server->skill_manager();
    auto skills = skill_mgr->list_skills();
    
    json skills_array = json::array();
    
    // 🎯 对齐 OpenClaw: buildSkillStatus for each entry
    for (const auto& skill_meta : skills) {
        json skill_info;
        
        // 基本信息
        skill_info["name"] = skill_meta.skill.name;
        std::string skill_key = skill_meta.skill.name;  // TODO: 支持 metadata.skillKey 覆盖
        skill_info["skillKey"] = skill_key;
        skill_info["description"] = skill_meta.skill.description;
        skill_info["source"] = skill_source_to_string(skill_meta.skill.source);
        skill_info["filePath"] = skill_meta.skill.file_path.string();
        skill_info["baseDir"] = skill_meta.skill.file_path.parent_path().string();
        
        // 🎯 对齐 OpenClaw: emoji (默认 🔧 或从 metadata 读取)
        std::string emoji = skill_meta.metadata.emoji.empty() ? "🔧" : skill_meta.metadata.emoji;
        skill_info["emoji"] = emoji;
        
        // 🎯 对齐 OpenClaw: homepage
        if (!skill_meta.metadata.homepage.empty()) {
            skill_info["homepage"] = skill_meta.metadata.homepage;
        }
        
        // 🎯 对齐 OpenClaw: always flag
        bool always = skill_meta.metadata.always;
        skill_info["always"] = always;
        
        // 🎯 对齐 OpenClaw: primaryEnv
        if (!skill_meta.metadata.primary_env.empty()) {
            skill_info["primaryEnv"] = skill_meta.metadata.primary_env;
        }
        
        // 🎯 对齐 OpenClaw: 启用状态 (从配置读取)
        bool disabled = false;  // TODO: 从 config.skills.entries[skillKey].enabled 读取
        skill_info["disabled"] = disabled;
        
        // 🎯 对齐 OpenClaw: bundled 标志
        bool bundled = (skill_meta.skill.source == skills::SkillSource::Bundled);
        skill_info["bundled"] = bundled;
        
        // 🎯 对齐 OpenClaw: blockedByAllowlist
        bool blocked_by_allowlist = false;  // TODO: 实现 resolveBundledAllowlist 逻辑
        skill_info["blockedByAllowlist"] = blocked_by_allowlist;
        
        // 🎯 对齐 OpenClaw: requirements 和 missing (evaluateEntryRequirementsForCurrentPlatform)
        json requirements;
        requirements["bins"] = json::array();
        requirements["anyBins"] = json::array();
        requirements["env"] = json::array();
        requirements["os"] = json::array();
        requirements["config"] = json::array();
        
        json missing;
        missing["bins"] = json::array();  // 缺失的二进制文件
        missing["env"] = json::array();   // 缺失的环境变量
        missing["os"] = json::array();    // 不兼容的操作系统
        
        // TODO: 实际检测缺失的依赖
        // hasBinary() 检查二进制文件是否存在
        // 检查环境变量: getenv()/config->skills.entries[skillKey].env
        // 检查操作系统: process.platform vs metadata.openclaw.os
        
        bool requirements_satisfied = true;  // TODO: 实际计算
        
        skill_info["requirements"] = requirements;
        skill_info["missing"] = missing;
        
        // 🎯 对齐 OpenClaw: eligible (综合判断)
        bool eligible = !disabled && !blocked_by_allowlist && requirements_satisfied;
        skill_info["eligible"] = eligible;
        
        // 🎯 对齐 OpenClaw: configChecks (配置路径检查)
        json config_checks = json::array();
        // 示例:
        // {
        //   "path": "providers.anthropic.apiKey",
        //   "satisfied": true,
        //   "label": "Anthropic API Key"
        // }
        skill_info["configChecks"] = config_checks;
        
        // 🎯 对齐 OpenClaw: install options (normalizeInstallOptions)
        json install = json::array();
        
        // 从 metadata.install 解析
        for (const auto& install_spec : skill_meta.metadata.install) {
            json spec_json;
            spec_json["id"] = install_spec.id;
            // TODO: 将 Kind enum 转换为字符串
            spec_json["label"] = install_spec.label;
            spec_json["bins"] = install_spec.bins;
            if (!install_spec.formula.empty()) {
                spec_json["formula"] = install_spec.formula;
            }
            install.push_back(spec_json);
        }
        skill_info["install"] = install;
        
        // Metadata - 结构化字段已经单独导出,不需要原始metadata
        
        skills_array.push_back(skill_info);
    }
    
    json result;
    result["skills"] = skills_array;
    
    // 🎯 对齐 OpenClaw: eligibility.remote (远程技能资格)
    json eligibility;
    json remote_eligibility;
    remote_eligibility["eligible"] = true;
    // remote_eligibility["reason"] = "...";  // 如果不符合资格
    eligibility["remote"] = remote_eligibility;
    result["eligibility"] = eligibility;
    
    // 🎯 对齐 OpenClaw: workspaceDir 和 managedSkillsDir
    // result["workspaceDir"] = "...";
    // result["managedSkillsDir"] = "...";
    
    return core::Result<json>::Ok(result);
}

// 🎯 对齐 OpenClaw: skills.bins - 列出所有技能需要的二进制文件
core::Result<json> method_skill_bins(
    std::shared_ptr<GatewayServer> server,
    std::shared_ptr<MethodGuard> method_guard,
    const ClientContext& client_ctx,
    const json& params
) {
    // 参数验证 (对齐 OpenClaw validateSkillsBinsParams)
    // OpenClaw 的 params 类型: {}
    
    // 🎯 对齐 OpenClaw: 收集所有 workspace 的 bins
    auto skill_mgr = server->skill_manager();
    auto skills = skill_mgr->list_skills();
    
    auto bins = collect_skill_bins(skills);
    
    json result;
    result["bins"] = bins;
    
    return core::Result<json>::Ok(result);
}

// 🎯 对齐 OpenClaw: skills.install - 安装技能依赖
core::Result<json> method_skill_install(
    std::shared_ptr<GatewayServer> server,
    std::shared_ptr<MethodGuard> method_guard,
    const ClientContext& client_ctx,
    const json& params
) {
    // 🎯 对齐 OpenClaw: 参数验证 (validateSkillsInstallParams)
    if (!params.contains("name") || !params.contains("installId")) {
        return core::Result<json>::Err("Missing required parameters: name, installId");
    }
    
    std::string skill_name = params["name"];
    std::string install_id = params["installId"];
    int timeout_ms = params.value("timeoutMs", 60000);
    
    // 🎯 对齐 OpenClaw: installSkill (src/agents/skills-install.ts line 392-470)
    // 1. 查找技能和 install spec
    // 2. 根据 kind 执行相应的安装命令:
    //    - "brew": brew install <formula>
    //    - "node": npm/pnpm/yarn/bun install -g <package>
    //    - "go": go install <module>@latest
    //    - "uv": uv tool install <package>
    //    - "download": 下载 URL, 解压, 移动到 targetDir
    // 3. 捕获 stdout/stderr
    // 4. 超时保护
    
    // 🎯 调用实现 (skills_install_impl.cc)
    // TODO: 需要包含头文件并链接实现
    // #include "skills_install_impl.cc"
    // auto install_result = hunterclaw::skills::install_skill(
    //     workspace_dir, name, install_id, timeout_ms, config
    // );
    
    // 🎯 临时返回 (待集成 install_skill 实现)
    json result;
    result["ok"] = false;
    result["message"] = "Installation implementation in skills_install_impl.cc - "
                        "requires integration with skills manager";
    result["code"] = -1;
    result["stdout"] = "";
    result["stderr"] = "See skills_install_impl.cc for complete implementation:\n"
                      "- brew install (line 247-254)\n"
                      "- node install (line 101-112, 221-238)\n"
                      "- go install (line 338-367, 445-505)\n"
                      "- uv install (line 256-283)\n"
                      "- download install (see skills-install-download.ts)";
    result["warnings"] = json::array();
    
    // 🎯 集成后的完整逻辑:
    // if (install_result.ok) {
    //     result["ok"] = true;
    //     result["message"] = install_result.message;
    //     result["code"] = install_result.code;
    //     result["stdout"] = install_result.stdout_output;
    //     result["stderr"] = install_result.stderr_output;
    //     if (!install_result.warnings.empty()) {
    //         result["warnings"] = json::array();
    //         for (const auto& warning : install_result.warnings) {
    //             result["warnings"].push_back(warning);
    //         }
    //     }
    // } else {
    //     result["ok"] = false;
    //     result["message"] = install_result.message;
    //     result["code"] = install_result.code;
    //     result["stdout"] = install_result.stdout_output;
    //     result["stderr"] = install_result.stderr_output;
    // }
    
    return core::Result<json>::Ok(result);
}

// 🎯 对齐 OpenClaw: skills.update - 更新技能配置
core::Result<json> method_skill_update(
    std::shared_ptr<GatewayServer> server,
    std::shared_ptr<MethodGuard> method_guard,
    const ClientContext& client_ctx,
    const json& params
) {
    // 🎯 对齐 OpenClaw: 参数验证 (validateSkillsUpdateParams)
    if (!params.contains("skillKey")) {
        return core::Result<json>::Err("Missing required parameter: skillKey");
    }
    
    std::string skill_key = params["skillKey"];
    
    // 🎯 对齐 OpenClaw: 可更新字段
    std::optional<bool> enabled;
    if (params.contains("enabled") && params["enabled"].is_boolean()) {
        enabled = params["enabled"].get<bool>();
    }
    
    std::optional<std::string> api_key;
    if (params.contains("apiKey") && params["apiKey"].is_string()) {
        api_key = params["apiKey"].get<std::string>();
    }
    
    json env;
    if (params.contains("env") && params["env"].is_object()) {
        env = params["env"];
    }
    
    // 🎯 对齐 OpenClaw: 读取现有配置
    // const cfg = loadConfig();
    // auto skills = cfg.skills ? cfg.skills : {};
    // auto entries = skills.entries ? skills.entries : {};
    // auto current = entries[skill_key] ? entries[skill_key] : {};
    
    json current = json::object();  // TODO: 从 config 读取
    
    // 🎯 对齐 OpenClaw: 应用更新
    if (enabled.has_value()) {
        current["enabled"] = enabled.value();
    }
    
    // 🎯 对齐 OpenClaw: API 密钥规范化 (normalizeSecretInput)
    if (api_key.has_value()) {
        std::string trimmed = api_key.value();
        // trim
        trimmed.erase(0, trimmed.find_first_not_of(" \t\n\r"));
        trimmed.erase(trimmed.find_last_not_of(" \t\n\r") + 1);
        
        if (!trimmed.empty()) {
            current["apiKey"] = trimmed;
        } else {
            current.erase("apiKey");  // 删除空值
        }
    }
    
    // 🎯 对齐 OpenClaw: 环境变量处理 (空值删除)
    if (!env.empty()) {
        json next_env = current.contains("env") ? current["env"] : json::object();
        
        for (auto& [key, value] : env.items()) {
            std::string trimmed_key = key;
            trimmed_key.erase(0, trimmed_key.find_first_not_of(" \t\n\r"));
            trimmed_key.erase(trimmed_key.find_last_not_of(" \t\n\r") + 1);
            
            if (trimmed_key.empty()) {
                continue;
            }
            
            if (value.is_string()) {
                std::string trimmed_val = value.get<std::string>();
                trimmed_val.erase(0, trimmed_val.find_first_not_of(" \t\n\r"));
                trimmed_val.erase(trimmed_val.find_last_not_of(" \t\n\r") + 1);
                
                if (trimmed_val.empty()) {
                    next_env.erase(trimmed_key);  // 删除空值
                } else {
                    next_env[trimmed_key] = trimmed_val;
                }
            } else {
                next_env[trimmed_key] = value;
            }
        }
        
        current["env"] = next_env;
    }
    
    // 🎯 对齐 OpenClaw: 写入配置文件
    // entries[skill_key] = current;
    // skills.entries = entries;
    // cfg.skills = skills;
    // await writeConfigFile(cfg);
    
    // TODO: 实际写入 config
    
    // 🎯 对齐 OpenClaw: 返回格式
    json result;
    result["ok"] = true;
    result["skillKey"] = skill_key;
    result["config"] = current;
    
    return core::Result<json>::Ok(result);
}

// 🎯 保留 HunterClaw 原有的 skills.list 方法 (简化版)
core::Result<json> method_skill_list(
    std::shared_ptr<GatewayServer> server,
    std::shared_ptr<MethodGuard> method_guard,
    const ClientContext& client_ctx,
    const json& params
) {
    auto skill_mgr = server->skill_manager();
    auto skills = skill_mgr->list_skills();

    json result = json::array();
    for (const auto& skill_meta : skills) {
        json skill_json;
        skill_json["name"] = skill_meta.skill.name;
        skill_json["skillKey"] = skill_meta.skill.name;  // 对齐 OpenClaw
        skill_json["description"] = skill_meta.skill.description;
        skill_json["path"] = skill_meta.skill.file_path.string();
        skill_json["enabled"] = true;  // TODO: 从配置读取
        result.push_back(skill_json);
    }

    return core::Result<json>::Ok(result);
}

// 🎯 保留 HunterClaw 原有的 skills.get 方法
core::Result<json> method_skill_get(
    std::shared_ptr<GatewayServer> server,
    std::shared_ptr<MethodGuard> method_guard,
    const ClientContext& client_ctx,
    const json& params
) {
    if (!params.contains("name")) {
        return core::Result<json>::Err("Missing name");
    }

    std::string name = params["name"];
    auto skill_mgr = server->skill_manager();
    auto skill_opt = skill_mgr->get_skill(name);

    if (!skill_opt.has_value()) {
        return core::Result<json>::Err("Skill not found: " + name);
    }

    const auto& skill_meta = skill_opt.value();
    json result;
    result["name"] = skill_meta.skill.name;
    result["skillKey"] = skill_meta.skill.name;
    result["description"] = skill_meta.skill.description;
    result["path"] = skill_meta.skill.file_path.string();
    result["content"] = skill_meta.skill.content;

    return core::Result<json>::Ok(result);
}

}  // namespace methods
}  // namespace gateway
}  // namespace hunterclaw
