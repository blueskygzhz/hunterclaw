//🎯 对齐 OpenClaw skills/workshop/types.ts 的数据模型序列化。

#include "hunterclaw/skills/skill_proposal_types.h"
#include "hunterclaw/infra/utilities.h"

namespace hunterclaw {
namespace skills {

using json = nlohmann::json;

// ---- 枚举 <-> string ----
std::string to_string(SkillProposalKind v) {
    return v == SkillProposalKind::Create ? "create" : "update";
}
std::string to_string(SkillProposalScannerState v) {
    switch (v) {
        case SkillProposalScannerState::Pending:     return "pending";
        case SkillProposalScannerState::Clean:       return "clean";
        case SkillProposalScannerState::Failed:      return "failed";
        case SkillProposalScannerState::Quarantined: return "quarantined";
    }
    return "pending";
}
std::string to_string(SkillProposalStatus v) {
    switch (v) {
        case SkillProposalStatus::Pending:     return "pending";
        case SkillProposalStatus::Applied:     return "applied";
        case SkillProposalStatus::Rejected:    return "rejected";
        case SkillProposalStatus::Quarantined: return "quarantined";
        case SkillProposalStatus::Stale:       return "stale";
    }
    return "pending";
}
std::string to_string(SkillProposalSource v) {
    switch (v) {
        case SkillProposalSource::SkillWorkshop: return "skill-workshop";
        case SkillProposalSource::Cli:           return "cli";
        case SkillProposalSource::Gateway:       return "gateway";
    }
    return "skill-workshop";
}
std::string to_string(SkillProposalEventType v) {
    switch (v) {
        case SkillProposalEventType::Created:             return "created";
        case SkillProposalEventType::Revised:             return "revised";
        case SkillProposalEventType::EvaluationCompleted: return "evaluation_completed";
        case SkillProposalEventType::Applied:             return "applied";
        case SkillProposalEventType::Rejected:            return "rejected";
        case SkillProposalEventType::Quarantined:         return "quarantined";
        case SkillProposalEventType::Stale:               return "stale";
    }
    return "created";
}
std::string to_string(SkillProposalEvaluationTrigger v) {
    return v == SkillProposalEvaluationTrigger::Apply ? "apply" : "manual";
}
std::optional<SkillProposalKind> parse_skill_proposal_kind(const std::string& s) {
    if (s == "create") return SkillProposalKind::Create;
    if (s == "update") return SkillProposalKind::Update;
    return std::nullopt;
}
std::optional<SkillProposalScannerState> parse_skill_proposal_scanner_state(const std::string& s) {
    if (s == "pending")     return SkillProposalScannerState::Pending;
    if (s == "clean")       return SkillProposalScannerState::Clean;
    if (s == "failed")      return SkillProposalScannerState::Failed;
    if (s == "quarantined") return SkillProposalScannerState::Quarantined;
    return std::nullopt;
}
std::optional<SkillProposalStatus> parse_skill_proposal_status(const std::string& s) {
    if (s == "pending")     return SkillProposalStatus::Pending;
    if (s == "applied")     return SkillProposalStatus::Applied;
    if (s == "rejected")    return SkillProposalStatus::Rejected;
    if (s == "quarantined") return SkillProposalStatus::Quarantined;
    if (s == "stale")       return SkillProposalStatus::Stale;
    return std::nullopt;
}
static std::optional<SkillProposalEventType> parse_event_type(const std::string& s) {
    if (s == "created")              return SkillProposalEventType::Created;
    if (s == "revised")              return SkillProposalEventType::Revised;
    if (s == "evaluation_completed") return SkillProposalEventType::EvaluationCompleted;
    if (s == "applied")              return SkillProposalEventType::Applied;
    if (s == "rejected")             return SkillProposalEventType::Rejected;
    if (s == "quarantined")          return SkillProposalEventType::Quarantined;
    if (s == "stale")                return SkillProposalEventType::Stale;
    return std::nullopt;
}

// ---- SupportFile ----
json SkillProposalSupportFile::to_json() const {
    return {{"path", path}, {"content", content}};
}
SkillProposalSupportFile SkillProposalSupportFile::from_json(const json& j) {
    SkillProposalSupportFile f;
    f.path    = j.value("path", "");
    f.content = j.value("content", "");
    return f;
}

// ---- ScannerFinding ----
json ScannerFinding::to_json() const {
    return {{"level", level}, {"message", message}};
}
ScannerFinding ScannerFinding::from_json(const json& j) {
    ScannerFinding f;
    f.level   = j.value("level", "info");
    f.message = j.value("message", "");
    return f;
}

// ---- SkillProposalScan ----
json SkillProposalScan::to_json() const {
    json j = {{"state", to_string(state)}, {"critical", critical},
              {"warn", warn}, {"info", info}};
    if (scanned_at) j["scannedAt"] = *scanned_at;
    if (!findings.empty()) {
        json arr = json::array();
        for (const auto& f : findings) arr.push_back(f.to_json());
        j["findings"] = arr;
    }
    return j;
}
SkillProposalScan SkillProposalScan::from_json(const json& j) {
    SkillProposalScan s;
    s.state = parse_skill_proposal_scanner_state(j.value("state", "pending"))
                  .value_or(SkillProposalScannerState::Pending);
    if (j.contains("scannedAt") && j["scannedAt"].is_string())
        s.scanned_at = j["scannedAt"].get<std::string>();
    s.critical = j.value("critical", 0);
    s.warn     = j.value("warn", 0);
    s.info     = j.value("info", 0);
    if (j.contains("findings") && j["findings"].is_array())
        for (const auto& fj : j["findings"]) s.findings.push_back(ScannerFinding::from_json(fj));
    return s;
}

// ---- EventActor ----
json SkillProposalEventActor::to_json() const {
    json j = json::object();
    if (source)   j["source"]   = *source;
    if (agent_id) j["agentId"]  = *agent_id;
    return j;
}
SkillProposalEventActor SkillProposalEventActor::from_json(const json& j) {
    SkillProposalEventActor a;
    if (j.contains("source") && j["source"].is_string())
        a.source = j["source"].get<std::string>();
    if (j.contains("agentId") && j["agentId"].is_string())
        a.agent_id = j["agentId"].get<std::string>();
    return a;
}

// ---- Finding ----
json SkillEvaluationFinding::to_json() const {
    json j = {{"ruleId", rule_id}, {"severity", severity}, {"message", message}};
    if (file) j["file"] = *file;
    if (line) j["line"] = *line;
    return j;
}
SkillEvaluationFinding SkillEvaluationFinding::from_json(const json& j) {
    SkillEvaluationFinding f;
    f.rule_id  = j.value("ruleId", "");
    f.severity = j.value("severity", "info");
    f.message  = j.value("message", "");
    if (j.contains("file") && j["file"].is_string()) f.file = j["file"].get<std::string>();
    if (j.contains("line") && j["line"].is_number_integer()) f.line = j["line"].get<int>();
    return f;
}

// ---- EvaluateResult ----
json SkillProposalEvaluateResult::to_json() const {
    json j = json::object();
    if (summary) j["summary"] = *summary;
    if (has_findings) {
        json arr = json::array();
        for (const auto& f : findings) arr.push_back(f.to_json());
        j["findings"] = arr;
    }
    if (has_metrics) j["metrics"] = metrics;
    if (evaluator_version) j["evaluatorVersion"] = *evaluator_version;
    if (mode) j["mode"] = *mode;
    if (decision) j["decision"] = *decision;
    if (decision_reason) j["decisionReason"] = *decision_reason;
    return j;
}
SkillProposalEvaluateResult SkillProposalEvaluateResult::from_json(const json& j) {
    SkillProposalEvaluateResult r;
    if (j.contains("summary") && j["summary"].is_string()) r.summary = j["summary"].get<std::string>();
    if (j.contains("findings") && j["findings"].is_array()) {
        r.has_findings = true;
        for (const auto& fj : j["findings"]) r.findings.push_back(SkillEvaluationFinding::from_json(fj));
    }
    if (j.contains("metrics") && j["metrics"].is_object()) {
        r.has_metrics = true; r.metrics = j["metrics"];
    }
    if (j.contains("evaluatorVersion") && j["evaluatorVersion"].is_string())
        r.evaluator_version = j["evaluatorVersion"].get<std::string>();
    if (j.contains("mode") && j["mode"].is_string()) r.mode = j["mode"].get<std::string>();
    if (j.contains("decision") && j["decision"].is_string()) r.decision = j["decision"].get<std::string>();
    if (j.contains("decisionReason") && j["decisionReason"].is_string())
        r.decision_reason = j["decisionReason"].get<std::string>();
    return r;
}

// ---- Outcome ----
json SkillProposalEvaluationOutcome::to_json() const {
    json j = {{"evaluatorId", evaluator_id}, {"pluginId", plugin_id}, {"status", status}};
    if (plugin_version) j["pluginVersion"] = *plugin_version;
    if (status == "completed" && result) j["result"] = result->to_json();
    if (status == "error" && error) j["error"] = *error;
    return j;
}
SkillProposalEvaluationOutcome SkillProposalEvaluationOutcome::from_json(const json& j) {
    SkillProposalEvaluationOutcome o;
    o.evaluator_id = j.value("evaluatorId", "");
    o.plugin_id    = j.value("pluginId", "unknown-plugin");
    o.status       = j.value("status", "error");
    if (j.contains("pluginVersion") && j["pluginVersion"].is_string())
        o.plugin_version = j["pluginVersion"].get<std::string>();
    if (o.status == "completed" && j.contains("result"))
        o.result = SkillProposalEvaluateResult::from_json(j["result"]);
    if (o.status == "error" && j.contains("error") && j["error"].is_string())
        o.error = j["error"].get<std::string>();
    return o;
}

// ---- Evaluation ----
json SkillProposalEvaluation::to_json() const {
    json arr = json::array();
    for (const auto& o : outcomes) arr.push_back(o.to_json());
    json j = {
        {"id", id}, {"proposedVersion", proposed_version}, {"revisionHash", revision_hash},
        {"trigger", trigger}, {"startedAt", started_at}, {"completedAt", completed_at},
        {"outcomes", arr},
    };
    if (correlation_id) j["correlationId"] = *correlation_id;
    return j;
}
SkillProposalEvaluation SkillProposalEvaluation::from_json(const json& j) {
    SkillProposalEvaluation e;
    e.id               = j.value("id", "");
    e.proposed_version = j.value("proposedVersion", "");
    e.revision_hash    = j.value("revisionHash", "");
    e.trigger          = j.value("trigger", "manual");
    e.started_at       = j.value("startedAt", "");
    e.completed_at     = j.value("completedAt", "");
    if (j.contains("correlationId") && j["correlationId"].is_string())
        e.correlation_id = j["correlationId"].get<std::string>();
    if (j.contains("outcomes") && j["outcomes"].is_array())
        for (const auto& oj : j["outcomes"]) e.outcomes.push_back(SkillProposalEvaluationOutcome::from_json(oj));
    return e;
}

// ---- Target ----
json SkillProposalTarget::to_json() const {
    json j = {{"skillName", skill_name}, {"skillKey", skill_key},
              {"skillFile", skill_file}, {"skillDir", skill_dir}};
    if (source) j["source"] = *source;
    if (current_content_hash) j["currentContentHash"] = *current_content_hash;
    return j;
}
SkillProposalTarget SkillProposalTarget::from_json(const json& j) {
    SkillProposalTarget t;
    t.skill_name = j.value("skillName", "");
    t.skill_key  = j.value("skillKey", "");
    t.skill_file = j.value("skillFile", "");
    t.skill_dir  = j.value("skillDir", "");
    if (j.contains("source") && j["source"].is_string()) t.source = j["source"].get<std::string>();
    if (j.contains("currentContentHash") && j["currentContentHash"].is_string())
        t.current_content_hash = j["currentContentHash"].get<std::string>();
    return t;
}

// ---- Record ----
json SkillProposalRecord::to_json() const {
    json j = {
        {"schema", schema}, {"id", id}, {"kind", to_string(kind)},
        {"status", to_string(status)}, {"title", title}, {"description", description},
        {"createdAt", created_at}, {"updatedAt", updated_at},
        {"createdBy", to_string(created_by)}, {"proposedVersion", proposed_version},
        {"draftHash", draft_hash}, {"content", content}, {"target", target.to_json()},
        {"draftFile", draft_file}, {"autonomousCapture", autonomous_capture},
    };
    if (evaluation) j["evaluation"] = evaluation->to_json();
    if (scan) j["scan"] = scan->to_json();
    if (applied_at)     j["appliedAt"]     = *applied_at;
    if (rejected_at)    j["rejectedAt"]    = *rejected_at;
    if (quarantined_at) j["quarantinedAt"] = *quarantined_at;
    if (stale_at)       j["staleAt"]       = *stale_at;
    if (status_reason)  j["statusReason"]  = *status_reason;
    if (goal)           j["goal"]          = *goal;
    if (evidence)       j["evidence"]      = *evidence;
    if (!support_files.empty()) {
        json arr = json::array();
        for (const auto& sf : support_files) arr.push_back(sf.to_json());
        j["supportFiles"] = arr;
    }
    if (origin) j["origin"] = *origin;
    if (!origin_run_ids.empty()) j["originRunIds"] = origin_run_ids;
    if (!origin_run_mutation_counts.empty()) j["originRunMutationCounts"] = origin_run_mutation_counts;
    return j;
}
SkillProposalRecord SkillProposalRecord::from_json(const json& j) {
    SkillProposalRecord r;
    r.schema= j.value("schema", std::string(SKILL_WORKSHOP_SCHEMA));
    r.id          = j.value("id", "");
    r.kind        = parse_skill_proposal_kind(j.value("kind", "create")).value_or(SkillProposalKind::Create);
    r.status      = parse_skill_proposal_status(j.value("status", "pending")).value_or(SkillProposalStatus::Pending);
    r.title       = j.value("title", "");
    r.description = j.value("description", "");
    r.created_at  = j.value("createdAt", "");
    r.updated_at  = j.value("updatedAt", "");
    std::string cb = j.value("createdBy", "skill-workshop");
    r.created_by  = cb == "cli" ? SkillProposalSource::Cli
                  : cb == "gateway" ? SkillProposalSource::Gateway : SkillProposalSource::SkillWorkshop;
    r.proposed_version = j.value("proposedVersion", "");
    r.draft_hash  = j.value("draftHash", "");
    r.content     = j.value("content", "");
    r.draft_file  = j.value("draftFile", "PROPOSAL.md");
    r.autonomous_capture = j.value("autonomousCapture", false);
    if (j.contains("target")) r.target = SkillProposalTarget::from_json(j["target"]);
    if (j.contains("evaluation") && j["evaluation"].is_object())
        r.evaluation = SkillProposalEvaluation::from_json(j["evaluation"]);
    if (j.contains("scan") && j["scan"].is_object())
        r.scan = SkillProposalScan::from_json(j["scan"]);
    if (j.contains("appliedAt") && j["appliedAt"].is_string())
        r.applied_at = j["appliedAt"].get<std::string>();
    if (j.contains("rejectedAt") && j["rejectedAt"].is_string())
        r.rejected_at = j["rejectedAt"].get<std::string>();
    if (j.contains("quarantinedAt") && j["quarantinedAt"].is_string())
        r.quarantined_at = j["quarantinedAt"].get<std::string>();
    if (j.contains("staleAt") && j["staleAt"].is_string())
        r.stale_at = j["staleAt"].get<std::string>();
    if (j.contains("statusReason") && j["statusReason"].is_string())
        r.status_reason = j["statusReason"].get<std::string>();
    if (j.contains("goal") && j["goal"].is_string())
        r.goal = j["goal"].get<std::string>();
    if (j.contains("evidence") && j["evidence"].is_string())
        r.evidence = j["evidence"].get<std::string>();
    if (j.contains("supportFiles") && j["supportFiles"].is_array())
        for (const auto& sj : j["supportFiles"])
            r.support_files.push_back(SkillProposalSupportFile::from_json(sj));
    if (j.contains("origin") && j["origin"].is_string())
        r.origin = j["origin"].get<std::string>();
    if (j.contains("originRunIds") && j["originRunIds"].is_array())
        for (const auto& v : j["originRunIds"]) r.origin_run_ids.push_back(v.get<std::string>());
    if (j.contains("originRunMutationCounts") && j["originRunMutationCounts"].is_array())
        for (const auto& v : j["originRunMutationCounts"]) r.origin_run_mutation_counts.push_back(v.get<int>());
    return r;
}

// ---- Event ----
json SkillProposalEvent::to_json() const {
    json j = {
        {"eventId", event_id}, {"proposalId", proposal_id}, {"sequence", sequence},
        {"proposedVersion", proposed_version}, {"revisionHash", revision_hash},
        {"type", to_string(type)}, {"occurredAt", occurred_at},
    };
    if (correlation_id) j["correlationId"] = *correlation_id;
    if (actor) j["actor"] = actor->to_json();
    if (payload) j["payload"] = *payload;
    if (evaluation) j["evaluation"] = evaluation->to_json();
    return j;
}
SkillProposalEvent SkillProposalEvent::from_json(const json& j) {
    SkillProposalEvent e;
    e.event_id        = j.value("eventId", "");
    e.proposal_id     = j.value("proposalId", "");
    e.sequence        = j.value("sequence", (int64_t)0);
    e.proposed_version= j.value("proposedVersion", "");
    e.revision_hash   = j.value("revisionHash", "");
    e.type            = parse_event_type(j.value("type", "created")).value_or(SkillProposalEventType::Created);
    e.occurred_at     = j.value("occurredAt", "");
    if (j.contains("correlationId") && j["correlationId"].is_string())
        e.correlation_id = j["correlationId"].get<std::string>();
    if (j.contains("actor") && j["actor"].is_object())
        e.actor = SkillProposalEventActor::from_json(j["actor"]);
    if (j.contains("payload")) e.payload = j["payload"];
    if (j.contains("evaluation") && j["evaluation"].is_object())
        e.evaluation = SkillProposalEvaluation::from_json(j["evaluation"]);
    return e;
}

std::string hash_skill_proposal_revision(const SkillProposalRecord& record) {
    // 对齐 revision-hash.ts 语义：稳定摘要覆盖身份 + 版本 + 内容哈希。
    return infra::sha256_hash(record.id + "\n" + record.proposed_version + "\n" + record.draft_hash);
}

} // namespace skills
} // namespace hunterclaw
