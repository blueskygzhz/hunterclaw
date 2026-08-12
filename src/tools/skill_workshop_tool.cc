// 🎯 严格对齐 OpenClaw src/agents/tools/skill-workshop-tool.ts
//   封装 SkillProposalService，给 LLM agent 提供提案全生命周期操控。

#include "hunterclaw/tools/skill_workshop_tool.h"
#include "hunterclaw/skills/skill_proposal_service.h"
#include "hunterclaw/skills/skill_proposal_store.h"
#include "hunterclaw/core/utils.h"
#include <nlohmann/json.hpp>

using json = nlohmann::json;

namespace hunterclaw {
namespace tools {
namespace {

// ── helpers ────────────────────────────────────────────────────────────────

// 对齐 OpenClaw: 返回 { content: [{type:"text", text:"..."}], details: {...} } 格式
ToolResult content_result(const std::string& text, const json& details) {
    ToolResult result;
    ToolResultContent tc;
    tc.type = ToolResultContent::Type::Text;
    tc.text = text;
    result.content = {tc};
    result.details = core::JsonValue(details);
    result.success = true;
    return result;
}

// proposal summary for text rendering
json proposal_summary(const skills::SkillProposalRecord& rec) {
    json j;
    j["id"]               = rec.id;
    j["kind"]             = skills::to_string(rec.kind);
    j["status"]           = skills::to_string(rec.status);
    j["skillName"]        = rec.target.skill_name;
    j["skillKey"]         = rec.target.skill_key;
    j["title"]            = rec.title;
    j["description"]      = rec.description;
    j["proposedVersion"]  = rec.proposed_version;
    j["draftHash"]        = rec.draft_hash;
    j["createdAt"]        = rec.created_at;
    j["updatedAt"]        = rec.updated_at;
    if (rec.applied_at)     j["appliedAt"]     = *rec.applied_at;
    if (rec.rejected_at)    j["rejectedAt"]    = *rec.rejected_at;
    if (rec.quarantined_at) j["quarantinedAt"] = *rec.quarantined_at;
    if (rec.status_reason)  j["statusReason"]  = *rec.status_reason;
    if (rec.evaluation)     j["evaluation"]    = rec.evaluation->to_json();
    return j;
}

// 从存储读取 proposal 并构造 result
ToolResult proposal_result(const skills::SkillProposalRecord& rec) {
    json details = proposal_summary(rec);
    details["content"] = rec.content;
    std::string text = "Proposal " + rec.id + " (" + skills::to_string(rec.kind) +
                       ") for " + rec.target.skill_name +
                       " [" + skills::to_string(rec.status) + "]";
    return content_result(text, details);
}

// skill_name → skill_key (对齐 OpenClaw normalizedSkillKey)
std::string normalize_skill_key(const std::string& name) {
    return name;  // name = key（不含路径前缀）
}

// 查找某 skill 的 pending update proposal（对齐 OpenClaw findUpdateProposal）
std::optional<skills::SkillProposalRecord> find_update_proposal(
    const std::string& skill_key) {
    auto list = skills::list_skill_proposals(skills::SkillProposalStatus::Pending);
    if (list.is_err()) return std::nullopt;
    for (const auto& rec : list.get()) {
        if (rec.target.skill_key == skill_key &&
            rec.kind == skills::SkillProposalKind::Update)
            return rec;
    }
    return std::nullopt;
}

// 从 ToolParams raw 值解析 support_files（对齐 support_files 数组格式）
std::vector<skills::SkillProposalSupportFile> parse_support_files(const ToolParams& p) {
    std::vector<skills::SkillProposalSupportFile> files;
    if (!p.has("support_files")) return files;
    const auto& raw = p.raw();
    if (!raw.has("support_files")) return files;
    const auto& sf_val = raw.at("support_files");
    if (!sf_val.is_array()) return files;
    for (size_t i = 0; i < sf_val.size(); ++i) {
        const auto& item = sf_val[i];
        if (!item.is_object()) continue;
        skills::SkillProposalSupportFile sf;
        sf.path    = item.get<std::string>("path", "");
        sf.content = item.get<std::string>("content", "");
        if (!sf.path.empty()) files.push_back(sf);
    }
    return files;
}

// ── SkillWorkshopTool ──────────────────────────────────────────────────────

class SkillWorkshopTool : public Tool {
public:
    explicit SkillWorkshopTool(const SkillWorkshopToolOptions& opts)
        : opts_(opts) {}

