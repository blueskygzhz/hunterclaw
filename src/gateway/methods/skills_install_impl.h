#pragma once
// \u2705 \u5bf9\u9f50 OpenClaw: src/agents/skills-install.ts
// HunterClaw Skills \u5b89\u88c5\u6267\u884c\u903b\u8f91\u5934\u6587\u4ef6

#include <string>
#include <vector>
#include <nlohmann/json.hpp>

namespace hunterclaw {
namespace skills {

using json = nlohmann::json;

// \u2705 \u5bf9\u9f50 OpenClaw: SkillInstallResult (line 27-34)
struct SkillInstallResult {
    bool ok;
    std::string message;
    std::string stdout_output;
    std::string stderr_output;
    int code;  // \u8fdb\u7a0b\u9000\u51fa\u7801\uff0c-1 \u8868\u793a\u6267\u884c\u5931\u8d25
    std::vector<std::string> warnings;
};

// \u2705 \u5bf9\u9f50 OpenClaw: installSkill() (line 392-470)
SkillInstallResult install_skill(
    const std::string& workspace_dir,
    const std::string& skill_name,
    const std::string& install_id,
    int timeout_ms,
    const json& config
);

} // namespace skills
} // namespace hunterclaw
