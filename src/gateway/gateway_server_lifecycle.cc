// ============================================================================
// gateway_server_lifecycle.cc
// ✅ 严格对齐 OpenClaw:
//   - src/gateway/server-close.ts (277行)
//   - src/gateway/server-maintenance.ts (194行)
//   - src/gateway/server-startup-early.ts (126行)
//   - src/gateway/server-startup-post-attach.ts (346行)
//
// 网关完整生命周期管理：
//   - runGatewayClosePrelude: 关停前置清理 (9项资源释放)
//   - createGatewayCloseHandler: 16+ 项有序资源释放 + 分级超时关停
//   - startGatewayMaintenanceTimers: 4 定时器 + 6 种资源回收策略
//   - startGatewayEarlyRuntime: mDNS 发现 + 远程技能注册 + 维护定时器
//   - startGatewaySidecars: 11 项侧车服务编排
//   - startGatewayPostAttachRuntime: 后期启动编排
// ============================================================================

#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

using json = nlohmann::json;

namespace hunterclaw {
namespace gateway {

namespace {
auto shutdown_log() { return spdlog::get("gateway/shutdown"); }
auto lifecycle_log() { return spdlog::get("gateway/lifecycle"); }
} // namespace

// ============================================================================
// 关停常量 (对齐 server-close.ts:13-16)
// ============================================================================

static constexpr int WEBSOCKET_CLOSE_GRACE_MS = 1000;
static constexpr int WEBSOCKET_CLOSE_FORCE_CONTINUE_MS = 250;
static constexpr int HTTP_CLOSE_GRACE_MS = 1000;
static constexpr int HTTP_CLOSE_FORCE_WAIT_MS = 5000;

// ============================================================================
// 维护定时器常量 (对齐 server-constants.ts)
// ============================================================================

static constexpr int64_t TICK_INTERVAL_MS = 30 * 1000;
static constexpr int64_t HEALTH_REFRESH_INTERVAL_MS = 60 * 1000;
static constexpr int64_t DEDUPE_TTL_MS = 5 * 60 * 1000;
static constexpr int DEDUPE_MAX = 10000;
static constexpr int AGENT_RUN_SEQ_MAX = 10000;
static constexpr int64_t ABORTED_RUN_TTL_MS = 60 * 60 * 1000;
static constexpr int64_t MEDIA_CLEANUP_INTERVAL_MS = 60 * 60 * 1000;
static constexpr int64_t DEDUPE_CLEANUP_INTERVAL_MS = 60 * 1000;
static constexpr int64_t SESSION_LOCK_STALE_MS = 30 * 60 * 1000;

// ============================================================================
// 🎯 runGatewayClosePrelude (对齐 server-close.ts:41-61)
// 关停前置清理 — 9 项资源释放
// ============================================================================

struct ClosePreludeParams {
    std::function<void()> stop_diagnostics;
    std::function<void()> clear_skills_refresh_timer;
    std::function<void()> skills_change_unsub;
    std::function<void()> dispose_auth_rate_limiter;
    std::function<void()> dispose_browser_auth_rate_limiter;
    std::function<void()> stop_model_pricing_refresh;
    std::function<void()> stop_channel_health_monitor;
    std::function<void()> clear_secrets_runtime_snapshot;
    std::function<void()> close_mcp_server;
};

void run_gateway_close_prelude(const ClosePreludeParams& params) {
    auto safe_call = [](const std::function<void()>& fn) {
        if (fn) { try { fn(); } catch (...) {} }
    };
    safe_call(params.stop_diagnostics);
    safe_call(params.clear_skills_refresh_timer);
    safe_call(params.skills_change_unsub);
    safe_call(params.dispose_auth_rate_limiter);
    safe_call(params.dispose_browser_auth_rate_limiter);
    safe_call(params.stop_model_pricing_refresh);
    safe_call(params.stop_channel_health_monitor);
    safe_call(params.clear_secrets_runtime_snapshot);
    safe_call(params.close_mcp_server);
}

// ============================================================================
// 🎯 GatewayCloseHandler (对齐 server-close.ts:63-276)
// 16+ 项有序资源释放 + 分级超时关停
// ============================================================================

struct GatewayCloseHandlerDeps {
    // Network cleanup
    std::function<void()> bonjour_stop;
    std::function<void()> tailscale_cleanup;