    std::string name() const override { return "skill_workshop"; }

    std::string description() const override {
        return R"(Create, update, read, patch, and manage skill proposals.

Proposals are drafts for changes to skills that go through evaluation before being applied.

Actions:
- create: Propose a new skill.
- update: Propose an update to an existing skill.
- patch: Make a targeted find-and-replace edit in an existing pending proposal.
- read: Read the current live SKILL.md content of an existing skill.
- list: List skill proposals (pending by default, filterable by status).
- inspect: Inspect a specific proposal by its proposal_id.
- evaluate: Run evaluators on a pending proposal so the agent can decide the next step.
- apply: Apply a proposal, writing the SKILL.md to disk and activating the skill.
- reject: Reject a proposal so it won't be applied.
- quarantine: Quarantine a proposal to remove the skill from loading.
- revise: Revise a pending proposal's draft body.
- complete: Complete the skill workshop review session (proposalOnly mode).

When to use each action:
- Use "create" when you're writing a brand-new skill from scratch. Pass "name" for the new skill name.
- Use "update" when you're changing an existing skill that already lives on disk. Pass "skill_name" for the existing skill name.
- Use "patch" when you have an existing pending update proposal and want to make a small targeted edit. Pass "skill_name", "old_string" and "new_string".
- Use "revise" when you want to replace the entire draft body (proposal_content) of a pending proposal.
- Always call "list" first if you need to check whether proposals already exist for a skill.

Required parameters:
- create: "name" (string), "proposal_content" (string), "description" (string; max 160 bytes)
- update: "skill_name" (string), "proposal_content" (string); "description" is optional
- patch: "skill_name" (string), "old_string" (string), "new_string" (string)
- read: "skill_name" (string)
- list: no required params; optional "status" (pending/applied/rejected/quarantined), "query" (search), "limit" (1-50)
- inspect: "proposal_id" (string)
- evaluate: "proposal_id" (string)
- apply/reject/quarantine: "proposal_id" (string)
- revise: "proposal_id" (string), "proposal_content" (string); optional "description"
- complete: no required params

Optional shared parameters:
- "goal" (string): intent / desired goal for the proposal
- "evidence" (string): evidence or justification for the change
- "support_files" (array of {path, content}): additional files to include in the proposal tree
- "expected_revision_hash" (string): optimistic concurrency guard for apply/reject/quarantine/revise
- "correlation_id" (string): client-controlled idempotency key
- "reason" (string): reason for apply/reject/quarantine/revise)";
    }

