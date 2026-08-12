#include "hunterclaw/gateway/gateway.h"
#include "hunterclaw/gateway/method_guard.h"
#include "hunterclaw/skills/skill.h"
#include "hunterclaw/skills/skill_proposal_service.h"
#include "hunterclaw/skills/skill_proposal_store.h"
#include "hunterclaw/core/utils.h"
#include "skills_install_impl.h"
#include <nlohmann/json.hpp>
#include <filesystem>
#include <set>
#include <algorithm>
#include <sstream>

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
    
    // ✅ 对齐 OpenClaw: 如果提供了 agent_id，验证它存在
    if (!agent_id.empty()) {
        auto cfg = server->config();
        if (cfg) {
            bool found = false;
            for (const auto& agent : cfg->agents) {
                if (agent.id == agent_id) { found = true; break; }
            }
            if (!found) {
                return core::Result<json>::Err("unknown agent id: " + agent_id);
            }
        }
    }
    
    auto skill_mgr = server->skill_manager();
    auto skills = skill_mgr->list_skills();
    
    json skills_array = json::array();
    
    // 🎯 对齐 OpenClaw: buildSkillStatus for each entry
    for (const auto& skill_meta : skills) {
        json skill_info;
        
        // 基本信息
        skill_info["name"] = skill_meta.skill.name;
        // ✅ 对齐 OpenClaw: skillKey 支持 metadata.skillKey 覆盖
        std::string skill_key = skill_meta.metadata.skill_key.empty()
            ? skill_meta.skill.name
            : skill_meta.metadata.skill_key;
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
        
        // ✅ 对齐 OpenClaw: 从 config.skills.entries[skillKey].enabled 读取禁用状态
        bool disabled = false;
        {
            auto skill_mgr_cfg = server->config();
            if (skill_mgr_cfg) {
                const auto& entries = skill_mgr_cfg->skills.entries;
                auto it = entries.find(skill_key);
                if (it != entries.end()) {
                    disabled = !it->second.enabled;
                }
            }
        }
        skill_info["disabled"] = disabled;        
        // 🎯 对齐 OpenClaw: bundled 标志
        bool bundled = (skill_meta.skill.source == skills::SkillSource::Bundled);
        skill_info["bundled"] = bundled;
        
        // ✅ 对齐 OpenClaw: blockedByAllowlist (resolveBundledAllowlist)
        // bundled skills 默认全部允许，除非配置了 allowlist
        bool blocked_by_allowlist = false;
        if (bundled) {
            auto skill_mgr_cfg2 = server->config();
            if (skill_mgr_cfg2 && !skill_mgr_cfg2->skills.allow_bundled.empty()) {
                // 如果配置了 allowlist，检查当前 skill 是否在列表中
                const auto& allowlist = skill_mgr_cfg2->skills.allow_bundled;
                blocked_by_allowlist = std::find(allowlist.begin(), allowlist.end(), skill_key) == allowlist.end();
            }
        }
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

        
        
        // ✅ 对齐 OpenClaw: evaluateEntryRequirementsForCurrentPlatform
        // 检测当前平台的二进制文件、环境变量、OS 兼容性
        bool requirements_satisfied = true;
        {
            // 当前 OS
            std::string current_os =
#if defined(__APPLE__)
                "darwin";
#elif defined(_WIN32)
                "win32";
#else
                "linux";
#endif

            // 检查 bins
            auto check_bin = [](const std::string& bin_name) -> bool {
                const char* path_env = std::getenv("PATH");
                if (!path_env) return false;
                std::istringstream path_stream(path_env);
                std::string path_dir;
                while (std::getline(path_stream, path_dir, ':')) {
                    fs::path full_path = fs::path(path_dir) / bin_name;
                    if (fs::exists(full_path) && !fs::is_directory(full_path)) return true;
                }
                return false;
            };

            // 检查 OS 兼容性
            if (!skill_meta.metadata.os.empty()) {
                bool os_ok = false;
                for (const auto& req_os : skill_meta.metadata.os) {
                    if (req_os == current_os) { os_ok = true; break; }
                }
                if (!os_ok) {
                    missing["os"].push_back(current_os);
                    requirements_satisfied = false;
                }
            }

            // 检查 bins
            for (const auto& bin : skill_meta.metadata.requires.bins) {
                requirements["bins"].push_back(bin);
                if (!check_bin(bin)) {
                    missing["bins"].push_back(bin);
                    requirements_satisfied = false;
                }
            }

            // 检查 any_bins (OR 逻辑)
            if (!skill_meta.metadata.requires.any_bins.empty()) {
                bool any_found = false;
                for (const auto& bin : skill_meta.metadata.requires.any_bins) {
                    requirements["anyBins"].push_back(bin);
                    if (check_bin(bin)) { any_found = true; }
                }
                if (!any_found) {
                    for (const auto& bin : skill_meta.metadata.requires.any_bins) {
                        missing["bins"].push_back(bin + " (any)");
                    }
                    requirements_satisfied = false;
                }
            }

            // 检查环境变量
            for (const auto& env_var : skill_meta.metadata.requires.env) {
                requirements["env"].push_back(env_var);
                if (!std::getenv(env_var.c_str())) {
                    missing["env"].push_back(env_var);
                    requirements_satisfied = false;
                }
            }
        }
        
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
            // ✅ 对齐 OpenClaw: Kind enum 转字符串
            switch (install_spec.kind) {
                case skills::SkillInstallKind::Brew:     spec_json["kind"] = "brew"; break;
                case skills::SkillInstallKind::Node:     spec_json["kind"] = "node"; break;
                case skills::SkillInstallKind::Go:       spec_json["kind"] = "go"; break;
                case skills::SkillInstallKind::Uv:       spec_json["kind"] = "uv"; break;
                case skills::SkillInstallKind::Download: spec_json["kind"] = "download"; break;
                default:                                  spec_json["kind"] = "unknown"; break;
            }
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
    
    // ✅ 对齐 OpenClaw: 调用 install_skill 实现
    std::string workspace_dir;
    {
        const char* home = getenv("HOME");
        workspace_dir = home ? (std::string(home) + "/.hunterclaw") : "./.hunterclaw";
    }
    auto install_result = hunterclaw::skills::install_skill(
        workspace_dir, skill_name, install_id, timeout_ms, json::object()
    );

    json result;
    result["ok"] = install_result.ok;
    result["message"] = install_result.message;
    result["code"] = install_result.code;
    result["stdout"] = install_result.stdout_output;
    result["stderr"] = install_result.stderr_output;
    if (!install_result.warnings.empty()) {
        result["warnings"] = json::array();
        for (const auto& w : install_result.warnings) result["warnings"].push_back(w);
    } else {
        result["warnings"] = json::array();
    }    
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
    
    // ✅ 对齐 OpenClaw: 读取现有配置
    // const cfg = loadConfig(); auto skills = cfg.skills; auto entries = skills.entries;
    json current = json::object();
    {
        auto cfg = server->config();
        if (cfg) {
            const auto& entries = cfg->skills.entries;
            auto it = entries.find(skill_key);
            if (it != entries.end()) {
                current["enabled"] = it->second.enabled;
                // api_key is stored in config["apiKey"]
                auto cfg_it = it->second.config.find("apiKey");
                if (cfg_it != it->second.config.end() && cfg_it->second.is_string()) {
                    current["apiKey"] = cfg_it->second.get<std::string>();
                }
                if (!it->second.env.empty()) {
                    json env_json = json::object();
                    for (const auto& [k, v] : it->second.env) env_json[k] = v;
                    current["env"] = env_json;
                }
            }
        }
    }
    
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
    
    // ✅ 对齐 OpenClaw: 写入配置文件
    // entries[skill_key] = current; skills.entries = entries; cfg.skills = skills; writeConfigFile(cfg);
    {
        auto cfg = server->config();
        if (cfg) {
            auto& entry = cfg->skills.entries[skill_key];
            if (current.contains("enabled")) entry.enabled = current["enabled"].get<bool>();
            // api_key is stored in config["apiKey"]
            if (current.contains("apiKey")) entry.config["apiKey"] = current["apiKey"].get<std::string>();
            if (current.contains("env") && current["env"].is_object()) {
                entry.env.clear();
                for (auto& [k, v] : current["env"].items()) {
                    if (v.is_string()) entry.env[k] = v.get<std::string>();
                }
            }
            // 尝试保存到文件
            std::string config_path = cfg->config_dir + "/config.json";
            try {
                core::save_config(config_path, *cfg);
            } catch (...) {}
        }
    }
    
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
    auto cfg = server->config();

    json result = json::array();
    for (const auto& skill_meta : skills) {
        json skill_json;
        skill_json["name"] = skill_meta.skill.name;
        skill_json["skillKey"] = skill_meta.skill.name;  // 对齐 OpenClaw
        skill_json["description"] = skill_meta.skill.description;
        skill_json["path"] = skill_meta.skill.file_path.string();
        // ✅ 对齐 OpenClaw: 从配置读取 enabled 状态
        bool enabled = true;
        if (cfg) {
            const auto& entries = cfg->skills.entries;
            auto it = entries.find(skill_meta.skill.name);
            if (it != entries.end()) enabled = it->second.enabled;
        }
        skill_json["enabled"] = enabled;
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

//============================================================================
// Skill Proposal (Workshop) Methods
// 🎯 严格对齐 OpenClaw server-methods/skills.ts skills.proposals.*
// ============================================================================

// Helper: 解析 workspace_dir（对齐 resolveSkillsAgentWorkspace 精简）。
static std::string resolve_proposal_workspace(std::shared_ptr<GatewayServer> server) {
    auto cfg = server ? server->config() : nullptr;
    if (cfg && !cfg->config_dir.empty()) return cfg->config_dir;
    const char* home = getenv("HOME");
    return home ? (std::string(home) + "/.hunterclaw") : "./.hunterclaw";
}

// 🎯 对齐 OpenClaw: skills.proposals.list
core::Result<json> method_skills_proposals_list(
    std::shared_ptr<GatewayServer>, std::shared_ptr<MethodGuard>,
    const ClientContext&, const json& params
) {
    std::optional<skills::SkillProposalStatus> filter;
    if (params.contains("status") && params["status"].is_string())
        filter = skills::parse_skill_proposal_status(params["status"].get<std::string>());
    auto res = skills::list_skill_proposals(filter);
    if (res.is_err()) return core::Result<json>::Err(res.error);
    json arr = json::array();
    for (const auto& r : res.get()) arr.push_back(r.to_json());
    json result; result["proposals"] = arr;
    return core::Result<json>::Ok(result);
}

// 🎯 对齐 OpenClaw: skills.proposals.events.list
core::Result<json> method_skills_proposals_events_list(
    std::shared_ptr<GatewayServer>, std::shared_ptr<MethodGuard>,
    const ClientContext&, const json& params
) {
    if (!params.contains("proposalId") || !params["proposalId"].is_string())
        return core::Result<json>::Err("Missing required parameter: proposalId");
    auto res = skills::list_skill_proposal_events(params["proposalId"].get<std::string>());
    if (res.is_err()) return core::Result<json>::Err(res.error);
    json arr = json::array();
    for (const auto& e : res.get()) arr.push_back(e.to_json());
    json result; result["events"] = arr;
    return core::Result<json>::Ok(result);
}

// 🎯 对齐 OpenClaw: skills.proposals.inspect
core::Result<json> method_skills_proposals_inspect(
    std::shared_ptr<GatewayServer>, std::shared_ptr<MethodGuard>,
    const ClientContext&, const json& params
) {
    if (!params.contains("proposalId") || !params["proposalId"].is_string())
        return core::Result<json>::Err("Missing required parameter: proposalId");
    auto res = skills::read_skill_proposal(params["proposalId"].get<std::string>());
    if (res.is_err()) return core::Result<json>::Err(res.error);
    if (!res.get().has_value())
        return core::Result<json>::Err("skill proposal not found: " + params["proposalId"].get<std::string>());
    json result; result["proposal"] = res.get()->to_json();
    // 附带事件历史（对齐 inspect 返回 record + events）。
    auto ev = skills::list_skill_proposal_events(res.get()->id);
    if (ev.is_ok()) {
        json arr = json::array();
        for (const auto& e : ev.get()) arr.push_back(e.to_json());
        result["events"] = arr;
    }
    return core::Result<json>::Ok(result);
}

// 🎯 对齐 OpenClaw: skills.proposals.evaluate（触发 skill_proposal_evaluate hook）
core::Result<json> method_skills_proposals_evaluate(
    std::shared_ptr<GatewayServer> server, std::shared_ptr<MethodGuard>,
    const ClientContext&, const json& params
) {
    if (!params.contains("proposalId") || !params["proposalId"].is_string())
        return core::Result<json>::Err("Missing required parameter: proposalId");
    skills::EvaluateSkillProposalInput input;
    input.proposal_id = params["proposalId"].get<std::string>();
    input.trigger = skills::SkillProposalEvaluationTrigger::Manual;
    input.workspace_dir = resolve_proposal_workspace(server);
    if (params.contains("agentId") && params["agentId"].is_string())
        input.agent_id = params["agentId"].get<std::string>();
    if (params.contains("expectedRevisionHash") && params["expectedRevisionHash"].is_string())
        input.expected_revision_hash = params["expectedRevisionHash"].get<std::string>();
    if (params.contains("correlationId") && params["correlationId"].is_string())
        input.correlation_id = params["correlationId"].get<std::string>();

    auto res = skills::evaluate_skill_proposal(input);
    if (res.is_err()) return core::Result<json>::Err(res.error);
    json result;
    result["proposal"] = res.get().record.to_json();
    result["evaluation"] = res.get().evaluation.to_json();
    return core::Result<json>::Ok(result);
}

// 🎯 对齐 OpenClaw: skills.proposals.action（reject/apply 状态流转）
core::Result<json> method_skills_proposals_action(
    std::shared_ptr<GatewayServer> server, std::shared_ptr<MethodGuard>,
    const ClientContext&, const json& params
) {
    if (!params.contains("proposalId") || !params["proposalId"].is_string())
        return core::Result<json>::Err("Missing required parameter: proposalId");
    if (!params.contains("action") || !params["action"].is_string())
        return core::Result<json>::Err("Missing required parameter: action");
    std::string proposal_id = params["proposalId"].get<std::string>();
    std::string action = params["action"].get<std::string>();
    std::string workspace_dir = resolve_proposal_workspace(server);
    std::optional<std::string> agent_id, correlation_id, reason, expected_revision_hash;
    if (params.contains("agentId") && params["agentId"].is_string())
        agent_id = params["agentId"].get<std::string>();
    if (params.contains("correlationId") && params["correlationId"].is_string())
        correlation_id = params["correlationId"].get<std::string>();
    if (params.contains("reason") && params["reason"].is_string())
        reason = params["reason"].get<std::string>();
    if (params.contains("expectedRevisionHash") && params["expectedRevisionHash"].is_string())
        expected_revision_hash = params["expectedRevisionHash"].get<std::string>();

    core::Result<skills::SkillProposalRecord> res =
        core::Result<skills::SkillProposalRecord>::Err("unknown action: " + action);
    if (action == "reject")
        res = skills::reject_skill_proposal(proposal_id, workspace_dir, agent_id, correlation_id, reason, expected_revision_hash);
    else if (action == "apply")
        res = skills::apply_skill_proposal(proposal_id, workspace_dir, agent_id, correlation_id, reason, expected_revision_hash);
    else
        return core::Result<json>::Err("unsupported action: " + action + " (expected reject|apply)");

    if (res.is_err()) return core::Result<json>::Err(res.error);
    json result; result["proposal"] = res.get().to_json();
    return core::Result<json>::Ok(result);
}

}  // namespace methods
}  // namespace gateway
}  // namespace hunterclaw
