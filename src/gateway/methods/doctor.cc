// ============================================================================
// doctor.cc - Diagnostic and health introspection
// 🎯 严格对齐 OpenClaw server-methods/doctor.ts
// ============================================================================

#include "hunterclaw/gateway/gateway.h"
#include "hunterclaw/gateway/method_guard.h"
#include "hunterclaw/memory/memory_index_manager.h"
#include "hunterclaw/tasks/task_executor.h"       // ✅ 对齐 OC: task status in doctor
#include "hunterclaw/tasks/task_flow_registry.h"  // ✅ 对齐 OC: TaskFlow health check
#include <nlohmann/json.hpp>
#include <chrono>

using json = nlohmann::json;

namespace hunterclaw {
namespace gateway {
namespace methods {

static inline int64_t doctor_now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()
    ).count();
}

// doctor.memory.status
// 🎯 对齐 OpenClaw server-methods/doctor.ts:770-820
// OpenClaw: const { manager, error } = await getActiveMemorySearchManager({ cfg, agentId, purpose: "status" })
//           if (!manager) → return { ok: false, error }
//           return { ok: true, ...manager.status() }
core::Result<json> method_doctor_memory_status(
    std::shared_ptr<GatewayServer> server,
    std::shared_ptr<MethodGuard> method_guard,
    const ClientContext& client_ctx,
    const json& params
) {
    (void)method_guard; (void)client_ctx; (void)params;

    json result;
    result["ts"] = doctor_now_ms();

    // 对齐 OpenClaw: resolveDefaultAgentId(cfg) → "main"
    std::string agent_id = "main";
    std::string workspace_dir;

    // 尝试从 server config 获取 workspace
    if (server) {
        auto cfg = server->config();
        if (cfg) {
            auto& data = cfg->data;
            if (data.contains("workspace") && data["workspace"].is_string()) {
                workspace_dir = data["workspace"].get<std::string>();
            }
        }
    }
    if (workspace_dir.empty()) {
        workspace_dir = ".";
    }

    // 对齐 OpenClaw: getActiveMemorySearchManager({ cfg, agentId, purpose: "status" })
    memory::MemoryIndexManager::Params mgr_params;
    mgr_params.agent_id = agent_id;
    mgr_params.workspace_dir = workspace_dir;
    mgr_params.purpose = "status";

    auto mgr_result = memory::MemoryIndexManager::get(mgr_params);
    if (mgr_result.is_err()) {
        result["ok"] = false;
        result["error"] = mgr_result.error;
        return core::Result<json>::Ok(result);
    }

    auto mgr = mgr_result.get();
    if (!mgr) {
        result["ok"] = false;
        result["error"] = "memory not configured";
        return core::Result<json>::Ok(result);
    }

    // 对齐 OpenClaw: return { ok: true, ...manager.status() }
    auto status = mgr->status();

    result["ok"] = true;
    result["backend"] = status.backend;
    if (status.files.has_value()) result["files"] = *status.files;
    if (status.chunks.has_value()) result["chunks"] = *status.chunks;
    if (status.dirty.has_value()) result["dirty"] = *status.dirty;
    result["provider"] = status.provider;
    if (status.workspace_dir.has_value()) result["workspaceDir"] = *status.workspace_dir;
    if (status.db_path.has_value()) result["dbPath"] = *status.db_path;
    if (status.requested_provider.has_value()) result["requestedProvider"] = *status.requested_provider;

    if (status.model.has_value()) {
        result["model"] = *status.model;
    }

    // sources
    if (status.sources.has_value()) {
        json sources_arr = json::array();
        for (const auto& s : *status.sources) {
            sources_arr.push_back(memory::to_string(s));
        }
        result["sources"] = sources_arr;
    }

    // cache
    if (status.cache.has_value()) {
        json cache_json;
        cache_json["enabled"] = status.cache->enabled;
        if (status.cache->entries.has_value()) {
            cache_json["entries"] = *(status.cache->entries);
        }
        result["cache"] = cache_json;
    }

    // fts
    if (status.fts.has_value()) {
        json fts_json;
        fts_json["enabled"] = status.fts->enabled;
        fts_json["available"] = status.fts->available;
        if (status.fts->error.has_value()) {
            fts_json["error"] = *status.fts->error;
        }
        result["fts"] = fts_json;
    }

    // vector
    if (status.vector.has_value()) {
        json vec_json;
        vec_json["enabled"] = status.vector->enabled;
        if (status.vector->available.has_value()) {
            vec_json["available"] = *status.vector->available;
        }
        if (status.vector->dims.has_value()) {
            vec_json["dims"] = *status.vector->dims;
        }
        if (status.vector->load_error.has_value()) {
            vec_json["loadError"] = *status.vector->load_error;
        }
        result["vector"] = vec_json;
    }

    // fallback
    if (status.fallback.has_value()) {
        json fb;
        fb["from"] = status.fallback->from;
        if (status.fallback->reason.has_value()) {
            fb["reason"] = *status.fallback->reason;
        }
        result["fallback"] = fb;
    }

    // batch
    if (status.batch.has_value()) {
        json batch_json;
        batch_json["enabled"] = status.batch->enabled;
        batch_json["failures"] = status.batch->failures;
        batch_json["limit"] = status.batch->limit;
        result["batch"] = batch_json;
    }

    // custom
    if (status.custom.has_value()) {
        json custom;
        custom["searchMode"] = status.custom->search_mode;
        if (status.custom->provider_unavailable_reason.has_value()) {
            custom["providerUnavailableReason"] = *status.custom->provider_unavailable_reason;
        }
        json recovery;
        recovery["attempts"] = status.custom->readonly_recovery_attempts;
        recovery["successes"] = status.custom->readonly_recovery_successes;
        recovery["failures"] = status.custom->readonly_recovery_failures;
        if (status.custom->readonly_recovery_last_error.has_value()) {
            recovery["lastError"] = *status.custom->readonly_recovery_last_error;
        }
        custom["readonlyRecovery"] = recovery;
        result["custom"] = custom;
    }

    return core::Result<json>::Ok(result);
}

// doctor.tasks.status
// ✅ 对齐 OpenClaw doctor-workspace-status.ts: TaskFlow health check + TaskRegistry summary
core::Result<json> method_doctor_tasks_status(
    std::shared_ptr<GatewayServer> server,
    std::shared_ptr<MethodGuard> method_guard,
    const ClientContext& client_ctx,
    const json& params
) {
    (void)method_guard; (void)client_ctx; (void)params;

    json result;
    result["ts"] = doctor_now_ms();

    // TaskRegistry summary (对齐 OC: getInspectableTaskRegistrySummary)
    auto task_registry = server->task_registry();
    if (task_registry) {
        auto summary = task_registry->get_summary();
        json tasks_json;
        tasks_json["total"] = summary.total;
        tasks_json["active"] = summary.active;
        tasks_json["terminal"] = summary.terminal;
        tasks_json["failures"] = summary.failures;
        tasks_json["byStatus"] = json(summary.by_status);
        tasks_json["byRuntime"] = json(summary.by_runtime);
        result["tasks"] = tasks_json;
    } else {
        result["tasks"] = nullptr;
    }

    // TaskFlowRegistry summary (对齐 OC: noteFlowRecoveryHints)
    auto task_flow_registry = server->task_flow_registry();
    if (task_flow_registry) {
        auto flows = task_flow_registry->list_task_flow_records();
        json flows_json;
        flows_json["total"] = (int)flows.size();
        int active = 0, blocked = 0, cancel_requested = 0;
        json suspicious_flows = json::array();
        for (const auto& flow : flows) {
            if (flow.status == tasks::TaskFlowStatus::QUEUED ||
                flow.status == tasks::TaskFlowStatus::RUNNING) {
                active++;
            }
            if (flow.status == tasks::TaskFlowStatus::BLOCKED) {
                blocked++;
                // 对齐 OC: check blocked flow with missing blockedTaskId
                if (flow.blocked_task_id && task_registry) {
                    auto linked_tasks = task_registry->list_tasks_for_flow_id(flow.flow_id);
                    bool blocked_task_found = false;
                    for (const auto& t : linked_tasks) {
                        if (t.task_id == *flow.blocked_task_id) {
                            blocked_task_found = true;
                            break;
                        }
                    }
                    if (!blocked_task_found) {
                        suspicious_flows.push_back({
                            {"flowId", flow.flow_id},
                            {"issue", "blocked TaskFlow points at missing task " + *flow.blocked_task_id}
                        });
                    }
                }
            }
            if (flow.cancel_requested_at) cancel_requested++;
            // 对齐 OC: check running managed flow with no tasks or wait state
            if (flow.sync_mode == tasks::TaskFlowSyncMode::MANAGED &&
                flow.status == tasks::TaskFlowStatus::RUNNING && task_registry) {
                auto linked = task_registry->list_tasks_for_flow_id(flow.flow_id);
                if (linked.empty() && !flow.wait_json) {
                    suspicious_flows.push_back({
                        {"flowId", flow.flow_id},
                        {"issue", "running managed TaskFlow has no linked tasks or wait state"}
                    });
                }
            }
        }
        flows_json["active"] = active;
        flows_json["blocked"] = blocked;
        flows_json["cancelRequested"] = cancel_requested;
        if (!suspicious_flows.empty()) {
            flows_json["suspiciousFlows"] = suspicious_flows;
        }
        result["taskFlows"] = flows_json;
    } else {
        result["taskFlows"] = nullptr;
    }

    // Task audit (对齐 OC: listTaskAuditFindings)
    if (task_registry) {
        auto all_tasks = task_registry->list_task_records();
        auto findings = tasks::list_task_audit_findings(all_tasks, doctor_now_ms());
        auto audit_summary = tasks::summarize_task_audit_findings(findings);
        result["taskAudit"] = {
            {"total", audit_summary.total},
            {"warnings", audit_summary.warnings},
            {"errors", audit_summary.errors}
        };
    }

    return core::Result<json>::Ok(result);
}

}  // namespace methods
}  // namespace gateway
}  // namespace hunterclaw
