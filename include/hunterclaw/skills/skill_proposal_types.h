#pragma once

// 🎯 对齐 OpenClaw src/skills/workshop/types.ts + plugins/hook-skill.types.ts
//   Skill Proposal（Skill Workshop）子系统的数据模型：proposal 记录、事件、评估
//   结果(outcome/finding)。状态机与枚举严格对齐 openclaw。

#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <vector>

namespace hunterclaw {
namespace skills {

// 对齐 SKILL_WORKSHOP_SCHEMA。
constexpr const char* SKILL_WORKSHOP_SCHEMA = "hunterclaw.skill-workshop.proposal.v1";
constexpr int SKILL_PROPOSAL_FORMAT_VERSION = 1;

// SkillProposalKind (types.ts:13)
enum class SkillProposalKind { Create, Update };

// SkillProposalScannerState (types.ts:15) — 对齐 openclaw
enum class SkillProposalScannerState { Pending, Clean, Failed, Quarantined };

// SkillProposalStatus (types.ts:14)
enum class SkillProposalStatus { Pending, Applied, Rejected, Quarantined, Stale };

// SkillProposalSource (types.ts:16)
enum class SkillProposalSource { SkillWorkshop, Cli, Gateway };

// SkillProposalEvaluationTrigger (types.ts:17)
enum class SkillProposalEvaluationTrigger { Manual, Apply };

// SkillProposalEventType (types.ts:18-25)
enum class SkillProposalEventType {
    Created, Revised, EvaluationCompleted, Applied, Rejected, Quarantined, Stale
};

std::string to_string(SkillProposalKind v);
std::string to_string(SkillProposalScannerState v);
std::string to_string(SkillProposalStatus v);
std::string to_string(SkillProposalSource v);
std::string to_string(SkillProposalEventType v);
std::string to_string(SkillProposalEvaluationTrigger v);
std::optional<SkillProposalKind> parse_skill_proposal_kind(const std::string& s);
std::optional<SkillProposalScannerState> parse_skill_proposal_scanner_state(const std::string& s);
std::optional<SkillProposalStatus> parse_skill_proposal_status(const std::string& s);

// ============================================================================
// Evaluation finding / result / outcome （对齐 hook-skill.types.ts:36-73）
// ============================================================================

// PluginHookSkillEvaluationFinding
struct SkillEvaluationFinding {
    std::string rule_id;
    std::string severity;   // "info" | "warn" | "critical"
    std::string message;
    std::optional<std::string> file;
    std::optional<int> line;

    nlohmann::json to_json() const;
    static SkillEvaluationFinding from_json(const nlohmann::json& j);
};

// PluginHookSkillProposalEvaluateResult
struct SkillProposalEvaluateResult {
    std::optional<std::string> summary;
    std::vector<SkillEvaluationFinding> findings;
    bool has_findings = false;
    nlohmann::json metrics = nlohmann::json::object();
    bool has_metrics = false;
    std::optional<std::string> evaluator_version;
    std::optional<std::string> mode;
    std::optional<std::string> decision;         // "pass" | "revise" | "block"
    std::optional<std::string> decision_reason;

    nlohmann::json to_json() const;
    static SkillProposalEvaluateResult from_json(const nlohmann::json& j);
};

// PluginHookSkillProposalEvaluationOutcome（含attribution）
struct SkillProposalEvaluationOutcome {
    std::string evaluator_id;
    std::string plugin_id;
    std::optional<std::string> plugin_version;
    std::string status;   // "completed" | "skipped" | "error"
    std::optional<SkillProposalEvaluateResult> result;   // completed
    std::optional<std::string> error;                    // error

    nlohmann::json to_json() const;
    static SkillProposalEvaluationOutcome from_json(const nlohmann::json& j);
};

// 评估集合（对齐 record.evaluation）
struct SkillProposalEvaluation {
    std::string id;
    std::string proposed_version;
    std::string revision_hash;
    std::string trigger;       // "manual" | "apply"
    std::string started_at;
    std::string completed_at;
    std::optional<std::string> correlation_id;
    std::vector<SkillProposalEvaluationOutcome> outcomes;

    nlohmann::json to_json() const;
    static SkillProposalEvaluation from_json(const nlohmann::json& j);
};

// ============================================================================
// Support files (对齐 types.ts 的 SkillProposalSupportFile)
// ============================================================================

struct SkillProposalSupportFile {
    std::string path;
    std::string content;