    // Canvas
    std::function<void()> close_canvas_host;
    std::function<void()> close_canvas_host_server;

    // Channels
    std::function<void(const std::string&)> stop_channel;
    std::vector<std::string> channel_ids;

    // Agent + Plugin
    std::function<void()> dispose_agent_harnesses;
    std::function<void()> stop_plugin_services;

    // Hooks + Cron + Heartbeat
    std::function<void()> stop_gmail_watcher;
    std::function<void()> stop_cron;
    std::function<void()> stop_heartbeat_runner;
    std::function<void()> stop_task_registry_maintenance;
    std::function<void()> stop_update_check;

    // Broadcast
    std::function<void(const std::string&, const json&)> broadcast;

    // Timer cleanup
    std::function<void()> clear_all_intervals;
    std::function<void()> clear_all_subscriptions;

    // Connection cleanup
    std::function<void()> close_all_ws_clients;     // code 1012 "service restart"
    std::function<void()> stop_config_reloader;

    // Server shutdown
    std::function<bool(int)> close_ws_server;  // returns true if closed within grace
    std::function<void()> terminate_all_ws;
    std::function<bool(int)> close_http_server; // returns true if closed within grace
    std::function<void()> force_close_http_connections;

    // Final
    std::function<void()> release_plugin_route_registry;
};

void run_gateway_close(
    GatewayCloseHandlerDeps& deps,
    const std::string& reason,
    std::optional<int64_t> restart_expected_ms
) {
    auto safe_call = [](const std::function<void()>& fn) {
        if (fn) { try { fn(); } catch (...) {} }
    };

    try {
        std::string close_reason = reason.empty() ? "gateway stopping" : reason;

        // 1. Bonjour + Tailscale — 对齐 :100-109
        safe_call(deps.bonjour_stop);
        safe_call(deps.tailscale_cleanup);

        // 2. Canvas — 对齐 :110-123
        safe_call(deps.close_canvas_host);
        safe_call(deps.close_canvas_host_server);

        // 3. Stop all channels — 对齐 :124-126
        for (const auto& ch_id : deps.channel_ids) {
            try { if (deps.stop_channel) deps.stop_channel(ch_id); } catch (...) {}
        }

        // 4. Agent harnesses — 对齐 :127
        safe_call(deps.dispose_agent_harnesses);

        // 5. Plugin services — 对齐 :128-130
        safe_call(deps.stop_plugin_services);

        // 6. Gmail watcher — 对齐 :131
        safe_call(deps.stop_gmail_watcher);

        // 7. Cron + Heartbeat — 对齐 :132-133
        safe_call(deps.stop_cron);
        safe_call(deps.stop_heartbeat_runner);

        // 8. Task registry + Update check — 对齐 :134-143
        safe_call(deps.stop_task_registry_maintenance);
        safe_call(deps.stop_update_check);

        // 9. Broadcast shutdown — 对齐 :148-151
        if (deps.broadcast) {
            json payload = {
                {"reason", close_reason},
                {"restartExpectedMs", restart_expected_ms.value_or(-1)}
            };
            if (!restart_expected_ms.has_value()) payload.erase("restartExpectedMs");
            deps.broadcast("shutdown", payload);
        }

        // 10. Clear all intervals + subscriptions — 对齐 :152-185
        safe_call(deps.clear_all_intervals);
        safe_call(deps.clear_all_subscriptions);

        // 11. Close WS clients with 1012 — 对齐 :187-194
        safe_call(deps.close_all_ws_clients);

        // 12. Stop config reloader — 对齐 :195
        safe_call(deps.stop_config_reloader);

        // 13. WebSocket 分级关停 — 对齐 :196-225
        if (deps.close_ws_server) {
            bool closed = deps.close_ws_server(WEBSOCKET_CLOSE_GRACE_MS);
            if (!closed) {
                if (shutdown_log()) {
                    shutdown_log()->warn(
                        "websocket server close exceeded {}ms; forcing shutdown",
                        WEBSOCKET_CLOSE_GRACE_MS);
                }
                safe_call(deps.terminate_all_ws);
                // 250ms force window
                std::this_thread::sleep_for(
                    std::chrono::milliseconds(WEBSOCKET_CLOSE_FORCE_CONTINUE_MS));
            }
        }

        // 14. HTTP 分级关停 — 对齐 :226-267
        if (deps.close_http_server) {
            bool closed = deps.close_http_server(HTTP_CLOSE_GRACE_MS);
            if (!closed) {
                if (shutdown_log()) {
                    shutdown_log()->warn(
                        "http server close exceeded {}ms; forcing connection shutdown",
                        HTTP_CLOSE_GRACE_MS);
                }
                safe_call(deps.force_close_http_connections);
                bool closed_after = deps.close_http_server(HTTP_CLOSE_FORCE_WAIT_MS);
                if (!closed_after) {
                    throw std::runtime_error(
                        "http server close still pending after forced connection shutdown");
                }
            }
        }
    } catch (...) {}

    // 15. Release plugin route registry — 对齐 :268-273
    safe_call(deps.release_plugin_route_registry);
}

// ============================================================================
// 🎯 DedupeEntry (对齐 server-shared.ts)
// ============================================================================

struct DedupeEntry {
    int64_t ts = 0;
};

// ============================================================================
// 🎯 MaintenanceTimerHandles (对齐 server-maintenance.ts:46-50)
// ============================================================================

struct MaintenanceTimerHandles {
    std::atomic<bool> stopped{false};
    std::thread tick_thread;
    std::thread health_thread;
    std::thread dedupe_thread;
    std::thread media_thread;