    Schema parameters() const override {
        Schema schema;
        schema.type = "object";
        schema.required = {"action"};

        schema.properties["action"] = core::JsonValue(json{
            {"type", "string"},
            {"enum", json::array({
                "create", "update", "patch", "read", "list",
                "inspect", "evaluate", "apply", "reject",
                "quarantine", "revise", "complete"}),
            },
            {"description", "Action to perform."},
        });

        // ── proposal_id ──
        schema.properties["proposal_id"] = core::JsonValue(json{
            {"type", "string"},
            {"description", "Proposal ID. Required for inspect, evaluate, apply, reject, quarantine, and revise."},  // 对齐 OpenClaw：complete 无需 proposal_id
        });

        // ── name (for create) ──
        schema.properties["name"] = core::JsonValue(json{
            {"type", "string"},
            {"description", "Skill name for create action. Lower-kebab-case slug."},
        });

        // ── skill_name (for update/patch/read) ──
        schema.properties["skill_name"] = core::JsonValue(json{
            {"type", "string"},
            {"description", "Existing skill name for update, patch, and read actions."},
        });

        // ── proposal_content ──
        schema.properties["proposal_content"] = core::JsonValue(json{
            {"type", "string"},
            {"description", "Full SKILL.md / PROPOSAL.md draft body. Required for create, update, and revise."},
        });

        // ── description ──
        schema.properties["description"] = core::JsonValue(json{
            {"type", "string"},
            {"description", "Short description (max 160 bytes). Required for create. Optional for update and revise."},
        });

        // ── old_string / new_string (patch) ──
        schema.properties["old_string"] = core::JsonValue(json{
            {"type", "string"},
            {"description", "Exact string to find and replace in the proposal draft. Required for patch action."},
        });
        schema.properties["new_string"] = core::JsonValue(json{
            {"type", "string"},
            {"description", "Replacement string. Required for patch action."},
        });

        // ── query / status / limit (list) ──
        schema.properties["query"] = core::JsonValue(json{
            {"type", "string"},
            {"description", "Free-text search filter for list action (searches skill name and description)."},
        });
        schema.properties["status"] = core::JsonValue(json{
            {"type", "string"},
            {"description", "Status filter for list action (pending, applied, rejected, quarantined, stale)."},
        });
        schema.properties["limit"] = core::JsonValue(json{
            {"type", "integer"},
            {"description", "Max proposals to return for list (1-50, default 20)."},
        });

        // ── support_files ──
        schema.properties["support_files"] = core::JsonValue(json{
            {"type", "array"},
            {"items", json{{"type", "object"}, {"properties", json{
                {"path",    json{{"type", "string"}}},
                {"content", json{{"type", "string"}}},
            }}, {"required", json::array({"path", "content"})}}},
            {"description", "Additional files to include in the proposal."},
        });

        // ── metadata ──
        schema.properties["goal"] = core::JsonValue(json{
            {"type", "string"},
            {"description", "Intent / desired goal for the proposal."},
        });
        schema.properties["evidence"] = core::JsonValue(json{
            {"type", "string"},
            {"description", "Evidence or justification for the change."},
        });

        // ── concurrency / idempotency ──
        schema.properties["expected_revision_hash"] = core::JsonValue(json{
            {"type", "string"},
            {"description", "Optimistic concurrency guard for apply/reject/quarantine/revise."},
        });
        schema.properties["correlation_id"] = core::JsonValue(json{
            {"type", "string"},
            {"description", "Client-controlled idempotency key."},
        });
        schema.properties["reason"] = core::JsonValue(json{
            {"type", "string"},
            {"description", "Human-readable reason for apply, reject, quarantine, or revise."},
        });

        return schema;
    }

    ToolResult execute(const core::JsonValue& params) override {
        try {
            ToolParams p(params);
            std::string action = p.read_string("action", true);

            if (action == "create")      return handle_create(p);
            if (action == "update")      return handle_update(p);
            if (action == "patch")       return handle_patch(p);
            if (action == "read")        return handle_read(p);
            if (action == "list")        return handle_list(p);
            if (action == "inspect")     return handle_inspect(p);
            if (action == "evaluate")    return handle_evaluate(p);
            if (action == "apply")       return handle_apply(p);
            if (action == "reject")      return handle_reject(p);
            if (action == "quarantine")  return handle_quarantine(p);
            if (action == "revise")      return handle_revise(p);
            if (action == "complete")   return handle_complete(p);

            return tool_result::error(
                "Unknown action: " + action +
                ". Supported: create, update, patch, read, list, inspect, "
                "evaluate, apply, reject, quarantine, revise, complete");
        } catch (const std::exception& e) {
            return tool_result::error(std::string("skill_workshop error: ") + e.what());
        }
    }

private:
    SkillWorkshopToolOptions opts_;

    // ── action: create ──────────────────────────────────────────────────

    ToolResult handle_create(ToolParams& p) {
        skills::CreateSkillProposalInput input;
        input.kind        = skills::SkillProposalKind::Create;
        input.skill_name  = p.read_string("name", true);              // 对齐: create 用 "name"
        input.title       = "Create " + input.skill_name;
        input.content     = p.read_string("proposal_content", true);   // 对齐: proposal_content
        input.description = p.read_string("description", true);
        input.source      = skills::SkillProposalSource::SkillWorkshop;
        input.workspace_dir = opts_.workspace_dir;
        input.agent_id    = opts_.agent_id;
        {
            std::string g = p.read_string("goal", false);
            if (!g.empty()) input.goal = g;
        }
        {
            std::string e = p.read_string("evidence", false);
            if (!e.empty()) input.evidence = e;
        }
        input.support_files = parse_support_files(p);

        auto res = skills::create_skill_proposal(input);
        if (res.is_err()) return tool_result::error(res.error);
        return proposal_result(res.get());
    }

