// ============================================================================
// stubs.cc - OpenClaw 对齐的方法实现
// 🎯 严格对齐 OpenClaw server-methods/ 目录下所有方法
//
// ⚠️  所有方法已迁移到独立实现文件:
//
//   devices.cc       → device.pair.* / device.token.* (6个方法)
//   send_impl.cc     → send / send.message / poll (3个方法)
//   nodes_impl.cc    → node.* (16个方法)
//   agents_impl.cc   → agents.* (7个方法)
//   usage.cc         → usage.status / usage.cost / sessions.usage / sessions.usage.timeseries / sessions.usage.logs (5个方法)
//   models.cc        → models.list (1个方法)
//   browser.cc       → browser.request (1个方法)
//   tools_catalog.cc → tools.catalog (1个方法)
//   secrets.cc       → secrets.reload / secrets.resolve (2个方法)
//   system.cc        → system-presence / system-event (2个方法)
//   wizard.cc        → wizard.start / wizard.next / wizard.cancel / wizard.status (4个方法)
//   tts.cc           → tts.status / tts.enable / tts.disable / tts.convert / tts.setProvider / tts.providers (6个方法)
//   talk.cc          → talk.config / talk.mode (2个方法)
//   logs.cc          → logs.tail (1个方法)
//   doctor.cc        → doctor.memory.status (1个方法)
//   voicewake.cc     → voicewake.get / voicewake.set (2个方法)
//   update.cc        → update.run (1个方法)
//   web.cc           → web.login.start / web.login.wait (2个方法)
//   push.cc          → push.test (1个方法)
//
// 本文件保留:
//   - gateway.identity.get (已实现，保持不动)
//   - last-heartbeat / set-heartbeats (已实现，保持不动)
//   - agent.identity.get / agent.wait (小方法)
//   - skills.status / skills.bins / skills.install / skills.update (已有 skills.cc)
//
// ============================================================================

#include "hunterclaw/gateway/gateway.h"
#include "hunterclaw/gateway/method_guard.h"
#include "hunterclaw/core/utils.h"
#include "hunterclaw/infra/heartbeat_events.h"
#include "hunterclaw/infra/heartbeat_wake.h"
#include <nlohmann/json.hpp>
#include <chrono>
#include <fstream>
#include <filesystem>

using json = nlohmann::json;