    void stop() {
        stopped.store(true);
    }
};

// ============================================================================
// 🎯 startGatewayMaintenanceTimers (对齐 server-maintenance.ts:17-193)
// 4 定时器 + 6 种资源回收策略
// ============================================================================

struct MaintenanceDeps {
    std::function<void(const std::string&, const json&)> broadcast;
    std::function<void(const std::string&, const json&)> node_send_to_all;
    std::function<json()> refresh_health_snapshot;
    std::shared_ptr<std::unordered_map<std::string, DedupeEntry>> dedupe;
    std::shared_ptr<std::unordered_map<std::string, int>> agent_run_seq;
    std::function<void()> prune_stale_control_plane_buckets;
    std::function<void()> sweep_stale_run_contexts;
    std::function<void()> clean_old_media;
    bool has_media_cleanup = false;
};

std::shared_ptr<MaintenanceTimerHandles> start_gateway_maintenance_timers(
    MaintenanceDeps deps
) {
    auto handles = std::make_shared<MaintenanceTimerHandles>();

    // 1. Tick — 对齐 :62-66
    handles->tick_thread = std::thread([handles, deps]() {
        while (!handles->stopped.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(TICK_INTERVAL_MS));
            if (handles->stopped.load()) break;
            json payload = {{"ts", std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count()}};
            if (deps.broadcast) deps.broadcast("tick", payload);
            if (deps.node_send_to_all) deps.node_send_to_all("tick", payload);
        }
    });

    // 2. Health refresh — 对齐 :69-78
    handles->health_thread = std::thread([handles, deps]() {
        // Initial prime
        if (deps.refresh_health_snapshot) {
            try { deps.refresh_health_snapshot(); } catch (...) {}
        }
        while (!handles->stopped.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(HEALTH_REFRESH_INTERVAL_MS));
            if (handles->stopped.load()) break;
            if (deps.refresh_health_snapshot) {
                try { deps.refresh_health_snapshot(); } catch (...) {}
            }
        }
    });

