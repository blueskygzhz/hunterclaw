#pragma once

// 🎯 对齐 OpenClaw skills/workshop/store-sqlite-*.ts + store.ts
//   Skill Proposal 的 SQLite 持久化：proposals 主表 + skill_proposal_events 事件表。
//   写路径经 BEGIN IMMEDIATE + 进程内互斥串行化（对齐 target-lock + 乐观并发）。
//
//   表结构：
//     skill_proposals(id TEXT PK, format_version INTEGER CHECK(=1),
//                     status TEXT, record_json TEXT, updated_at INTEGER)
//     skill_proposal_events(event_id TEXT PK, proposal_id TEXT, sequence INTEGER,
//                           event_json TEXT, occurred_at INTEGER)

#include "hunterclaw/core/result.h"
#include "hunterclaw/skills/skill_proposal_types.h"
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace hunterclaw {
namespace skills {

// {state_dir}/state/hunterclaw.sqlite（对齐 mcp_oauth_persistence 复用同一 state db）。
std::string resolve_skill_proposal_state_db_path();

// 读取单条 proposal（缺失→nullopt）。
core::Result<std::optional<SkillProposalRecord>> read_skill_proposal(const std::string& id);

// 列出所有 proposal（可按 status 过滤）。
core::Result<std::vector<SkillProposalRecord>> list_skill_proposals(
    std::optional<SkillProposalStatus> status_filter = std::nullopt);

// 创建 proposal + 追加 "created" 事件（原子）。返回写入的 record + event。
struct CreateProposalResult { SkillProposalRecord record; SkillProposalEvent event; };
core::Result<CreateProposalResult> insert_skill_proposal(const SkillProposalRecord& record);

// 记录评估结果：乐观并发校验（proposedVersion + revisionHash 未变），
//   写回 record.evaluation + 追加 "evaluation_completed" 事件（原子）。
//   对齐 recordSkillProposalEvaluation。
struct RecordEvaluationResult { SkillProposalRecord record; SkillProposalEvent event; };
core::Result<RecordEvaluationResult> record_skill_proposal_evaluation(
    const std::string& proposal_id,
    const std::string& expected_proposed_version,
    const std::string& expected_revision_hash,
    const SkillProposalEvaluation& evaluation,
    const SkillProposalEvent& event);

// 修订 pending proposal 的内容（不改状态）+ 追加 Revised 事件，原子。
struct ReviseProposalResult { SkillProposalRecord record; SkillProposalEvent event; };
core::Result<ReviseProposalResult> revise_skill_proposal_content(
    const std::string& proposal_id,
    const std::string& new_content,
    const std::string& new_draft_hash,
    const std::string& new_proposed_version);

// 状态转换 + 追加对应事件（applied/rejected/quarantined/stale），原子。
// reason 写入 record.statusReason；expectedRevisionHash 做乐观并发校验。
core::Result<SkillProposalRecord> transition_skill_proposal_status(
    const std::string& proposal_id, SkillProposalStatus next_status,
    SkillProposalEventType event_type,
    std::optional<std::string> correlation_id = std::nullopt,
    std::optional<std::string> reason = std::nullopt,
    std::optional<std::string> expected_revision_hash = std::nullopt);

// 读取某 proposal 的事件列表（按 sequence 升序）。
core::Result<std::vector<SkillProposalEvent>> list_skill_proposal_events(const std::string& proposal_id);

// 原地更新 proposal 的 record_json（不改状态/不产生事件）。
core::Result<void> update_skill_proposal_record(const SkillProposalRecord& record);

// 测试辅助：覆盖 db 路径。
void set_skill_proposal_state_db_path_for_test(const std::string& path);

} // namespace skills
} // namespace hunterclaw
