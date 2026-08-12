#pragma once

// 🎯 对齐 OpenClaw src/skills/workshop/service.ts
//   封装 ProposalService，把底层 Store 的基础 CRUD 拼成带业务语义（discovery / hook
//   dispatch / auto-evaluate 等）的 facade。

#include "hunterclaw/core/result.h"
#include "hunterclaw/skills/skill_proposal_types.h"
#include <optional>
#include <string>
#include <vector>

namespace hunterclaw {
namespace skills {

// 创建 proposal 输入（对齐 CreateSkillProposalInput 全字段）。
struct CreateSkillProposalInput {
    SkillProposalKind kind = SkillProposalKind::Create;
    std::string skill_name;
    std::string title;
    std::string content;             // draft body (PROPOSAL.md)
    std::string description;         // 可以为空（update 场景缺省）
    SkillProposalSource source = SkillProposalSource::SkillWorkshop;
    std::optional<std::string> agent_id;
    std::string workspace_dir;
    // -- 对齐 types.ts 扩展字段 --
    std::optional<std::string> goal;
    std::optional<std::string> evidence;
    std::vector<SkillProposalSupportFile> support_files;
    bool autonomous_capture = false;
    std::optional<std::string> origin;
    std::vector<std::string> origin_run_ids;
    std::vector<int> origin_run_mutation_counts;
};

// 创建 proposal：持久化 + created 事件 + dispatch skill_proposal_changed(created)。
core::Result<SkillProposalRecord> create_skill_proposal(const CreateSkillProposalInput& input);

// 评估 proposal（对齐 evaluateSkillProposal）。
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

// 状态流转 — reason / expectedRevisionHash / correlationId 透传到 store
core::Result<SkillProposalRecord> reject_skill_proposal(
    const std::string& proposal_id, const std::string& workspace_dir,
    std::optional<std::string> agent_id = std::nullopt,
    std::optional<std::string> correlation_id = std::nullopt,
    std::optional<std::string> reason = std::nullopt,
    std::optional<std::string> expected_revision_hash = std::nullopt);
core::Result<SkillProposalRecord> apply_skill_proposal(
    const std::string& proposal_id, const std::string& workspace_dir,
    std::optional<std::string> agent_id = std::nullopt,
    std::optional<std::string> correlation_id = std::nullopt,
    std::optional<std::string> reason = std::nullopt,
    std::optional<std::string> expected_revision_hash = std::nullopt);
core::Result<SkillProposalRecord> quarantine_skill_proposal(
    const std::string& proposal_id, const std::string& workspace_dir,
    std::optional<std::string> agent_id = std::nullopt,
    std::optional<std::string> correlation_id = std::nullopt,
    std::optional<std::string> reason = std::nullopt,
    std::optional<std::string> expected_revision_hash = std::nullopt);

// 修订 pending proposal — reason / expectedRevisionHash / description 可选更新
struct ReviseSkillProposalInput {
    std::string proposal_id;
    std::string new_content;
    std::string workspace_dir;
    std::optional<std::string> agent_id;
    std::optional<std::string> correlation_id;
    std::optional<std::string> expected_revision_hash;
    std::optional<std::string> reason;
    std::optional<std::string> description;    // 可选修订
    std::vector<SkillProposalSupportFile> support_files;
    bool clear_support_files = false;
};
core::Result<SkillProposalRecord> revise_skill_proposal(
    const ReviseSkillProposalInput& input);

} // namespace skills
} // namespace hunterclaw