namespace hunterclaw {
namespace gateway {
namespace methods {

static inline int64_t now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()
    ).count();
}

// ============================================================================
// Agent Identity Methods (agent.ts)
// 🎯 OpenClaw 对齐: agentHandlers
// ============================================================================

// agent.identity.get
core::Result<json> method_agent_identity_get(
    std::shared_ptr<GatewayServer> server,
    std::shared_ptr<MethodGuard> method_guard,
    const ClientContext& client_ctx,
    const json& params
) {
    (void)method_guard; (void)client_ctx; (void)params;

    auto cfg = server->config();
    json result;
    result["agentId"] = "main";
    std::string name = "HunterClaw";
    if (cfg && cfg->data.contains("agent") && cfg->data["agent"].contains("name") &&
        cfg->data["agent"]["name"].is_string()) {
        name = cfg->data["agent"]["name"].get<std::string>();
    }
    result["name"] = name;
    return core::Result<json>::Ok(result);
}

// agent.wait
core::Result<json> method_agent_wait(
    std::shared_ptr<GatewayServer> server,
    std::shared_ptr<MethodGuard> method_guard,
    const ClientContext& client_ctx,
    const json& params
) {
    (void)server; (void)method_guard; (void)client_ctx; (void)params;
    // 🎯 OpenClaw: agent.wait returns when agent becomes idle
    json result;
    result["ok"] = true;
    result["status"] = "idle";
    return core::Result<json>::Ok(result);
}

// ============================================================================
// Gateway Identity (system.ts)
// 🎯 OpenClaw 对齐: systemHandlers["gateway.identity.get"]
// ============================================================================
core::Result<json> method_gateway_identity_get(
    std::shared_ptr<GatewayServer> server,
    std::shared_ptr<MethodGuard> method_guard,
    const ClientContext& client_ctx,
    const json& params
) {
    (void)method_guard; (void)client_ctx; (void)params;

    auto cfg = server->config();
    std::string identity_file = cfg ? (cfg->config_dir + "/device-identity.json") : "";

    json result;
    if (!identity_file.empty() && std::filesystem::exists(identity_file)) {
        try {
            std::ifstream ident_f(identity_file);
            if (ident_f.is_open()) {
                json identity;
                ident_f >> identity;
                result["deviceId"] = identity.value("deviceId",
                    std::string("hunterclaw-") + core::utils::generate_uuid());
                result["publicKey"] = identity.contains("publicKey")
                    ? identity["publicKey"] : json(nullptr);
            }
        } catch (...) {
            result["deviceId"] = "hunterclaw-" + core::utils::generate_uuid();
            result["publicKey"] = nullptr;
        }
    } else {
        std::string new_id = "hunterclaw-" + core::utils::generate_uuid();
        result["deviceId"] = new_id;
        result["publicKey"] = nullptr;
        if (!identity_file.empty()) {
            try {
                std::filesystem::create_directories(
                    std::filesystem::path(identity_file).parent_path());
                std::ofstream out(identity_file);
                if (out.is_open()) { out << result.dump(2); }
            } catch (...) {}
        }
    }
    return core::Result<json>::Ok(result);
}

// ============================================================================
// Heartbeat Methods
// 🎯 OpenClaw 对齐: systemHandlers["last-heartbeat"] / ["set-heartbeats"]
// ============================================================================

// last-heartbeat
core::Result<json> method_last_heartbeat(
    std::shared_ptr<GatewayServer> server,
    std::shared_ptr<MethodGuard> method_guard,
    const ClientContext& client_ctx,
    const json& params
) {
    (void)server; (void)method_guard; (void)client_ctx; (void)params;
    auto last = hunterclaw::infra::get_last_heartbeat_event();
    if (!last.has_value()) {
        return core::Result<json>::Ok(json(nullptr));
    }
    const auto& evt = last.value();
    json result;
    result["ts"]     = evt.ts;
    result["status"] = evt.status;
    if (evt.to.has_value())           result["to"]         = evt.to.value();
    if (evt.account_id.has_value())   result["accountId"]  = evt.account_id.value();
    if (evt.preview.has_value())      result["preview"]    = evt.preview.value();
    if (evt.duration_ms.has_value())  result["durationMs"] = evt.duration_ms.value();
    if (evt.has_media.has_value())    result["hasMedia"]   = evt.has_media.value();
    if (evt.reason.has_value())       result["reason"]     = evt.reason.value();
    if (evt.channel.has_value())      result["channel"]    = evt.channel.value();
    if (evt.silent.has_value())       result["silent"]     = evt.silent.value();
    if (evt.indicator_type.has_value()) {
        switch (evt.indicator_type.value()) {
            case hunterclaw::infra::HeartbeatIndicatorType::Ok:    result["indicatorType"] = "ok";    break;
            case hunterclaw::infra::HeartbeatIndicatorType::Alert: result["indicatorType"] = "alert"; break;
            case hunterclaw::infra::HeartbeatIndicatorType::Error: result["indicatorType"] = "error"; break;
        }
    }
    return core::Result<json>::Ok(result);
}

// set-heartbeats
core::Result<json> method_set_heartbeats(
    std::shared_ptr<GatewayServer> server,
    std::shared_ptr<MethodGuard> method_guard,
    const ClientContext& client_ctx,
    const json& params
) {
    (void)server; (void)method_guard; (void)client_ctx;
    if (!params.contains("enabled") || !params["enabled"].is_boolean()) {
        return core::Result<json>::Err(
            "invalid set-heartbeats params: enabled (boolean) required"
        );
    }
    bool enabled = params["enabled"].get<bool>();
    hunterclaw::infra::set_heartbeats_enabled(enabled);
    json result;
    result["ok"]      = true;
    result["enabled"] = enabled;
    return core::Result<json>::Ok(result);
}

// ============================================================================
// Skills Methods - keeping stubs here since skills.cc has partial impl
// 🎯 OpenClaw 对齐: skillsHandlers (stubs for methods not in skills.cc)
// ============================================================================

core::Result<json> method_skills_status(
    std::shared_ptr<GatewayServer> server,
    std::shared_ptr<MethodGuard> method_guard,
    const ClientContext& client_ctx,
    const json& params
) {
    (void)method_guard; (void)client_ctx; (void)params;

    // 🎯 严格对齐 OpenClaw: skills.status (server-methods/skills.ts)
    json result;
    result["ok"] = true;

    auto skill_mgr = server->skill_manager();
    if (!skill_mgr) {
        result["skills"] = json::array();
        return core::Result<json>::Ok(result);
    }

    // Get all skills from manager
    auto snapshot = skill_mgr->get_snapshot();
    json skills_arr = json::array();
    for (const auto& skill : snapshot.skills) {
        json s;
        s["name"] = skill.name;
        s["description"] = skill.description;
        s["location"] = skill.file_path.string();
        s["installed"] = skill.installed;
        s["enabled"] = skill.enabled;
        if (skill.version.has_value()) s["version"] = *skill.version;
        skills_arr.push_back(s);
    }
    result["skills"] = skills_arr;
    result["version"] = snapshot.version;
    return core::Result<json>::Ok(result);
}

core::Result<json> method_skills_bins(
    std::shared_ptr<GatewayServer> server,
    std::shared_ptr<MethodGuard> method_guard,
    const ClientContext& client_ctx,
    const json& params
) {
    (void)method_guard; (void)client_ctx; (void)params;

    // 🎯 严格对齐 OpenClaw: skills.bins (server-methods/skills.ts)
    json result;

    auto skill_mgr = server->skill_manager();
    if (!skill_mgr) {
        result["bins"] = json::array();
        return core::Result<json>::Ok(result);
    }

    // Get bin requirements from all skills
    auto snapshot = skill_mgr->get_snapshot();
    json bins_arr = json::array();
    for (const auto& skill : snapshot.skills) {
        for (const auto& bin : skill.required_bins) {
            json b;
            b["name"] = bin.name;
            b["skill"] = skill.name;
            b["found"] = bin.found;
            if (bin.path.has_value()) b["path"] = *bin.path;
            if (bin.version.has_value()) b["version"] = *bin.version;
            bins_arr.push_back(b);
        }
    }
    result["bins"] = bins_arr;
    return core::Result<json>::Ok(result);
}

core::Result<json> method_skills_install(
    std::shared_ptr<GatewayServer> server,
    std::shared_ptr<MethodGuard> method_guard,
    const ClientContext& client_ctx,
    const json& params
) {
    (void)method_guard; (void)client_ctx;

    // 🎯 严格对齐 OpenClaw: skills.install (server-methods/skills.ts)
    std::string skill_name = params.value("skillName", "");
    if (skill_name.empty()) {
        return core::Result<json>::Err("skillName is required");
    }

    std::optional<std::string> install_id;
    if (params.contains("installId") && params["installId"].is_string()) {
        install_id = params["installId"].get<std::string>();
    }

    auto skill_mgr = server->skill_manager();
    if (!skill_mgr) {
        return core::Result<json>::Err("skill manager not available");
    }

    auto install_result = skill_mgr->install_skill(skill_name, install_id);
    if (install_result.is_err()) {
        json result;
        result["ok"] = false;
        result["message"] = install_result.error;
        return core::Result<json>::Ok(result);
    }

    json result;
    result["ok"] = true;
    result["skillName"] = skill_name;
    return core::Result<json>::Ok(result);
}

core::Result<json> method_skills_update(
    std::shared_ptr<GatewayServer> server,
    std::shared_ptr<MethodGuard> method_guard,
    const ClientContext& client_ctx,
    const json& params
) {
    (void)method_guard; (void)client_ctx;

    // 🎯 严格对齐 OpenClaw: skills.update (server-methods/skills.ts)
    // Update skill config entry (apiKey, enabled, etc.)
    std::string skill_name = params.value("skillName", "");
    if (skill_name.empty()) {
        return core::Result<json>::Err("skillName is required");
    }

    // Apply updates to skill entry in config
    auto cfg = server->config();
    if (cfg) {
        // Update config JSON with skill entry changes
        auto& data = cfg->data;
        if (!data.contains("skills")) data["skills"] = json::object();
        if (!data["skills"].contains("entries")) data["skills"]["entries"] = json::object();

        json entry = data["skills"]["entries"].value(skill_name, json::object());
        if (params.contains("apiKey")) {
            entry["apiKey"] = params["apiKey"];
        }
        if (params.contains("enabled")) {
            entry["enabled"] = params["enabled"];
        }
        data["skills"]["entries"][skill_name] = entry;
    }

    // Bump skill snapshot version to trigger refresh
    auto skill_mgr = server->skill_manager();
    if (skill_mgr) {
        skill_mgr->bump_snapshot_version();
    }

    json result;
    result["ok"] = true;
    result["skillName"] = skill_name;
    return core::Result<json>::Ok(result);
}

// ============================================================================
// Exec Approval Methods → 保留在 approval.cc 中
// 这里只需要确保不重复定义
// ============================================================================

}  // namespace methods
}  // namespace gateway
}  // namespace hunterclaw