    // ── action: update ──────────────────────────────────────────────────

    ToolResult handle_update(ToolParams& p) {
        std::string skill_name = p.read_string("skill_name", true);    // 对齐: update 用 "skill_name"

        skills::CreateSkillProposalInput input;
        input.kind        = skills::SkillProposalKind::Update;
        input.skill_name  = skill_name;
        input.title       = "Update " + skill_name;
        input.content     = p.read_string("proposal_content", true);   // 对齐: proposal_content
        input.description = p.read_string("description", false);       // 对齐: optional
        input.source      = skills::SkillProposalSource::SkillWorkshop;
        input.workspace_dir = opts_.workspace_dir;
        input.agent_id    = opts_.agent_id;
        {
            std::string g = p.read_string("goal", false);
            if (!g.empty()) input.goal = g;
        }
        {
            std::string e = p.read_string("evidence", false);
            if (!e.empty()) input.evidence = e;
        }
        input.support_files = parse_support_files(p);

        auto res = skills::create_skill_proposal(input);
        if (res.is_err()) return tool_result::error(res.error);
        return proposal_result(res.get());
    }

    // ── action: patch ──────────────────────────────────────────────────
    // 对齐 OpenClaw: targeted find-and-replace on existing pending update proposal

    ToolResult handle_patch(ToolParams& p) {
        std::string skill_name = p.read_string("skill_name", true);
        std::string old_string = p.read_string("old_string", true);
        std::string new_string = p.read_string("new_string", true);
        std::string skill_key = normalize_skill_key(skill_name);

        auto found = find_update_proposal(skill_key);
        if (!found.has_value())
            return tool_result::error(
                "No pending update proposal found for skill '" + skill_name +
                "'. Use 'update' action first to create one.");

        std::string content = found->content;
        size_t pos = content.find(old_string);
        if (pos == std::string::npos)
            return tool_result::error(
                "old_string not found in the current proposal draft. "
                "Use 'inspect' or 'read' to check current content first.");

        content.replace(pos, old_string.length(), new_string);

        skills::ReviseSkillProposalInput rev;
        rev.proposal_id      = found->id;
        rev.new_content      = content;
        rev.workspace_dir    = opts_.workspace_dir;
        rev.agent_id         = opts_.agent_id;
        {
            std::string c = p.read_string("correlation_id", false);
            if (!c.empty()) rev.correlation_id = c;
        }
        {
            std::string h = p.read_string("expected_revision_hash", false);
            if (!h.empty()) rev.expected_revision_hash = h;
        }
        {
            std::string r = p.read_string("reason", false);
            if (!r.empty()) rev.reason = r;
        }
        {
            std::string d = p.read_string("description", false);
            if (!d.empty()) rev.description = d;
        }

        auto res = skills::revise_skill_proposal(rev);
        if (res.is_err()) return tool_result::error(res.error);
        return proposal_result(res.get());
    }

    // ── action: read ────────────────────────────────────────────────────
    // 对齐 OpenClaw: 返回 live SKILL.md + updateProposals boolean

    ToolResult handle_read(ToolParams& p) {
        std::string skill_name = p.read_string("skill_name", true);
        std::string skill_key = normalize_skill_key(skill_name);

        std::string path = opts_.skills_dir + "/" + skill_key + "/SKILL.md";
        auto content = core::utils::read_file(path);
        if (content.is_err())
            return tool_result::error("Failed to read skill file: " + content.error);
        if (content.get().empty())
            return tool_result::error("Skill file is empty or not found: " + path);

        auto pending = find_update_proposal(skill_key);
        bool has_update_proposal = pending.has_value();

        json details;
        details["skillName"]  = skill_name;
        details["skillKey"]   = skill_key;
        details["path"]       = path;
        details["content"]    = content.get();
        details["hasUpdateProposal"] = has_update_proposal;
        if (has_update_proposal) {
            details["updateProposalId"]   = pending->id;
            details["updateProposalHash"] = pending->draft_hash;
        }

        std::string text = has_update_proposal
            ? "Read skill '" + skill_name + "' (a pending update proposal exists)."
            : "Read skill '" + skill_name + "'.";
        return content_result(text, details);
    }

