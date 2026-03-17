// 🎯 严格对齐 OpenClaw: src/agents/skills-install.ts
// HunterClaw Skills 安装执行逻辑 - 头文件

#pragma once

#include <string>
#include <vector>
#include <nlohmann/json.hpp>

namespace hunterclaw {
namespace skills {

// 🎯 对齐 OpenClaw: SkillInstallResult (line 27-34)
struct SkillInstallResult {
    bool ok;
    std::string message;
    std::string stdout_output;
    std::string stderr_output;
    int code;
    std::vector<std::string> warnings;
};

// 🎯 对齐 OpenClaw: installSkill() (line 392-470)
SkillInstallResult install_skill(
    const std::string& workspace_dir,
    const std::string& skill_name,
    const std::string& install_id,
    int timeout_ms,
    const nlohmann::json& config
);

}  // namespace skills
}  // namespace hunterclaw