    nlohmann::json to_json() const;
    static SkillProposalSupportFile from_json(const nlohmann::json& j);
};

// ============================================================================
// Scan result (对齐 types.ts SkillProposalScan + ScannerFinding)
// ============================================================================

struct ScannerFinding {
    std::string level;   // "critical" | "warn" | "info"
    std::string message;
    nlohmann::json to_json() const;
    static ScannerFinding from_json(const nlohmann::json& j);
};

struct SkillProposalScan {
    SkillProposalScannerState state = SkillProposalScannerState::Pending;
    std::optional<std::string> scanned_at;
    int critical = 0;
    int warn = 0;
    int info = 0;
    std::vector<ScannerFinding> findings;

    nlohmann::json to_json() const;
    static SkillProposalScan from_json(const nlohmann::json& j);
};

// ============================================================================
// Event actor (对齐 types.ts SkillProposalEventActor)
// ============================================================================

struct SkillProposalEventActor {
    std::optional<std::string> source;   // "SkillWorkshop" | "Cli" | "Gateway"
    std::optional<std::string> agent_id;

    nlohmann::json to_json() const;
    static SkillProposalEventActor from_json(const nlohmann::json& j);
};

// ============================================================================
// Proposal 目标 + 记录 + 事件（对齐 types.ts:129-160）
// ============================================================================

struct SkillProposalTarget {
    std::string skill_name;
    std::string skill_key;
    std::string skill_file;
    std::string skill_dir;
    std::optional<std::string> source;
    std::optional<std::string> current_content_hash;

    nlohmann::json to_json() const;
    static SkillProposalTarget from_json(const nlohmann::json& j);
};

// SkillProposalRecord（严格对齐 types.ts SkillWorkshopRecord）
struct SkillProposalRecord {
    std::string schema = SKILL_WORKSHOP_SCHEMA;
    std::string id;
    SkillProposalKind kind = SkillProposalKind::Create;
    SkillProposalStatus status = SkillProposalStatus::Pending;
    std::string title;
    std::string description;
    std::string created_at;
    std::string updated_at;
    SkillProposalSource created_by = SkillProposalSource::SkillWorkshop;
    std::string proposed_version;   // 语义版本或修订号
    std::string draft_hash;         // sha256(draft body)
    std::string content;            // 草稿正文（skill.md / PROPOSAL.md body）
    std::string draft_file = "PROPOSAL.md";  // 内部文件名
    SkillProposalTarget target;
    std::optional<SkillProposalEvaluation> evaluation;
    std::optional<SkillProposalScan> scan;
    // Status 时间戳
    std::optional<std::string> applied_at;
    std::optional<std::string> rejected_at;
    std::optional<std::string> quarantined_at;
    std::optional<std::string> stale_at;
    std::optional<std::string> status_reason;
    // 体量/上下文 metadata（对齐 types.ts）
    std::optional<std::string> goal;
    std::optional<std::string> evidence;
    std::vector<SkillProposalSupportFile> support_files;
    bool autonomous_capture = false;
    // 溯源（proposal-only 场景可选）
    std::optional<std::string> origin;
    std::vector<std::string> origin_run_ids;
    std::vector<int> origin_run_mutation_counts;

    nlohmann::json to_json() const;
    static SkillProposalRecord from_json(const nlohmann::json& j);
};

// SkillProposalEvent（持久化到 events 表，对齐 types.ts）
struct SkillProposalEvent {
    std::string event_id;
    std::string proposal_id;
    int64_t sequence = 0;
    std::string proposed_version;
    std::string revision_hash;
    SkillProposalEventType type = SkillProposalEventType::Created;
    std::string occurred_at;
    std::optional<std::string> correlation_id;
    std::optional<SkillProposalEventActor> actor;
    std::optional<nlohmann::json> payload;
    std::optional<SkillProposalEvaluation> evaluation;

    nlohmann::json to_json() const;
    static SkillProposalEvent from_json(const nlohmann::json& j);
};

// revisionHash = sha256(id + proposedVersion + draftHash)（对齐 revision-hash.ts 语义）
std::string hash_skill_proposal_revision(const SkillProposalRecord& record);

} // namespace skills
} // namespace hunterclaw
