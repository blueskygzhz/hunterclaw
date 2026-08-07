// 🎯 对齐 OpenClaw skills/workshop/service.ts + service-evaluation.ts + plugin-hooks.ts
//   Skill Proposal生命周期 + evaluate 流水线，触发 skill_proposal_evaluate /
//   skill_proposal_changed 两个 plugin hook。

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

// 构造 skill_proposal_changed event（对齐 dispatchSkillProposalChanged）。
plugins::PluginHookSkillProposalChangedEvent build_changed_event(
    const SkillProposalRecord& record, const SkillProposalEvent& event,
    const std::vector<SkillProposalEvaluationOutcome>* evaluations) {
    plugins::PluginHookSkillProposalChangedEvent ev;
    ev.event_id    = event.event_id;
    ev.sequence    = static_cast<int>(event.sequence);
    ev.action      = to_string(event.type);
    ev.occurred_at = event.occurred_at;
    // proposal 快照 +可选 evaluations 序列化进 evaluations_json（对齐 event.evaluations）。
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

// 把 plugin outcome（plugins 命名空间）转成 skills 领域 outcome。
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
    SkillProposalRecord record;
    record.id = "skp_" + infra::generate_uuid();
    record.kind = input.kind;
    record.status = SkillProposalStatus::Pending;
    record.title = (input.kind == SkillProposalKind::Create ? "Create " : "Update ") + input.skill_name;
    record.description = input.description;
    record.created_at = iso_now();
    record.updated_at = record.created_at;
    record.created_by = input.created_by;
    record.proposed_version = input.proposed_version.empty() ? "1" : input.proposed_version;
    record.content = input.content;
    record.draft_hash = infra::sha256_hash(input.content);
    record.target.skill_name = input.skill_name;
    record.target.skill_key = input.skill_key;
    record.target.skill_file = input.skill_file;
    record.target.skill_dir = input.skill_dir;
    record.target.source = input.source;

    auto res = insert_skill_proposal(record);
    if (res.is_err()) return Ret::Err(res.error);

    // dispatch skill_proposal_changed(created)
    dispatch_changed(res.get().record, res.get().event, input.workspace_dir, input.agent_id, nullptr);
    return Ret::Ok(res.get().record);
}

core::Result<EvaluateSkillProposalResult> evaluate_skill_proposal(
    const EvaluateSkillProposalInput& input) {
    using Ret = core::Result<EvaluateSkillProposalResult>;

    // 1. 读 pending proposal。
    auto read = read_skill_proposal(input.proposal_id);
    if (read.is_err()) return Ret::Err(read.error);
    if (!read.get().has_value()) return Ret::Err("skill proposal not found: " + input.proposal_id);
    SkillProposalRecord record = *read.get();
    if (record.status != SkillProposalStatus::Pending)
        return Ret::Err("Only pending proposals can be evaluated. Current status: " + to_string(record.status));

    const std::string revision_hash = hash_skill_proposal_revision(record);
    // expectedRevisionHash 校验（对齐 assertExpectedRevisionHash）。
    if (input.expected_revision_hash && !input.expected_revision_hash->empty() &&
        *input.expected_revision_hash != revision_hash)
        return Ret::Err("Skill proposal revision changed; reload and retry.");

    auto* mgr = tools::get_global_hook_manager();
    const bool should_run = mgr && mgr->has_hooks(plugins::PluginHookName::SKILL_PROPOSAL_EVALUATE);

    std::string started_at = iso_now();
    std::vector<SkillProposalEvaluationOutcome> outcomes;

    // 2. 触发 skill_proposal_evaluate hook 收集 outcomes（对齐 runSkillProposalEvaluators）。
    if (should_run) {
        plugins::PluginHookSkillProposalEvaluateEvent event;
        // 构造 evaluate event payload（proposal + skill + candidate 快照）。
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

    // 3. 组装 evaluation。
    SkillProposalEvaluation evaluation;
    evaluation.id = infra::generate_uuid();
    evaluation.proposed_version = record.proposed_version;
    evaluation.revision_hash = revision_hash;
    evaluation.trigger = to_string(input.trigger);
    evaluation.started_at = started_at;
    evaluation.completed_at = completed_at;
    evaluation.correlation_id = input.correlation_id;
    evaluation.outcomes = outcomes;

    // 构造 evaluation_completed 事件。
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

    // 4. 持久化（乐观并发校验）。
    auto stored = record_skill_proposal_evaluation(
        record.id, record.proposed_version, revision_hash, evaluation, event);
    if (stored.is_err()) return Ret::Err(stored.error);

    // 5. dispatch skill_proposal_changed(evaluation_completed, evaluations)。
    dispatch_changed(stored.get().record, stored.get().event, input.workspace_dir,
                     input.agent_id, &outcomes);

    return Ret::Ok(EvaluateSkillProposalResult{stored.get().record, evaluation});
}

static core::Result<SkillProposalRecord> transition_and_dispatch(
    const std::string& proposal_id, SkillProposalStatus next, SkillProposalEventType type,
    const std::string& workspace_dir, std::optional<std::string> agent_id,
    std::optional<std::string> correlation_id) {
    auto res = transition_skill_proposal_status(proposal_id, next, type, correlation_id);
    if (res.is_err()) return res;
    // 转换事件是本函数内追加的最后一条；重新构造用于 dispatch。
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
    std::optional<std::string> agent_id, std::optional<std::string> correlation_id) {
    return transition_and_dispatch(proposal_id, SkillProposalStatus::Rejected,
        SkillProposalEventType::Rejected, workspace_dir, agent_id, correlation_id);
}

core::Result<SkillProposalRecord> apply_skill_proposal(
    const std::string& proposal_id, const std::string& workspace_dir,
    std::optional<std::string> agent_id, std::optional<std::string> correlation_id) {
    return transition_and_dispatch(proposal_id, SkillProposalStatus::Applied,
        SkillProposalEventType::Applied, workspace_dir, agent_id, correlation_id);
}

} // namespace skills
} // namespace hunterclaw
