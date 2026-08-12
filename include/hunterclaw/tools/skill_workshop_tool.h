#pragma once

// 🎯 对齐 OpenClaw src/agents/tools/skill-workshop-tool.ts
//   封装 SkillProposalService，给 LLM agent 提供提案全生命周期操控：
//   create / update / read / list / inspect / evaluate / apply / reject / quarantine / revise

#include "hunterclaw/tools/tool.h"
#include <optional>
#include <string>

namespace hunterclaw {
namespace tools {

struct SkillWorkshopToolOptions {
    std::string workspace_dir;
    std::string skills_dir;
    std::optional<std::string> agent_id;
    bool proposal_review_completion = false;  // 对齐 OpenClaw proposalOnly 模式
};

std::shared_ptr<Tool> create_skill_workshop_tool(const SkillWorkshopToolOptions& opts);

} // namespace tools
} // namespace hunterclaw