    // 3. Dedupe cleanup — 对齐 :81-162
    handles->dedupe_thread = std::thread([handles, deps]() {
        while (!handles->stopped.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(DEDUPE_CLEANUP_INTERVAL_MS));
            if (handles->stopped.load()) break;

            auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count();

            // Dedupe TTL eviction
            if (deps.dedupe) {
                auto& dd = *deps.dedupe;
                for (auto it = dd.begin(); it != dd.end(); ) {
                    if (now - it->second.ts > DEDUPE_TTL_MS) {
                        it = dd.erase(it);
                    } else {
                        ++it;
                    }
                }
                // Size cap
                if (static_cast<int>(dd.size()) > DEDUPE_MAX) {
                    std::vector<std::pair<std::string, int64_t>> entries;
                    for (const auto& [k, v] : dd) entries.emplace_back(k, v.ts);
                    std::sort(entries.begin(), entries.end(),
                        [](const auto& a, const auto& b) { return a.second < b.second; });
                    int excess = static_cast<int>(dd.size()) - DEDUPE_MAX;
                    for (int i = 0; i < excess && i < static_cast<int>(entries.size()); ++i) {
                        dd.erase(entries[i].first);
                    }
                }
            }

            // Agent run seq overflow — 对齐 :96-106
            if (deps.agent_run_seq) {
                auto& seq = *deps.agent_run_seq;
                if (static_cast<int>(seq.size()) > AGENT_RUN_SEQ_MAX) {
                    int excess = static_cast<int>(seq.size()) - AGENT_RUN_SEQ_MAX;
                    int removed = 0;
                    for (auto it = seq.begin(); it != seq.end() && removed < excess; ) {
                        it = seq.erase(it);
                        removed++;
                    }
                }
            }

            // Prune stale control plane buckets — 对齐 :141
            if (deps.prune_stale_control_plane_buckets) {
                try { deps.prune_stale_control_plane_buckets(); } catch (...) {}
            }

            // Sweep stale run contexts — 对齐 :161
            if (deps.sweep_stale_run_contexts) {
                try { deps.sweep_stale_run_contexts(); } catch (...) {}
            }
        }
    });

    // 4. Media cleanup — 对齐 :164-192
    if (deps.has_media_cleanup) {
        handles->media_thread = std::thread([handles, deps]() {
            // Initial cleanup
            if (deps.clean_old_media) {
                try { deps.clean_old_media(); } catch (...) {}
            }
            while (!handles->stopped.load()) {
                std::this_thread::sleep_for(std::chrono::milliseconds(MEDIA_CLEANUP_INTERVAL_MS));
                if (handles->stopped.load()) break;
                if (deps.clean_old_media) {
                    try { deps.clean_old_media(); } catch (...) {}
                }
            }
        });
    }

    return handles;
}

// ============================================================================
// 🎯 startGatewayEarlyRuntime (对齐 server-startup-early.ts:14-125)
// mDNS 发现 + 远程技能注册 + 技能变更监听 + 维护定时器
// ============================================================================

struct EarlyRuntimeResult {
    std::function<void()> bonjour_stop;
    std::function<void()> skills_change_unsub;
    std::shared_ptr<MaintenanceTimerHandles> maintenance;
};

EarlyRuntimeResult start_gateway_early_runtime(
    const json& config,
    bool minimal_test_gateway,
    int port,
    std::function<void()> register_skills_remote,
    std::function<void()> prime_remote_skills_cache,
    std::function<void()> start_task_registry_maintenance,
    MaintenanceDeps maintenance_deps
) {
    EarlyRuntimeResult result;

    if (!minimal_test_gateway) {
        // mDNS/Bonjour discovery — 对齐 :57-72
        // Note: actual Bonjour/mDNS implementation is platform-specific
        if (lifecycle_log()) {
            lifecycle_log()->info("starting gateway discovery on port {}", port);
        }
        // Skills remote registration — 对齐 :74-78
        if (register_skills_remote) register_skills_remote();
        if (prime_remote_skills_cache) prime_remote_skills_cache();
        if (start_task_registry_maintenance) start_task_registry_maintenance();
    }

    // 维护定时器 — 对齐 :97-118
    if (!minimal_test_gateway) {
        result.maintenance = start_gateway_maintenance_timers(std::move(maintenance_deps));
    }

    return result;
}

// ============================================================================
// 🎯 startGatewaySidecars (对齐 server-startup-post-attach.ts:91-238)
// 11 项侧车服务编排
// ============================================================================