    // ── action: list ────────────────────────────────────────────────────
    // 对齐 OpenClaw: status / query / limit

    ToolResult handle_list(ToolParams& p) {
        std::optional<skills::SkillProposalStatus> filter;
        std::string raw_status = p.read_string("status", false);       // 对齐: "status"
        if (!raw_status.empty())
            filter = skills::parse_skill_proposal_status(raw_status);

        std::string query = p.read_string("query", false);
        int limit = 20;
        int64_t limit_raw = p.read_int("limit", false, 20);
        limit = static_cast<int>(limit_raw);
        if (limit < 1) limit = 1;
        if (limit > 50) limit = 50;

        auto res = skills::list_skill_proposals(filter);
        if (res.is_err()) return tool_result::error(res.error);

        json proposals = json::array();
        int count = 0;
        for (const auto& rec : res.get()) {
            if (!filter.has_value()) {
                // Default: non-terminal = skip applied/rejected/stale
                if (rec.status == skills::SkillProposalStatus::Applied ||
                    rec.status == skills::SkillProposalStatus::Rejected ||
                    rec.status == skills::SkillProposalStatus::Stale)
                    continue;
            }
            // query filter (fzf-style substring match)
            if (!query.empty()) {
                std::string haystack = rec.target.skill_name + " " + rec.title + " " + rec.description;
                if (haystack.find(query) == std::string::npos) continue;
            }
            if (count >= limit) break;
            proposals.push_back(proposal_summary(rec));
            count++;
        }

        json details;
        details["proposals"] = proposals;
        details["count"] = proposals.size();

        std::string text = "Found " + std::to_string(proposals.size()) + " proposal(s)";
        if (!query.empty()) text += " matching '" + query + "'";
        text += ".";
        return content_result(text, details);
    }

    // ── action: inspect ─────────────────────────────────────────────────

    ToolResult handle_inspect(ToolParams& p) {
        std::string proposal_id = p.read_string("proposal_id", true);
        auto res = skills::read_skill_proposal(proposal_id);
        if (res.is_err()) return tool_result::error(res.error);
        if (!res.get().has_value())
            return tool_result::error("Proposal not found: " + proposal_id);
        return proposal_result(*res.get());
    }

    // ── action: evaluate ────────────────────────────────────────────────

    ToolResult handle_evaluate(ToolParams& p) {
        skills::EvaluateSkillProposalInput input;
        input.proposal_id     = p.read_string("proposal_id", true);
        input.trigger         = skills::SkillProposalEvaluationTrigger::Manual;
        input.workspace_dir   = opts_.workspace_dir;
        input.agent_id        = opts_.agent_id;
        {
            std::string c = p.read_string("correlation_id", false);
            if (!c.empty()) input.correlation_id = c;
        }
        {
            std::string h = p.read_string("expected_revision_hash", false);
            if (!h.empty()) input.expected_revision_hash = h;
        }

        auto res = skills::evaluate_skill_proposal(input);
        if (res.is_err()) return tool_result::error(res.error);

        json details = proposal_summary(res.get().record);
        details["content"] = res.get().record.content;
        details["evaluation"] = res.get().evaluation.to_json();

        std::string text = "Evaluation completed for proposal " +
            res.get().record.id + " (" + res.get().record.target.skill_name + ").";
        return content_result(text, details);
    }

    // ── action: apply ───────────────────────────────────────────────────
    // 对齐 OpenClaw: reason / expectedRevisionHash / correlationId 透传

