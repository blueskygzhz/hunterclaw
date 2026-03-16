#include "hunterclaw/gateway/gateway.h"
#include "hunterclaw/gateway/method_guard.h"
#include "hunterclaw/skills/skill.h"
#include "hunterclaw/core/utils.h"
#include <nlohmann/json.hpp>
#include <filesystem>

using json = nlohmann::json;
namespace fs = std::filesystem;

namespace hunterclaw {
namespace gateway {
namespace methods {

// ============================================================================
// Skill Methods - 完全对齐 OpenClaw
// ============================================================================

// skills.list - 列出所有技能 (简化版)
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

// skills.status - 获取技能状态 (对齐 OpenClaw skills.status)
core::Result<json> method_skill_status(
    std::shared_ptr<GatewayServer> server,
    std::shared_ptr<MethodGuard> method_guard,
    const ClientContext& client_ctx,
    const json& params
) {
    std::string agent_id = params.value("agentId", "default");
    
    auto skill_mgr = server->skill_manager();
    auto skills = skill_mgr->list_skills();
    
    json skills_array = json::array();
    
    for (const auto& skill_meta : skills) {
        json skill_info;
        skill_info["name"] = skill_meta.skill.name;
        skill_info["skillKey"] = skill_meta.skill.name;
        skill_info["description"] = skill_meta.skill.description;
        
        // 表情符号 (默认)
        skill_info["emoji"] = "🔧";
        
        // 启用状态
        skill_info["enabled"] = true;  // TODO: 从配置读取
        skill_info["eligible"] = true;  // 是否符合资格
        skill_info["disabled"] = false;
        skill_info["blockedByAllowlist"] = false;
        
        // 缺失依赖
        json missing;
        missing["bins"] = json::array();  // 缺失的二进制文件
        missing["env"] = json::array();   // 缺失的环境变量
        missing["os"] = json::array();    // 不兼容的操作系统
        skill_info["missing"] = missing;
        
        // 依赖需求 (从 skill metadata 解析)
        // json requires;
        // requires["bins"] = json::array();     // 必需的二进制文件
        // requires["anyBins"] = json::array();  // 任意一个二进制文件
        // skill_info["requires"] = requires;
        
        // 主要环境变量
        // skill_info["primaryEnv"] = "API_KEY";
        
        // 安装选项
        json install = json::array();
        // 示例:
        // {
        //   "id": "brew-python3",
        //   "label": "Install Python 3 via Homebrew",
        //   "kind": "brew"
        // }
        skill_info["install"] = install;
        
        // Metadata
        skill_info["metadata"] = json::object();
        
        skills_array.push_back(skill_info);
    }
    
    json result;
    result["skills"] = skills_array;
    
    // 远程技能资格 (可选)
    json eligibility;
    json remote_eligibility;
    remote_eligibility["eligible"] = true;
    // remote_eligibility["reason"] = "...";  // 如果不符合资格
    eligibility["remote"] = remote_eligibility;
    result["eligibility"] = eligibility;
    
    return core::Result<json>::Ok(result);
}

// skills.bins - 列出所有技能需要的二进制文件 (对齐 OpenClaw skills.bins)
core::Result<json> method_skill_bins(
    std::shared_ptr<GatewayServer> server,
    std::shared_ptr<MethodGuard> method_guard,
    const ClientContext& client_ctx,
    const json& params
) {
    auto skill_mgr = server->skill_manager();
    auto skills = skill_mgr->list_skills();
    
    // 收集所有 bins (去重)
    std::set<std::string> bins_set;
    
    for (const auto& skill_meta : skills) {
        // TODO: 从 skill metadata 解析 requires.bins
        // 示例: ["python3", "node", "docker"]
    }
    
    // 转换为数组
    json bins = json::array();
    for (const auto& bin : bins_set) {
        bins.push_back(bin);
    }
    
    json result;
    result["bins"] = bins;
    
    return core::Result<json>::Ok(result);
}

// skills.install - 安装技能依赖 (对齐 OpenClaw skills.install)
core::Result<json> method_skill_install(
    std::shared_ptr<GatewayServer> server,
    std::shared_ptr<MethodGuard> method_guard,
    const ClientContext& client_ctx,
    const json& params
) {
    if (!params.contains("name") || !params.contains("installId")) {
        return core::Result<json>::Err("Missing required parameters: name, installId");
    }
    
    std::string skill_name = params["name"];
    std::string install_id = params["installId"];
    int timeout_ms = params.value("timeoutMs", 60000);
    
    // TODO: 实现安装逻辑
    // 1. 解析 install_id (例如: "brew-python3", "npm-typescript", "binary-ffmpeg")
    // 2. 执行相应的安装命令
    // 3. 捕获 stdout/stderr
    // 4. 超时保护
    
    // 示例返回
    json result;
    result["ok"] = true;
    result["message"] = "Installation successful";
    // result["code"] = 0;
    // result["stdout"] = "...";
    // result["stderr"] = "...";
    // result["warnings"] = json::array();
    
    return core::Result<json>::Ok(result);
}

// skills.update - 更新技能配置 (对齐 OpenClaw skills.update)
core::Result<json> method_skill_update(
    std::shared_ptr<GatewayServer> server,
    std::shared_ptr<MethodGuard> method_guard,
    const ClientContext& client_ctx,
    const json& params
) {
    if (!params.contains("skillKey")) {
        return core::Result<json>::Err("Missing required parameter: skillKey");
    }
    
    std::string skill_key = params["skillKey"];
    
    // 可更新字段
    bool enabled = params.value("enabled", true);  // 启用/禁用
    std::string api_key = params.value("apiKey", "");  // API 密钥
    json env = params.value("env", json::object());    // 环境变量
    
    // TODO: 更新技能配置
    // 1. 读取现有配置
    // 2. 应用更新
    // 3. 写入配置文件
    
    // API 密钥规范化
    // API key validation (TODO: add trim if needed)
    
    // 环境变量处理 (空值删除)
    json normalized_env = json::object();
    for (auto& [key, value] : env.items()) {
        if (value.is_string()) {
            std::string val = value.get<std::string>();
            if (!val.empty()) {
                normalized_env[key] = val;
            }
        } else {
            normalized_env[key] = value;
        }
    }
    
    // 构建结果
    json result;
    result["ok"] = true;
    result["skillKey"] = skill_key;
    
    json config;
    config["enabled"] = enabled;
    if (!api_key.empty()) {
        config["apiKey"] = api_key;
    }
    if (!normalized_env.empty()) {
        config["env"] = normalized_env;
    }
    result["config"] = config;
    
    return core::Result<json>::Ok(result);
}

// skills.get - 获取技能详情 (保留原有)
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
