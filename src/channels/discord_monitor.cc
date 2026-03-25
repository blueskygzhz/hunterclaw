// Aligned with openclaw: src/discord/monitor/provider.lifecycle.ts
#include "hunterclaw/channels/discord_monitor.h"
#include <chrono>
#include <thread>
#include <spdlog/spdlog.h>
#include <spdlog/sinks/stdout_sinks.h>

namespace hunterclaw {
namespace channels {

namespace {

std::shared_ptr<spdlog::logger> get_fallback_logger() {
    auto log = spdlog::get("discord_monitor");
    if (!log) log = spdlog::stdout_logger_mt("discord_monitor");
    return log;
}

int64_t now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

}  // namespace

// ============================================================================
// dispatch_discord_event
// Aligned with openclaw: client.listeners → listener.handle(data, client)
// ============================================================================
void dispatch_discord_event(
    const std::vector<std::shared_ptr<DiscordListenerBase>>& listeners,
    const std::string& event_name,
    const nlohmann::json& data,
    const std::string& account_id)
{
    for (const auto& listener : listeners) {
        if (!listener) continue;
        try {
            listener->handle(event_name, data, account_id);
        } catch (const std::exception& e) {
            get_fallback_logger()->error(
                "[discord_monitor] listener dispatch error event={} err={}", event_name, e.what());
        } catch (...) {
            get_fallback_logger()->error(
                "[discord_monitor] listener dispatch unknown error event={}", event_name);
        }
    }
}

// ============================================================================
// run_discord_gateway_lifecycle
// Aligned with openclaw: runDiscordGatewayLifecycle (provider.lifecycle.ts)
// ============================================================================
void run_discord_gateway_lifecycle(DiscordGatewayLifecycleParams params) {
    const auto& log = params.logger ? params.logger : get_fallback_logger();

    // Aligned with openclaw: constants
    constexpr int HELLO_TIMEOUT_MS              = 30'000;
    constexpr int HELLO_CONNECTED_POLL_MS       = 250;
    constexpr int MAX_CONSECUTIVE_HELLO_STALLS  = 3;
    constexpr int64_t RECONNECT_STALL_TIMEOUT_MS = 5 * 60'000;

    auto push_status = [&](bool connected,
                           std::optional<int64_t> last_connected_at = std::nullopt,
                           std::optional<int64_t> last_event_at = std::nullopt,
                           std::optional<nlohmann::json> last_disconnect = std::nullopt,
                           std::optional<std::string> last_error = std::nullopt) {
        if (params.set_status) {
            params.set_status(connected, last_connected_at, last_event_at,
                              last_disconnect, last_error);
        }
    };

    // Aligned with openclaw: lifecycle state
    bool lifecycle_stopping = false;
    int consecutive_hello_stalls = 0;
    int reconnect_attempts = 0;

    // Aligned with openclaw: reconnect stall watchdog
    std::atomic<bool> stall_watchdog_armed{false};
    std::atomic<int64_t> stall_watchdog_armed_at{0};

    // Aligned with openclaw: hello timeout state
    std::atomic<bool> hello_timeout_active{false};
    std::atomic<int64_t> hello_started_at{0};
    std::atomic<bool> saw_connected_after_hello{false};

    // Shared gateway client (recreated on each reconnect)
    std::shared_ptr<DiscordGatewayClient> gateway;
    std::atomic<bool> gateway_connected{false};
    std::atomic<bool> reconnect_requested{false};
    std::atomic<bool> fatal_error{false};
    std::string fatal_error_msg;
    std::mutex fatal_mutex;

    // Aligned with openclaw: isDisallowedIntentsError
    auto is_disallowed = [&](const std::string& err) -> bool {
        if (params.is_disallowed_intents_error) {
            return params.is_disallowed_intents_error(err);
        }
        return err.find("4014") != std::string::npos;
    };

    // Aligned with openclaw: onAbort
    auto on_abort = [&]() {
        lifecycle_stopping = true;
        stall_watchdog_armed.store(false);
        const int64_t at = now_ms();
        push_status(false, std::nullopt, at);
        if (gateway) {
            gateway->set_max_reconnect_attempts(0);
            gateway->disconnect();
        }
    };

    // ── Main reconnect loop ──────────────────────────────────────────────────
    // Aligned with openclaw: waitForDiscordGatewayStop → reconnect loop inside GatewayPlugin
    while (!lifecycle_stopping && !params.abort_signal->load()) {
        const int max_attempts = gateway ? gateway->is_connected() ? 50 : 50 : 50;
        if (reconnect_attempts > 0 && reconnect_attempts >= max_attempts) {
            log->error("[{}] discord: max reconnect attempts ({}) reached",
                       params.account_id, max_attempts);
            std::lock_guard<std::mutex> lock(fatal_mutex);
            fatal_error.store(true);
            fatal_error_msg = "Max reconnect attempts reached";
            break;
        }

        // Aligned with openclaw: createDiscordGatewayPlugin → new GatewayPlugin
        const bool should_resume = (reconnect_attempts > 0);
        DiscordGatewayClientOptions gw_opts;
        gw_opts.token      = params.token;
        gw_opts.account_id = params.account_id;
        gw_opts.intents    = params.intents;
        gw_opts.logger     = params.logger;
        gw_opts.abort_signal = params.abort_signal;
        gw_opts.max_reconnect_attempts = 0;  // We manage reconnect ourselves

        // Aligned with openclaw: gatewayEmitter.on("debug", onGatewayDebug)
        gw_opts.on_debug = [&](const std::string& msg) {
            log->debug("[{}] discord gateway: {}", params.account_id, msg);
            const int64_t at = now_ms();
            push_status(gateway_connected.load(), std::nullopt, at);

            // Aligned with openclaw: "WebSocket connection closed" → arm stall watchdog
            if (msg.find("WebSocket connection closed") != std::string::npos) {
                if (gateway_connected.load()) {
                    consecutive_hello_stalls = 0;
                }
                stall_watchdog_armed.store(true);
                stall_watchdog_armed_at.store(at);
                push_status(false, std::nullopt, at,
                    nlohmann::json{{"at", at}});
                hello_timeout_active.store(false);
                return;
            }

            // Aligned with openclaw: "WebSocket connection opened" → start hello timeout
            if (msg.find("WebSocket connection opened") != std::string::npos) {
                stall_watchdog_armed.store(false);
                hello_timeout_active.store(true);
                hello_started_at.store(at);
                saw_connected_after_hello.store(false);
            }
        };

        // Aligned with openclaw: gateway.isConnected → on_connected (READY/RESUMED)
        gw_opts.on_connected = [&]() {
            gateway_connected.store(true);
            consecutive_hello_stalls = 0;
            stall_watchdog_armed.store(false);
            hello_timeout_active.store(false);
            saw_connected_after_hello.store(true);
            const int64_t at = now_ms();
            push_status(true, at, at, nlohmann::json(nullptr));
            log->info("[{}] discord: gateway connected (READY/RESUMED)", params.account_id);
        };

        // Aligned with openclaw: "WebSocket connection closed" → reconnect
        gw_opts.on_disconnected = [&](uint16_t code, const std::string& reason) {
            gateway_connected.store(false);
            reconnect_requested.store(true);
            const int64_t at = now_ms();
            push_status(false, std::nullopt, at,
                nlohmann::json{{"at", at}, {"status", code}, {"reason", reason}});
            log->info("[{}] discord: gateway disconnected code={} reason={}",
                      params.account_id, code, reason);
        };

        // Aligned with openclaw: gateway error
        gw_opts.on_error = [&](const std::string& err) {
            log->error("[{}] discord gateway error: {}", params.account_id, err);
            if (is_disallowed(err)) {
                log->error("[{}] discord: gateway closed with code 4014 (missing privileged "
                           "gateway intents). Enable the required intents in the Discord "
                           "Developer Portal or disable them in config.", params.account_id);
                std::lock_guard<std::mutex> lock(fatal_mutex);
                fatal_error.store(true);
                fatal_error_msg = err;
            }
        };

        // Aligned with openclaw: DISPATCH → dispatch to listeners
        gw_opts.on_dispatch = [&](const std::string& event_name, const nlohmann::json& data) {
            dispatch_discord_event(params.listeners, event_name, data, params.account_id);
        };

        gateway = std::make_shared<DiscordGatewayClient>(gw_opts);
        reconnect_requested.store(false);
        gateway_connected.store(false);
        gateway->connect(should_resume);

        // Aligned with openclaw: wait for connected/disconnected/abort
        // Poll loop: check abort, stall watchdog, hello timeout
        while (!params.abort_signal->load() && !reconnect_requested.load()
               && !fatal_error.load() && !lifecycle_stopping)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(200));

            // Aligned with openclaw: reconnect stall watchdog (5min)
            if (stall_watchdog_armed.load()) {
                const int64_t elapsed = now_ms() - stall_watchdog_armed_at.load();
                if (elapsed >= RECONNECT_STALL_TIMEOUT_MS) {
                    log->error("[{}] discord: reconnect watchdog timeout after {}ms; "
                               "force-stopping monitor task",
                               params.account_id, RECONNECT_STALL_TIMEOUT_MS);
                    const int64_t at = now_ms();
                    push_status(false, std::nullopt, at, std::nullopt,
                                "reconnect watchdog timeout");
                    std::lock_guard<std::mutex> lock(fatal_mutex);
                    fatal_error.store(true);
                    fatal_error_msg = "reconnect watchdog timeout";
                    break;
                }
            }

            // Aligned with openclaw: hello timeout (30s)
            if (hello_timeout_active.load()) {
                const int64_t elapsed = now_ms() - hello_started_at.load();
                if (elapsed >= HELLO_TIMEOUT_MS) {
                    hello_timeout_active.store(false);
                    if (!saw_connected_after_hello.load() && !gateway_connected.load()) {
                        consecutive_hello_stalls++;
                        const bool force_fresh = (consecutive_hello_stalls >= MAX_CONSECUTIVE_HELLO_STALLS);
                        const int64_t at = now_ms();
                        stall_watchdog_armed.store(true);
                        stall_watchdog_armed_at.store(at);
                        push_status(false, std::nullopt, at,
                            nlohmann::json{{"at", at}, {"error", "hello-timeout"}});
                        log->warn("[{}] discord: connection stalled: no HELLO within {}ms "
                                  "({}/{}); {}",
                                  params.account_id, HELLO_TIMEOUT_MS,
                                  consecutive_hello_stalls, MAX_CONSECUTIVE_HELLO_STALLS,
                                  force_fresh ? "forcing fresh identify" : "retrying resume");
                        if (force_fresh) {
                            gateway->clear_resume_state();
                            consecutive_hello_stalls = 0;
                        }
                        reconnect_requested.store(true);
                    } else {
                        consecutive_hello_stalls = 0;
                    }
                }
            }
        }

        // Aligned with openclaw: onAbort
        if (params.abort_signal->load() && !lifecycle_stopping) {
            on_abort();
            break;
        }

        if (fatal_error.load()) break;

        // Aligned with openclaw: reconnect with backoff
        if (reconnect_requested.load() && !lifecycle_stopping) {
            reconnect_attempts++;
            gateway->disconnect();
            // Aligned with openclaw: GatewayPlugin reconnect backoff
            const int backoff_ms = std::min(1000 * reconnect_attempts, 30'000);
            log->info("[{}] discord: reconnecting in {}ms (attempt {})",
                      params.account_id, backoff_ms, reconnect_attempts);
            for (int i = 0; i < backoff_ms / 100 && !params.abort_signal->load(); ++i) {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
        }
    }

    // Aligned with openclaw: finally → cleanup
    if (gateway) {
        gateway->disconnect();
    }
    // Aligned with openclaw: finally → opts.setStatus?.({ connected: false })
    push_status(false);
}

}  // namespace channels
}  // namespace hunterclaw