    ToolResult handle_apply(ToolParams& p) {
        std::string proposal_id = p.read_string("proposal_id", true);
        std::optional<std::string> correlation_id, reason, expected_hash;
        {
            std::string c = p.read_string("correlation_id", false);
            if (!c.empty()) correlation_id = c;
        }
        {
            std::string r = p.read_string("reason", false);
            if (!r.empty()) reason = r;
        }
        {
            std::string h = p.read_string("expected_revision_hash", false);
            if (!h.empty()) expected_hash = h;
        }
        auto res = skills::apply_skill_proposal(
            proposal_id, opts_.workspace_dir, opts_.agent_id,
            correlation_id, reason, expected_hash);
        if (res.is_err()) return tool_result::error(res.error);
        return proposal_result(res.get());
    }

    // ── action: reject ──────────────────────────────────────────────────

    ToolResult handle_reject(ToolParams& p) {
        std::string proposal_id = p.read_string("proposal_id", true);
        std::optional<std::string> correlation_id, reason, expected_hash;
        {
            std::string c = p.read_string("correlation_id", false);
            if (!c.empty()) correlation_id = c;
        }
        {
            std::string r = p.read_string("reason", false);
            if (!r.empty()) reason = r;
        }
        {
            std::string h = p.read_string("expected_revision_hash", false);
            if (!h.empty()) expected_hash = h;
        }
        auto res = skills::reject_skill_proposal(
            proposal_id, opts_.workspace_dir, opts_.agent_id,
            correlation_id, reason, expected_hash);
        if (res.is_err()) return tool_result::error(res.error);
        return proposal_result(res.get());
    }

    // ── action: quarantine ──────────────────────────────────────────────

    ToolResult handle_quarantine(ToolParams& p) {
        std::string proposal_id = p.read_string("proposal_id", true);
        std::optional<std::string> correlation_id, reason, expected_hash;
        {
            std::string c = p.read_string("correlation_id", false);
            if (!c.empty()) correlation_id = c;
        }
        {
            std::string r = p.read_string("reason", false);
            if (!r.empty()) reason = r;
        }
        {
            std::string h = p.read_string("expected_revision_hash", false);
            if (!h.empty()) expected_hash = h;
        }
        auto res = skills::quarantine_skill_proposal(
            proposal_id, opts_.workspace_dir, opts_.agent_id,
            correlation_id, reason, expected_hash);
        if (res.is_err()) return tool_result::error(res.error);
        return proposal_result(res.get());
    }

    // ── action: revise ──────────────────────────────────────────────────
    // 对齐 OpenClaw: proposal_content / reason / expectedRevisionHash / correlationId / description

    ToolResult handle_revise(ToolParams& p) {
        skills::ReviseSkillProposalInput input;
        input.proposal_id   = p.read_string("proposal_id", true);
        input.new_content   = p.read_string("proposal_content", true);  // 对齐: proposal_content
        input.workspace_dir = opts_.workspace_dir;
        input.agent_id      = opts_.agent_id;
        {
            std::string c = p.read_string("correlation_id", false);
            if (!c.empty()) input.correlation_id = c;
        }
        {
            std::string h = p.read_string("expected_revision_hash", false);
            if (!h.empty()) input.expected_revision_hash = h;
        }
        {
            std::string r = p.read_string("reason", false);
            if (!r.empty()) input.reason = r;
        }
        {
            std::string d = p.read_string("description", false);
            if (!d.empty()) input.description = d;
        }
        input.support_files = parse_support_files(p);

        auto res = skills::revise_skill_proposal(input);
        if (res.is_err()) return tool_result::error(res.error);
        return proposal_result(res.get());
    }

    // ── action: complete ─────────────────────────────────────────────────
    // 对齐 OpenClaw: completeProposalReview() — 完成 proposal-only 评审会话

    ToolResult handle_complete(ToolParams& /*p*/) {
        if (!opts_.proposal_review_completion) {
            return tool_result::error(
                "complete is only available during proposal-only reviews");
        }

        json details;
        details["completed"] = true;

        return content_result("Completed Skill Workshop review.", details);
    }
};

} // anonymous namespace

// ── factory ────────────────────────────────────────────────────────────────

std::shared_ptr<Tool> create_skill_workshop_tool(
    const SkillWorkshopToolOptions& opts) {
    return std::make_shared<SkillWorkshopTool>(opts);
}

} // namespace tools
} // namespace hunterclaw
