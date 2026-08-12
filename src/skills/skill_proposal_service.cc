// 🎯 对齐 OpenClaw src/skills/workshop/service.ts
//   把底层 Store 的基础 CRUD 拼成带业务语义的 facade。

#include "hunterclaw/skills/skill_proposal_service.h"
#include "hunterclaw/skills/skill_proposal_store.h"
#include "hunterclaw/tools/tool_hooks.h"
#include "hunterclaw/infra/utilities.h"

#include <spdlog/spdlog.h>
#include <spdlog/sinks/stdout_sinks.h>
#include <chrono>
#include <ctime>

namespace hunterclaw {
namespace skills {

namespace {
using json = nlohmann::json;

auto log() {
    static auto logger = spdlog::get("skills");
    if (!logger) logger = spdlog::stdout_logger_mt("skills");
    return logger;
}

std::string iso_now() {
    auto now = std::chrono::system_clock::now();
    std::time_t t = std::chrono::system_clock::to_time_t(now);
    std::tm tm_utc{};
    gmtime_r(&t, &tm_utc);
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tm_utc);
    return std::string(buf);
}

// 坐标推导（skill_name → path/key/dir）。
void resolve_skill_coords(const std::string& workspace_dir, const std::string& skill_name,
                          std::string& skill_key, std::string& skill_file, std::string& skill_dir) {
    skill_key = skill_name;  // 对齐：name = key（不含路径前缀）
    skill_dir = workspace_dir + "/skills/" + skill_key;
    skill_file = skill_dir + "/SKILL.md";
}

// 构造 skill_proposal_changed event payload。
plugins::PluginHookSkillProposalChangedEvent build_changed_event(
    const SkillProposalRecord& record, const SkillProposalEvent& event,
    const std::vector<SkillProposalEvaluationOutcome>* evaluations) {
    plugins::PluginHookSkillProposalChangedEvent ev;
    ev.event_id    = event.event_id;
    ev.sequence    = static_cast<int>(event.sequence);
    ev.action      = to_string(event.type);
    ev.occurred_at = event.occurred_at;
    json proposal = {
        {"id", record.id}, {"kind", to_string(record.kind)},
        {"status", to_string(record.status)}, {"revision", record.proposed_version},
        {"revisionSha256", event.revision_hash}, {"skillName", record.target.skill_name},
        {"skillKey", record.target.skill_key}, {"skillFile", record.target.skill_file},
    };
    if (record.target.source) proposal["source"] = *record.target.source;
    json payload = {{"proposal", proposal}};
    if (evaluations) {
        json arr = json::array();
        for (const auto& o : *evaluations) arr.push_back(o.to_json());
        payload["evaluations"] = arr;
    }
    ev.evaluations_json = payload.dump();
    return ev;
}

void dispatch_changed(const SkillProposalRecord& record, const SkillProposalEvent& event,
                      const std::string& workspace_dir, const std::optional<std::string>& agent_id,
                      const std::vector<SkillProposalEvaluationOutcome>* evaluations) {
    auto* mgr = tools::get_global_hook_manager();
    if (!mgr || !mgr->has_hooks(plugins::PluginHookName::SKILL_PROPOSAL_CHANGED)) return;
    auto ev = build_changed_event(record, event, evaluations);
    plugins::PluginHookSkillContext ctx;
    ctx.workspace_dir = workspace_dir;
    ctx.agent_id = agent_id;
    try { mgr->run_skill_proposal_changed(ev, ctx); }
    catch (const std::exception& e) { log()->warn("skill_proposal_changed hook failed: {}", e.what()); }
}

SkillProposalEvaluationOutcome convert_outcome(
    const plugins::PluginHookSkillProposalEvaluationOutcome& src) {
    SkillProposalEvaluationOutcome o;
    o.evaluator_id   = src.evaluator_id;
    o.plugin_id      = src.plugin_id;
    o.plugin_version = src.plugin_version;
    o.status         = src.status;
    o.error          = src.error;
    if (src.status == "completed" && src.result) {
        SkillProposalEvaluateResult r;
        r.summary           = src.result->summary;
        r.evaluator_version = src.result->evaluator_version;
        r.mode              = src.result->mode;
        r.decision          = src.result->decision;
        r.decision_reason   = src.result->decision_reason;
        if (src.result->findings_json) {
            try {
                auto arr = json::parse(*src.result->findings_json);
                if (arr.is_array()) {
                    r.has_findings = true;
                    for (const auto& fj : arr) r.findings.push_back(SkillEvaluationFinding::from_json(fj));
                }
            } catch (...) {}
        }
        if (src.result->metrics_json) {
            try {
                auto m = json::parse(*src.result->metrics_json);
                if (m.is_object()) { r.has_metrics = true; r.metrics = m; }
            } catch (...) {}
        }
        o.result = r;
    }
    return o;
}

} // namespace

core::Result<SkillProposalRecord> create_skill_proposal(const CreateSkillProposalInput& input) {
    using Ret = core::Result<SkillProposalRecord>;

    std::string title = input.title.empty()
        ? ((input.kind == SkillProposalKind::Create ? "Create " : "Update ") + input.skill_name)
        : input.title;

    std::string skill_key, skill_file, skill_dir;
    resolve_skill_coords(input.workspace_dir, input.skill_name, skill_key, skill_file, skill_dir);

    SkillProposalRecord record;
    record.id = "skp_" + infra::generate_uuid();
    record.kind = input.kind;
    record.status = SkillProposalStatus::Pending;
    record.title = title;
    record.description = input.description;
    record.created_at = iso_now();
    record.updated_at = record.created_at;
    record.created_by = input.source;
    record.proposed_version = "1";
    record.content = input.content;
    record.draft_hash = infra::sha256_hash(input.content);
    record.draft_file = "PROPOSAL.md";
    record.target.skill_name = input.skill_name;
    record.target.skill_key = skill_key;
    record.target.skill_file = skill_file;
    record.target.skill_dir = skill_dir;
    // 对齐 types.ts 扩展字段
    record.goal = input.goal;
    record.evidence = input.evidence;
    record.support_files = input.support_files;
    record.autonomous_capture = input.autonomous_capture;
    record.origin = input.origin;
    record.origin_run_ids = input.origin_run_ids;
    record.origin_run_mutation_counts = input.origin_run_mutation_counts;

    auto res = insert_skill_proposal(record);
    if (res.is_err()) return Ret::Err(res.error);

    dispatch_changed(res.get().record, res.get().event, input.workspace_dir, input.agent_id, nullptr);
    return Ret::Ok(res.get().record);
}

core::Result<EvaluateSkillProposalResult> evaluate_skill_proposal(
    const EvaluateSkillProposalInput& input) {
    using Ret = core::Result<EvaluateSkillProposalResult>;

    auto read = read_skill_proposal(input.proposal_id);
    if (read.is_err()) return Ret::Err(read.error);
    if (!read.get().has_value()) return Ret::Err("skill proposal not found: " + input.proposal_id);
    SkillProposalRecord record = *read.get();
    if (record.status != SkillProposalStatus::Pending)
        return Ret::Err("Only pending proposals can be evaluated. Current status: " + to_string(record.status));

    const std::string revision_hash = hash_skill_proposal_revision(record);
    if (input.expected_revision_hash && !input.expected_revision_hash->empty() &&
        *input.expected_revision_hash != revision_hash)
        return Ret::Err("Skill proposal revision changed; reload and retry.");

    auto* mgr = tools::get_global_hook_manager();
    const bool should_run = mgr && mgr->has_hooks(plugins::PluginHookName::SKILL_PROPOSAL_EVALUATE);

    std::string started_at = iso_now();
    std::vector<SkillProposalEvaluationOutcome> outcomes;

    if (should_run) {
        plugins::PluginHookSkillProposalEvaluateEvent event;
        json ev_json = {
            {"proposal", {
                {"id", record.id}, {"kind", to_string(record.kind)},
                {"revision", record.proposed_version}, {"revisionSha256", revision_hash},
            }},
            {"skill", {
                {"name", record.target.skill_name}, {"skillKey", record.target.skill_key},
                {"description", record.description},
            }},
            {"candidate", {
                {"skillMd", {{"path", record.target.skill_file}, {"content", record.content},
                             {"encoding", "utf8"}, {"sha256", record.draft_hash},
                             {"sizeBytes", (int)record.content.size()}}},
                {"files", json::array()},
                {"treeSha256", record.draft_hash},
            }},
            {"reason", input.trigger == SkillProposalEvaluationTrigger::Apply ? "apply" : "manual"},
        };
        if (record.target.source) ev_json["skill"]["source"] = *record.target.source;
        event.proposal_json = ev_json.dump();

        plugins::PluginHookSkillContext ctx;
        ctx.workspace_dir = input.workspace_dir;
        ctx.agent_id = input.agent_id;

        auto raw = mgr->run_skill_proposal_evaluate(event, ctx);
        for (const auto& o : raw) outcomes.push_back(convert_outcome(o));
    }

    std::string completed_at = iso_now();

    SkillProposalEvaluation evaluation;
    evaluation.id = infra::generate_uuid();
    evaluation.proposed_version = record.proposed_version;
    evaluation.revision_hash = revision_hash;
    evaluation.trigger = to_string(input.trigger);
    evaluation.started_at = started_at;
    evaluation.completed_at = completed_at;
    evaluation.correlation_id = input.correlation_id;
    evaluation.outcomes = outcomes;

    SkillProposalEvent event;
    event.event_id = infra::generate_uuid();
    event.proposal_id = record.id;
    event.proposed_version = record.proposed_version;
    event.revision_hash = revision_hash;
    event.type = SkillProposalEventType::EvaluationCompleted;
    event.occurred_at = completed_at;
    event.correlation_id = input.correlation_id;
    event.payload = json{
        {"evaluationId", evaluation.id}, {"trigger", evaluation.trigger},
        {"outcomeCount", (int)evaluation.outcomes.size()},
    };
    event.evaluation = evaluation;

    auto stored = record_skill_proposal_evaluation(
        record.id, record.proposed_version, revision_hash, evaluation, event);
    if (stored.is_err()) return Ret::Err(stored.error);

    dispatch_changed(stored.get().record, stored.get().event, input.workspace_dir,
                     input.agent_id, &outcomes);
    return Ret::Ok(EvaluateSkillProposalResult{stored.get().record, evaluation});
}

// 统一 transition helper — reason / expectedRevisionHash / correlationId 全透传
static core::Result<SkillProposalRecord> transition_and_dispatch(
    const std::string& proposal_id, SkillProposalStatus next, SkillProposalEventType type,
    const std::string& workspace_dir, std::optional<std::string> agent_id,
    std::optional<std::string> correlation_id, std::optional<std::string> reason,
    std::optional<std::string> expected_revision_hash) {
    auto res = transition_skill_proposal_status(proposal_id, next, type,
        correlation_id, reason, expected_revision_hash);
    if (res.is_err()) return res;
    SkillProposalRecord record = res.get();
    SkillProposalEvent ev;
    ev.event_id = infra::generate_uuid();
    ev.proposal_id = record.id;
    ev.proposed_version = record.proposed_version;
    ev.revision_hash = hash_skill_proposal_revision(record);
    ev.type = type;
    ev.occurred_at = record.updated_at;
    ev.correlation_id = correlation_id;
    dispatch_changed(record, ev, workspace_dir, agent_id, nullptr);
    return core::Result<SkillProposalRecord>::Ok(std::move(record));
}

core::Result<SkillProposalRecord> reject_skill_proposal(
    const std::string& proposal_id, const std::string& workspace_dir,
    std::optional<std::string> agent_id, std::optional<std::string> correlation_id,
    std::optional<std::string> reason, std::optional<std::string> expected_revision_hash) {
    return transition_and_dispatch(proposal_id, SkillProposalStatus::Rejected,
        SkillProposalEventType::Rejected, workspace_dir, agent_id,
        correlation_id, reason, expected_revision_hash);
}

core::Result<SkillProposalRecord> apply_skill_proposal(
    const std::string& proposal_id, const std::string& workspace_dir,
    std::optional<std::string> agent_id, std::optional<std::string> correlation_id,
    std::optional<std::string> reason, std::optional<std::string> expected_revision_hash) {
    return transition_and_dispatch(proposal_id, SkillProposalStatus::Applied,
        SkillProposalEventType::Applied, workspace_dir, agent_id,
        correlation_id, reason, expected_revision_hash);
}

core::Result<SkillProposalRecord> quarantine_skill_proposal(
    const std::string& proposal_id, const std::string& workspace_dir,
    std::optional<std::string> agent_id, std::optional<std::string> correlation_id,
    std::optional<std::string> reason, std::optional<std::string> expected_revision_hash) {
    return transition_and_dispatch(proposal_id, SkillProposalStatus::Quarantined,
        SkillProposalEventType::Quarantined, workspace_dir, agent_id,
        correlation_id, reason, expected_revision_hash);
}

core::Result<SkillProposalRecord> revise_skill_proposal(const ReviseSkillProposalInput& input) {
    using Ret = core::Result<SkillProposalRecord>;

    std::string new_draft_hash = infra::sha256_hash(input.new_content);

    auto read = read_skill_proposal(input.proposal_id);
    if (read.is_err()) return Ret::Err(read.error);
    if (!read.get().has_value()) return Ret::Err("skill proposal not found: " + input.proposal_id);
    SkillProposalRecord cur = *read.get();
    if (cur.status != SkillProposalStatus::Pending)
        return Ret::Err("Only pending proposals can be revised. Current status: " + to_string(cur.status));

    // expectedRevisionHash 校验
    if (input.expected_revision_hash && !input.expected_revision_hash->empty()) {
        std::string cur_hash = hash_skill_proposal_revision(cur);
        if (*input.expected_revision_hash != cur_hash)
            return Ret::Err("Skill proposal revision changed; reload and retry.");
    }

    int ver = 1;
    try { ver = std::stoi(cur.proposed_version) + 1; } catch (...) { ver = 2; }
    std::string new_version = std::to_string(ver);

    auto res = revise_skill_proposal_content(input.proposal_id, input.new_content,
                                             new_draft_hash, new_version);
    if (res.is_err()) return Ret::Err(res.error);

    // 修订 description / support_files（如果提供）
    SkillProposalRecord updated = res.get().record;
    bool record_changed = false;
    if (input.description) { updated.description = *input.description; record_changed = true; }
    if (input.clear_support_files) { updated.support_files.clear(); record_changed = true; }
    if (!input.support_files.empty()) {
        updated.support_files = input.support_files; record_changed = true;
    }
    if (input.reason) { updated.status_reason = *input.reason; record_changed = true; }
    if (record_changed) {
        auto upd = update_skill_proposal_record(updated);
        if (upd.is_err()) return Ret::Err(upd.error);
    }

    dispatch_changed(updated, res.get().event, input.workspace_dir, input.agent_id, nullptr);
    return Ret::Ok(updated);
}

} // namespace skills
} // namespace hunterclaw