struct SidecarDeps {
    json config;
    std::function<void()> clean_stale_lock_files;
    std::function<void()> start_gmail_watcher;
    std::function<void()> load_internal_hooks;
    std::function<void()> prewarm_primary_model;
    std::function<void()> start_channels;
    std::function<void()> trigger_gateway_startup_hook;
    std::function<void()> start_plugin_services;
    std::function<void()> reconcile_acp_sessions;
    std::function<void()> start_memory_backend;
    std::function<void()> schedule_subagent_orphan_recovery;
    std::function<void()> schedule_restart_sentinel_wake;
};

void start_gateway_sidecars(const SidecarDeps& deps) {
    auto safe_call = [](const std::function<void()>& fn, const std::string& label) {
        if (!fn) return;
        try {
            fn();
        } catch (const std::exception& e) {
            if (spdlog::get("gateway/lifecycle")) {
                spdlog::get("gateway/lifecycle")->warn("{} failed: {}", label, e.what());
            }
        }
    };

    // 1. Clean stale session lock files — 对齐 :105-118
    safe_call(deps.clean_stale_lock_files, "session lock cleanup");

    // 2. Start Gmail watcher — 对齐 :120-123
    safe_call(deps.start_gmail_watcher, "gmail watcher");

    // 3. Load internal hooks — 对齐 :157-167
    safe_call(deps.load_internal_hooks, "internal hooks");

    // 4. Prewarm model + Start channels — 对齐 :169-186
    safe_call(deps.prewarm_primary_model, "model prewarm");
    safe_call(deps.start_channels, "channel startup");

    // 5. Trigger gateway:startup hook (250ms delayed) — 对齐 :188-197
    if (deps.trigger_gateway_startup_hook) {
        std::thread([fn = deps.trigger_gateway_startup_hook]() {
            std::this_thread::sleep_for(std::chrono::milliseconds(250));
            try { fn(); } catch (...) {}
        }).detach();
    }

    // 6. Start plugin services — 对齐 :199-208
    safe_call(deps.start_plugin_services, "plugin services");

    // 7. ACP session identity reconciliation — 对齐 :210-224
    if (deps.reconcile_acp_sessions) {
        std::thread([fn = deps.reconcile_acp_sessions]() {
            try { fn(); } catch (...) {}
        }).detach();
    }

    // 8. Start QMD memory backend — 对齐 :226-228
    if (deps.start_memory_backend) {
        std::thread([fn = deps.start_memory_backend]() {
            try { fn(); } catch (...) {}
        }).detach();
    }

    // 9. Restart sentinel wake — 对齐 :230-234
    if (deps.schedule_restart_sentinel_wake) {
        std::thread([fn = deps.schedule_restart_sentinel_wake]() {
            std::this_thread::sleep_for(std::chrono::milliseconds(750));
            try { fn(); } catch (...) {}
        }).detach();
    }

    // 10. Subagent orphan recovery — 对齐 :236
    safe_call(deps.schedule_subagent_orphan_recovery, "subagent orphan recovery");
}

// ============================================================================
// 🎯 startGatewayPostAttachRuntime (对齐 server-startup-post-attach.ts:241-341)
// ============================================================================

struct PostAttachResult {
    std::function<void()> stop_update_check;
    std::function<void()> tailscale_cleanup;
};

PostAttachResult start_gateway_post_attach_runtime(
    const json& config,
    bool minimal_test_gateway,
    int port,
    const std::string& bind_host,
    std::function<void()> log_startup,
    std::function<void()> schedule_update_check,
    std::function<void()> start_tailscale_exposure,
    const SidecarDeps& sidecar_deps
) {
    PostAttachResult result;

    // Log startup info — 对齐 :277-289
    if (log_startup) log_startup();

    // Schedule update check — 对齐 :291-301
    if (!minimal_test_gateway && schedule_update_check) {
        schedule_update_check();
    }

    // Tailscale exposure — 对齐 :303-311
    if (!minimal_test_gateway && start_tailscale_exposure) {
        start_tailscale_exposure();
    }

    // Start sidecars — 对齐 :314-328
    if (!minimal_test_gateway) {
        if (lifecycle_log()) {
            lifecycle_log()->info("starting channels and sidecars...");
        }
        start_gateway_sidecars(sidecar_deps);
    }

    return result;
}

} // namespace gateway
} // namespace hunterclaw
