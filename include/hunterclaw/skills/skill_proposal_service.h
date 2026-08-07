#pragma once

// 🎯 对齐 OpenClaw skills/workshop/service.ts + service-evaluation.ts + plugin-hooks.ts
//   Skill Proposal 生命周期主服务 + evaluate 流水线。核心职责：驱动状态机、
//   触发 skill_proposal_evaluate（收集 evaluator outcomes）与 skill_proposal_changed
//   （void 观测）两个 plugin hook。

#include "hunterclaw/core/result.h"
#include "hunterclaw/skills/skill_proposal_types.h"
#include <optional>
#include <string>

namespace hunterclaw {
namespace skills {

// 创建 proposal 输入（对齐 SkillProposalCreateInput 精简）。
struct CreateSkillProposalInput {
    SkillProposalKind kind = SkillProposalKind::Create;
    std::string skill_name;
    std::string skill_key;
    std::string skill_file;
    std::string skill_dir;
    std::optional<std::string> source;
    std::string description;
    std::string content;            // 草稿正文（skill.md）
    std::string proposed_version;   // 缺省 "1"
    SkillProposalSource created_by = SkillProposalSource::SkillWorkshop;
    std::string workspace_dir;
    std::optional<std::string> agent_id;
};

// 创建 proposal：持久化 + created 事件 + dispatch skill_proposal_changed(created)。
core::Result<SkillProposalRecord> create_skill_proposal(const CreateSkillProposalInput& input);

// 评估 proposal（对齐 evaluateSkillProposal）：
//   1. 读 pending proposal +乐观并发快照；
//   2. 若有 evaluator（has_hooks(skill_proposal_evaluate)）→ 构造 evaluate event →
//      run_skill_proposal_evaluate 收集 outcomes → 归一化；
//   3. 持久化 evaluation + evaluation_completed 事件（乐观并发校验）；
//   4. dispatch skill_proposal_changed(evaluation_completed, evaluations)。
struct EvaluateSkillProposalInput {
    std::string proposal_id;
    SkillProposalEvaluationTrigger trigger = SkillProposalEvaluationTrigger::Manual;
    std::optional<std::string> expected_revision_hash;
    std::optional<std::string> correlation_id;
    std::string workspace_dir;
    std::optional<std::string> agent_id;
};
struct EvaluateSkillProposalResult {
    SkillProposalRecord record;
    SkillProposalEvaluation evaluation;
};
core::Result<EvaluateSkillProposalResult> evaluate_skill_proposal(
    const EvaluateSkillProposalInput& input);

// 状态流转（reject/apply/quarantine/stale）：转换 + 事件 + dispatch changed。
core::Result<SkillProposalRecord> reject_skill_proposal(
    const std::string& proposal_id, const std::string& workspace_dir,
    std::optional<std::string> agent_id = std::nullopt,
    std::optional<std::string> correlation_id = std::nullopt);
core::Result<SkillProposalRecord> apply_skill_proposal(
    const std::string& proposal_id, const std::string& workspace_dir,
    std::optional<std::string> agent_id = std::nullopt,
    std::optional<std::string> correlation_id = std::nullopt);

} // namespace skills
} // namespace hunterclaw
