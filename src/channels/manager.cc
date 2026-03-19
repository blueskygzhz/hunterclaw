#include "hunterclaw/channels/manager.h"
#include "hunterclaw/channels/registry.h"
#include "hunterclaw/core/utils.h"
#include "hunterclaw/auto_reply/message_dispatcher.h"
#include <thread>
#include <chrono>
#include <random>
#include <spdlog/spdlog.h>

namespace hunterclaw {
namespace channels {

namespace {
    auto log() {
        auto logger = spdlog::get("channels");
        if (!logger) {
            logger = spdlog::stdout_logger_mt("channels");
        }
        return logger;
    }
    
    // ✅ 健康评估结果 (对齐 OpenClaw: ChannelHealth)
    struct ChannelHealthResult {
        bool healthy;
        std::string reason;
    };
    
    // ✅ 重启事件记录 (对齐 OpenClaw: RestartEvent)
    struct RestartEvent {
        int64_t at;  // 重启时间戳 (ms)
    };
    
    // ✅ 重启记录 (对齐 OpenClaw: RestartRecord)
    struct RestartRecord {
        int64_t last_restart_at;
        std::vector<RestartEvent> restarts_this_hour;
    };
    
    // ✅ 健康评估函数 (100% 对齐 OpenClaw: evaluateChannelHealth)
    ChannelHealthResult evaluate_channel_health(
        const ChannelAccountSnapshot& runtime,
        ChannelId channel_id,
        int64_t now_ms,
        const ChannelHealthMonitorConfig& config
    ) {
        // 1. 未启用 → 健康 (跳过)
        if (runtime.enabled.has_value() && !runtime.enabled.value()) {
            return {true, "disabled"};
        }
        
        // 2. 未配置 → 健康 (跳过)
        if (runtime.configured.has_value() && !runtime.configured.value()) {
            return {true, "not configured"};
        }
        
        // 3. 运行中但未连接 → 检查连接宽限期 (对齐 channelConnectGraceMs)
        bool is_running   = runtime.running.value_or(false);
        bool is_connected = runtime.connected.value_or(false);
        if (is_running && !is_connected) {
            if (runtime.last_start_at.has_value()) {
                int64_t elapsed = now_ms - runtime.last_start_at.value();
                if (elapsed < config.timing.channel_connect_grace_ms) {
                    return {true, "connecting"};  // ✅ 在宽限期内,允许启动
                }
            }
            return {false, "disconnected"};  // ❌ 超过宽限期仍未连接
        }
        
        // 4. 已连接但消息过期 → 不健康 (对齐 staleEventThresholdMs)
        if (is_connected && runtime.last_message_at.has_value()) {
            int64_t idle = now_ms - runtime.last_message_at.value();
            if (idle > config.timing.stale_event_threshold_ms) {
                return {false, "stale"};  // ❌ 消息过期,需要重启
            }
        }
        
        return {true, "ok"};  // ✅ 一切正常
    }
}

// ============================================================================
// ChannelManager Implementation (100% 对齐 OpenClaw)
// ============================================================================

class ChannelManagerImpl {
public:
    explicit ChannelManagerImpl(const ChannelManager::Options& options)
        : options_(options) {
        restart_policy_.initial_ms = 5000;
        restart_policy_.max_ms = 5 * 60 * 1000;
        restart_policy_.factor = 2.0;
        restart_policy_.jitter = 0.1;
    }
    
    ~ChannelManagerImpl() {
        stop_health_monitor();
    }
    
    // ========== Channel Lifecycle ==========
    
    core::Result<void> start_channel(
        ChannelId channel_id,
        const std::string& account_id,
        const StartChannelOptions& options
    ) {
        std::string effective_account_id = account_id.empty() ? "default" : account_id;
        std::string key = std::string(channel_id) + ":" + effective_account_id;
        
        // Check if manually stopped
        {
            std::lock_guard<std::mutex> lock(manually_stopped_mutex_);
            if (manually_stopped_.find(key) != manually_stopped_.end() && !options.force) {
                return core::Result<void>::Err("Channel manually stopped, use force=true to override");
            }
        }
        
        // Get or create channel store
        auto& store = get_store(channel_id);
        
        // ✅ 从 Registry 加载 ChannelPlugin (对齐 OpenClaw: getChannelPlugin())
        if (!store.plugin.has_value()) {
            auto plugin_opt = ChannelPluginRegistry::instance().get_plugin(std::string(channel_id));
            if (!plugin_opt.has_value()) {
                return core::Result<void>::Err("Channel plugin not found: " + std::string(channel_id));
            }
            store.plugin = plugin_opt;
            log()->info("Loaded channel plugin: {}", channel_id);
        }
        
        // Check if already running
        {
            std::lock_guard<std::mutex> lock(stores_mutex_);
            auto runtime = get_runtime(channel_id, effective_account_id);
            if (runtime.running.value_or(false) && !options.force) {
                return core::Result<void>::Err("Channel already running");
            }
        }
        
        // Clear manual stop flag
        {
            std::lock_guard<std::mutex> lock(manually_stopped_mutex_);
            manually_stopped_.erase(key);
        }
        
        // Reset restart attempts if requested
        if (!options.preserve_restart_attempts) {
            reset_restart_attempts(channel_id, effective_account_id);
        }
        
        // Update status (Phase 1: Starting)
        ChannelAccountSnapshot snapshot;
        snapshot.account_id = effective_account_id;
        snapshot.enabled    = true;
        snapshot.configured = true;
        snapshot.running    = true;
        snapshot.connected  = false;  // ← 启动中,尚未连接
        snapshot.last_start_at = core::utils::now().time_since_epoch().count() / 1000000;
        
        set_runtime(channel_id, effective_account_id, snapshot);
        
        // ✅ 创建 abort signal (对齐 OpenClaw: new AbortController())
        auto abort = std::make_shared<std::atomic<bool>>(false);
        {
            std::lock_guard<std::mutex> lock(stores_mutex_);
            store.aborts[effective_account_id] = abort;
        }
        
        // ✅ 启动 Channel 异步任务 (对齐 OpenClaw: await startAccount(...))
        auto plugin_copy = store.plugin.value();
        std::thread([this, channel_id, effective_account_id, plugin_copy, abort]() {
            try {
                // 构建 ChannelGatewayContext (对齐 OpenClaw ChannelGatewayContext)
                ChannelGatewayContext ctx;
                ctx.account_id    = effective_account_id;
                ctx.account       = nlohmann::json::object();  // TODO: resolve from config
                ctx.abort_signal  = abort;

                // 设置日志回调
                auto log_sink = std::make_shared<ChannelLogSink>();
                log_sink->info  = [channel_id, effective_account_id](const std::string& msg) {
                    ::hunterclaw::channels::log()->info("[{}/{}] {}", channel_id, effective_account_id, msg);
                };
                log_sink->warn  = [channel_id, effective_account_id](const std::string& msg) {
                    ::hunterclaw::channels::log()->warn("[{}/{}] {}", channel_id, effective_account_id, msg);
                };
                log_sink->error = [channel_id, effective_account_id](const std::string& msg) {
                    ::hunterclaw::channels::log()->error("[{}/{}] {}", channel_id, effective_account_id, msg);
                };
                log_sink->debug = [channel_id, effective_account_id](const std::string& msg) {
                    ::hunterclaw::channels::log()->debug("[{}/{}] {}", channel_id, effective_account_id, msg);
                };
                ctx.log = log_sink;

                // get_status / set_status callbacks
                ctx.get_status = [this, channel_id, effective_account_id]() -> ChannelAccountSnapshot {
                    return get_runtime(channel_id, effective_account_id);
                };
                ctx.set_status = [this, channel_id, effective_account_id](const ChannelAccountSnapshot& s) {
                    set_runtime(channel_id, effective_account_id, s);
                    // 更新 last_message_at when connected
                    if (s.connected.value_or(false)) {
                        update_runtime_fields(channel_id, effective_account_id, [](auto& snap) {
                            snap.last_connected_at = core::utils::now().time_since_epoch().count() / 1000000;
                        });
                    }
                };

                // Aligned with openclaw: inject dispatchReplyFromConfig into gateway context.
                // The channel gateway (e.g. telegram webhook handler) calls ctx.dispatch_inbound
                // when an inbound message arrives. Wire to the real MessageDispatcher if available.
                {
                    auto dispatcher_weak = std::weak_ptr<auto_reply::MessageDispatcher>(dispatcher_);
                    ctx.dispatch_inbound = [channel_id, effective_account_id, dispatcher_weak](
                        const channels::InboundMessage& msg
                    ) {
                        auto dispatcher = dispatcher_weak.lock();
                        if (dispatcher) {
                            // Aligned with openclaw: dispatchReplyFromConfig(inboundMessage)
                            dispatcher->dispatch_inbound_message(msg);
                        } else {
                            ::hunterclaw::channels::log()->warn(
                                "[{}/{}] dispatch_inbound: MessageDispatcher not available, message dropped",
                                channel_id, effective_account_id
                            );
                        }
                    };
                }

                // ✅ 调用 ChannelGatewayAdapter::start_account (对齐 OpenClaw)
                if (!plugin_copy.gateway.has_value() || !plugin_copy.gateway->start_account) {
                    log()->warn("Channel {} has no gateway.start_account", channel_id);
                    return;
                }

                auto result = plugin_copy.gateway->start_account(ctx);

                if (result.is_err()) {
                    log()->error("Channel {} account {} failed to start: {}",
                                 channel_id, effective_account_id, result.error);
                    update_runtime_fields(channel_id, effective_account_id, [&result](auto& s) {
                        s.running   = false;
                        s.connected = false;
                        s.last_error = result.error;
                    });
                } else {
                    log()->info("Channel {} account {} started successfully",
                                channel_id, effective_account_id);
                }
            } catch (const std::exception& e) {
                log()->error("Channel {} account {} exception: {}",
                             channel_id, effective_account_id, e.what());
                update_runtime_fields(channel_id, effective_account_id, [&e](auto& s) {
                    s.running   = false;
                    s.connected = false;
                    s.last_error = std::string(e.what());
                });
            }
        }).detach();
        
        return core::Result<void>::Ok();
    }
    
    core::Result<void> start_channels(const std::vector<ChannelId>& channel_ids) {
        std::vector<ChannelId> to_start;

        if (channel_ids.empty()) {
            // Aligned with openclaw: startChannels() → for (const plugin of listChannelPlugins())
            // Enumerate all registered channel plugins from the registry.
            const auto& all_plugins = ChannelPluginRegistry::instance().list_plugins();
            for (const auto& plugin : all_plugins) {
                to_start.push_back(plugin.id.c_str());
            }
            log()->info("start_channels: starting {} registered channels", to_start.size());
        } else {
            to_start = channel_ids;
        }

        for (const auto& channel_id : to_start) {
            // Aligned with openclaw: await startChannel(plugin.id)
            // listAccountIds from config — for now use "default" account
            // TODO: resolve account IDs from config (plugin.config.listAccountIds(cfg))
            auto result = start_channel(channel_id, "default", {});
            if (result.is_err()) {
                log()->warn("Failed to start channel {}: {}", channel_id, result.error);
                // Aligned with openclaw: continue on per-channel failure (don't abort all)
            }
        }

        return core::Result<void>::Ok();
    }
    
    core::Result<void> stop_channel(ChannelId channel_id, const std::string& account_id) {
        std::string effective_account_id = account_id.empty() ? "default" : account_id;
        std::string key = std::string(channel_id) + ":" + effective_account_id;
        
        // Mark as manually stopped
        {
            std::lock_guard<std::mutex> lock(manually_stopped_mutex_);
            manually_stopped_.insert(key);
        }
        
        // ✅ 原子性更新状态
        update_runtime_fields(channel_id, effective_account_id, [](auto& s) {
            s.running   = false;
            s.connected = false;
            s.last_stop_at = core::utils::now().time_since_epoch().count() / 1000000;
        });

        // Abort any running tasks
        auto& store = get_store(channel_id);
        auto it = store.aborts.find(effective_account_id);
        if (it != store.aborts.end()) {
            it->second->store(true);
        }
        
        log()->info("Channel {} account {} stopped", channel_id, effective_account_id);
        
        return core::Result<void>::Ok();
    }
    
    core::Result<void> mark_channel_logged_out(ChannelId channel_id, const std::string& account_id, bool cleared = true) {
        std::string effective_account_id = account_id.empty() ? "default" : account_id;
        
        // ✅ 对齐 OpenClaw markChannelLoggedOut:
        //   next = { running: false, restartPending: false,
        //            lastError: cleared ? "logged out" : current.lastError,
        //            connected: false (if was boolean) }
        //   注意: 不修改 configured 字段 (OpenClaw 不修改)
        update_runtime_fields(channel_id, effective_account_id, [cleared](auto& s) {
            s.running         = false;
            s.restart_pending = false;
            s.last_stop_at    = core::utils::now().time_since_epoch().count() / 1000000;
            if (cleared) {
                s.last_error = std::string("logged out");
            }
            // 如果之前有 connected 值，则设为 false (对齐 OpenClaw)
            if (s.connected.has_value()) {
                s.connected = false;
            }
        });
        
        return core::Result<void>::Ok();
    }
    
    // ========== Status Query ==========
    
    ChannelRuntimeSnapshot get_runtime_snapshot() const {
        // ✅ 对齐 OpenClaw getRuntimeSnapshot():
        //   遍历 listChannelPlugins() (所有注册插件，不只是已启动的)
        //   对每个 plugin，遍历 listAccountIds(cfg)
        //   合并 store.runtimes 中的运行时状态
        //   根据 enabled/configured 设置 lastError fallback
        std::lock_guard<std::mutex> lock(stores_mutex_);
        
        ChannelRuntimeSnapshot snapshot;
        
        // 1. 遍历所有注册的 channel plugins (对齐 OpenClaw: listChannelPlugins())
        const auto& all_plugins = ChannelPluginRegistry::instance().list_plugins();
        for (const auto& plugin : all_plugins) {
            const std::string& channel_id_str = plugin.id;
            ChannelId channel_id = channel_id_str.c_str();
            
            // 获取该 channel 的 store（可能不存在，即未启动）
            auto store_it = channel_stores_.find(channel_id_str);
            
            // 2. 遍历该 plugin 的所有账户 (对齐 OpenClaw: plugin.config.listAccountIds(cfg))
            // HunterClaw 简化版: 从 store 中获取已知账户，或使用 "default"
            std::vector<std::string> account_ids;
            if (store_it != channel_stores_.end()) {
                for (const auto& [acc_id, _] : store_it->second.runtimes) {
                    account_ids.push_back(acc_id);
                }
            }
            if (account_ids.empty()) {
                account_ids.push_back("default");
            }
            
            // 3. 确定 defaultAccountId (对齐 OpenClaw: resolveChannelDefaultAccountId)
            std::string default_account_id = "default";
            
            std::map<std::string, ChannelAccountSnapshot> accounts;
            for (const auto& acc_id : account_ids) {
                ChannelAccountSnapshot current;
                current.account_id = acc_id;
                
                // 从 store 中获取运行时状态
                if (store_it != channel_stores_.end()) {
                    auto runtime_it = store_it->second.runtimes.find(acc_id);
                    if (runtime_it != store_it->second.runtimes.end()) {
                        current = runtime_it->second;
                    }
                }
                current.account_id = acc_id;
                
                // 4. 对齐 OpenClaw: enabled/configured fallback
                bool enabled = current.enabled.value_or(true);
                bool configured = current.configured.value_or(true);
                
                // 5. 对齐 OpenClaw: 未运行时设置 lastError fallback
                if (!current.running.value_or(false)) {
                    if (!enabled) {
                        if (!current.last_error.has_value() || current.last_error->empty()) {
                            current.last_error = std::string("disabled");
                        }
                    } else if (!configured) {
                        if (!current.last_error.has_value() || current.last_error->empty()) {
                            current.last_error = std::string("not configured");
                        }
                    }
                }
                
                accounts[acc_id] = current;
            }
            
            // 6. 设置 default account (对齐 OpenClaw: channels[plugin.id] = defaultAccount)
            auto default_it = accounts.find(default_account_id);
            if (default_it != accounts.end()) {
                snapshot.channels[channel_id] = default_it->second;
            } else if (!accounts.empty()) {
                snapshot.channels[channel_id] = accounts.begin()->second;
            }
            
            snapshot.channel_accounts[channel_id] = accounts;
        }
        
        // 7. 补充 channel_stores_ 中有但 plugin registry 中没有的 channel
        //    (兼容动态注册的 channel)
        for (const auto& [channel_id_str, store] : channel_stores_) {
            if (snapshot.channel_accounts.find(channel_id_str) != snapshot.channel_accounts.end()) {
                continue;  // 已处理
            }
            ChannelId channel_id = channel_id_str.c_str();
            auto default_it = store.runtimes.find("default");
            if (default_it != store.runtimes.end()) {
                snapshot.channels[channel_id] = default_it->second;
            }
            for (const auto& [account_id, runtime] : store.runtimes) {
                snapshot.channel_accounts[channel_id][account_id] = runtime;
            }
        }
        
        return snapshot;
    }
    
    bool is_manually_stopped(ChannelId channel_id, const std::string& account_id) const {
        std::string effective_account_id = account_id.empty() ? "default" : account_id;
        std::string key = std::string(channel_id) + ":" + effective_account_id;
        
        std::lock_guard<std::mutex> lock(manually_stopped_mutex_);
        return manually_stopped_.find(key) != manually_stopped_.end();
    }
    
    void reset_restart_attempts(ChannelId channel_id, const std::string& account_id) {
        std::string effective_account_id = account_id.empty() ? "default" : account_id;
        std::string key = std::string(channel_id) + ":" + effective_account_id;
        
        std::lock_guard<std::mutex> lock(restart_attempts_mutex_);
        restart_attempts_.erase(key);
    }
    
    // ========== Health Monitoring ==========
    
    void start_health_monitor(const ChannelHealthMonitorConfig& config) {
        if (health_monitor_running_.load()) {
            return;
        }
        
        health_config_ = config;
        health_monitor_running_.store(true);
        
        health_monitor_thread_ = std::thread([this]() {
            health_check_loop();
        });
        
        log()->info("Channel health monitor started");
    }
    
    void stop_health_monitor() {
        if (!health_monitor_running_.load()) {
            return;
        }
        
        health_monitor_running_.store(false);
        
        if (health_monitor_thread_.joinable()) {
            health_monitor_thread_.join();
        }
        
        log()->info("Channel health monitor stopped");
    }
    
    void set_dispatcher(std::shared_ptr<auto_reply::MessageDispatcher> dispatcher) {
        dispatcher_ = std::move(dispatcher);
        log()->info("MessageDispatcher wired into ChannelManager");
    }

    // ========== Channel Activity (对齐 OpenClaw: channel-activity.ts) ==========

    void record_channel_activity(
        const ChannelId& channel_id,
        const std::string& account_id,
        ChannelDirection direction,
        std::optional<int64_t> at = std::nullopt
    ) {
        std::string effective_account_id = account_id.empty() ? "default" : account_id;
        // trim
        while (!effective_account_id.empty() && (effective_account_id.front() == ' ' || effective_account_id.front() == '\t'))
            effective_account_id.erase(effective_account_id.begin());
        while (!effective_account_id.empty() && (effective_account_id.back() == ' ' || effective_account_id.back() == '\t'))
            effective_account_id.pop_back();
        if (effective_account_id.empty()) effective_account_id = "default";

        int64_t ts = at.value_or(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch()
            ).count()
        );
        std::string key = channel_id + ":" + effective_account_id;
        std::lock_guard<std::mutex> lock(activity_mutex_);
        auto& entry = channel_activity_[key];
        if (direction == ChannelDirection::Inbound) {
            entry.inbound_at = ts;
        } else {
            entry.outbound_at = ts;
        }
    }

    ChannelActivityEntry get_channel_activity(
        const ChannelId& channel_id,
        const std::string& account_id
    ) const {
        std::string effective_account_id = account_id.empty() ? "default" : account_id;
        std::string key = channel_id + ":" + effective_account_id;
        std::lock_guard<std::mutex> lock(activity_mutex_);
        auto it = channel_activity_.find(key);
        if (it != channel_activity_.end()) {
            return it->second;
        }
        return ChannelActivityEntry{};
    }

private:
    
    ChannelManager::ChannelRuntimeStore& get_store(ChannelId channel_id) {
        std::lock_guard<std::mutex> lock(stores_mutex_);
        return channel_stores_[std::string(channel_id)];
    }
    
    const ChannelManager::ChannelRuntimeStore& get_store(ChannelId channel_id) const {
        std::lock_guard<std::mutex> lock(stores_mutex_);
        return channel_stores_.at(std::string(channel_id));
    }
    
    ChannelAccountSnapshot get_runtime(ChannelId channel_id, const std::string& account_id) const {
        std::lock_guard<std::mutex> lock(stores_mutex_);
        
        auto store_it = channel_stores_.find(std::string(channel_id));
        if (store_it == channel_stores_.end()) {
            return ChannelAccountSnapshot{};
        }
        
        auto runtime_it = store_it->second.runtimes.find(account_id);
        if (runtime_it == store_it->second.runtimes.end()) {
            return ChannelAccountSnapshot{};
        }
        
        return runtime_it->second;
    }
    
    void set_runtime(ChannelId channel_id, const std::string& account_id, const ChannelAccountSnapshot& snapshot) {
        std::lock_guard<std::mutex> lock(stores_mutex_);
        channel_stores_[std::string(channel_id)].runtimes[account_id] = snapshot;
    }
    
    // ✅ 原子性字段更新 (避免 read-modify-write 并发问题,100% 对齐 OpenClaw)
    void update_runtime_fields(
        ChannelId channel_id,
        const std::string& account_id,
        std::function<void(ChannelAccountSnapshot&)> updater
    ) {
        std::lock_guard<std::mutex> lock(stores_mutex_);
        auto& snapshot = channel_stores_[std::string(channel_id)].runtimes[account_id];
        updater(snapshot);
    }
    
    // ========== Auto Restart Logic (100% 对齐 OpenClaw) ==========
    
    void schedule_auto_restart(ChannelId channel_id, const std::string& account_id, int attempt) {
        std::string key = std::string(channel_id) + ":" + account_id;
        
        // Check if manually stopped
        {
            std::lock_guard<std::mutex> lock(manually_stopped_mutex_);
            if (manually_stopped_.find(key) != manually_stopped_.end()) {
                return;  // Don't auto-restart if manually stopped
            }
        }
        
        // Check max attempts
        if (attempt >= MAX_RESTART_ATTEMPTS) {
            log()->error("Channel {} account {} exceeded max restart attempts", channel_id, account_id);
            return;
        }
        
        // Compute backoff
        int64_t delay_ms = compute_backoff(restart_policy_, attempt);
        
        log()->info("Scheduling restart for channel {} account {} in {}ms (attempt {})",
                     channel_id, account_id, delay_ms, attempt + 1);
        
        // Schedule restart
        std::thread([this, channel_id, account_id, attempt, delay_ms]() {
            std::this_thread::sleep_for(std::chrono::milliseconds(delay_ms));
            
            StartChannelOptions opts;
            opts.preserve_restart_attempts = true;
            auto result = start_channel(channel_id, account_id, opts);
            if (result.is_err()) {
                log()->error("Auto-restart failed for channel {} account {}: {}",
                             channel_id, account_id, result.error);
                schedule_auto_restart(channel_id, account_id, attempt + 1);
            }
        }).detach();
    }
    
    int64_t compute_backoff(const BackoffPolicy& policy, int attempt) const {
        // Exponential backoff with jitter (对齐 OpenClaw)
        double base_delay = policy.initial_ms * std::pow(policy.factor, attempt);
        double capped_delay = std::min(base_delay, static_cast<double>(policy.max_ms));
        
        // Add jitter
        std::random_device rd;
        std::mt19937 gen(rd());
        std::uniform_real_distribution<> dis(-policy.jitter, policy.jitter);
        double jitter = dis(gen);
        
        double final_delay = capped_delay * (1.0 + jitter);
        return static_cast<int64_t>(std::max(final_delay, 0.0));
    }
    
    // ========== Health Check Loop ==========

    void health_check_loop() {
        auto start_time = core::utils::now();
        int64_t start_time_ms = start_time.time_since_epoch().count() / 1000000;

        while (health_monitor_running_.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(health_config_.check_interval_ms));

            auto now = core::utils::now();
            int64_t now_ms = now.time_since_epoch().count() / 1000000;

            // Skip if within startup grace period
            if (now_ms - start_time_ms < health_config_.timing.monitor_startup_grace_ms) {
                continue;
            }

            // Check all channels (copy keys to avoid holding lock during restart)
            std::vector<std::string> channel_ids;
            {
                std::lock_guard<std::mutex> lock(stores_mutex_);
                for (const auto& [channel_id_str, store] : channel_stores_) {
                    channel_ids.push_back(channel_id_str);
                }
            }
            for (const auto& channel_id_str : channel_ids) {
                check_channel_health(channel_id_str);
            }
        }
    }
    
    void check_channel_health(ChannelId channel_id) {
        auto now_ms = core::utils::now().time_since_epoch().count() / 1000000;

        // Copy runtimes to avoid holding lock during restart
        std::map<std::string, ChannelAccountSnapshot> runtimes_copy;
        {
            std::lock_guard<std::mutex> lock(stores_mutex_);
            auto it = channel_stores_.find(std::string(channel_id));
            if (it == channel_stores_.end()) return;
            runtimes_copy = it->second.runtimes;
        }

        for (const auto& [account_id, runtime] : runtimes_copy) {
            std::string key = std::string(channel_id) + ":" + account_id;
            
            // ✅ 1. 跳过手动停止的 Channel (对齐 OpenClaw: isManuallyStopped)
            {
                std::lock_guard<std::mutex> lock(manually_stopped_mutex_);
                if (manually_stopped_.find(key) != manually_stopped_.end()) {
                    continue;  // 尊重用户意图,不自动重启
                }
            }
            
            // ✅ 2. 评估健康状态 (对齐 OpenClaw: evaluateChannelHealth)
            auto health = evaluate_channel_health(runtime, channel_id, now_ms, health_config_);
            
            if (health.healthy) {
                continue;  // 健康,无需处理
            }
            
            // ✅ 3. 冷却期检查 (对齐 OpenClaw: cooldownMs)
            {
                std::lock_guard<std::mutex> lock(restart_records_mutex_);
                auto record_it = restart_records_.find(key);
                
                if (record_it != restart_records_.end()) {
                    int64_t cooldown_ms = health_config_.cooldown_cycles * health_config_.check_interval_ms;
                    int64_t last_restart_elapsed = now_ms - record_it->second.last_restart_at;
                    
                    if (last_restart_elapsed <= cooldown_ms) {
                        continue;  // 在冷却期内,跳过重启
                    }
                }
            }
            
            // ✅ 4. 重启频率限制 (对齐 OpenClaw: maxRestartsPerHour)
            std::vector<RestartEvent> restarts_this_hour;
            {
                std::lock_guard<std::mutex> lock(restart_records_mutex_);
                auto record_it = restart_records_.find(key);
                
                if (record_it != restart_records_.end()) {
                    for (const auto& event : record_it->second.restarts_this_hour) {
                        int64_t event_elapsed = now_ms - event.at;
                        if (event_elapsed < 60 * 60 * 1000) {  // 1 小时内
                            restarts_this_hour.push_back(event);
                        }
                    }
                }
                
                if (restarts_this_hour.size() >= static_cast<size_t>(health_config_.max_restarts_per_hour)) {
                    log()->warn("Channel {} account {} hit restart limit ({} restarts/hour)",
                                channel_id, account_id, restarts_this_hour.size());
                    continue;  // 超过频率限制,跳过重启
                }
            }
            
            // ✅ 5. 执行重启 (对齐 OpenClaw)
            log()->info("Channel {} account {} restarting (reason: {})",
                        channel_id, account_id, health.reason);

            // 如果正在运行,先停止
            if (runtime.running.value_or(false)) {
                stop_channel(channel_id, account_id);
            }
            
            // 重置重启尝试计数
            reset_restart_attempts(channel_id, account_id);
            
            // 启动 Channel
            StartChannelOptions opts;
            opts.force = false;
            opts.preserve_restart_attempts = false;
            start_channel(channel_id, account_id, opts);
            
            // ✅ 6. 记录重启事件 (对齐 OpenClaw: restartRecords.set)
            {
                std::lock_guard<std::mutex> lock(restart_records_mutex_);
                restarts_this_hour.push_back({now_ms});
                RestartRecord record;
                record.last_restart_at = now_ms;
                record.restarts_this_hour = std::move(restarts_this_hour);
                restart_records_[key] = record;
            }
        }
    }
    
    ChannelManager::Options options_;
    std::shared_ptr<auto_reply::MessageDispatcher> dispatcher_;  // Aligned with openclaw: runtime.dispatchReplyFromConfig
    
    mutable std::mutex stores_mutex_;
    std::map<std::string, ChannelManager::ChannelRuntimeStore> channel_stores_;

    // ✅ Channel Activity 追踪 (对齐 OpenClaw: channel-activity.ts)
    // key = "channelId:accountId"
    mutable std::mutex activity_mutex_;
    std::map<std::string, ChannelActivityEntry> channel_activity_;
    
    mutable std::mutex manually_stopped_mutex_;
    std::set<std::string> manually_stopped_;
    
    mutable std::mutex restart_attempts_mutex_;
    std::map<std::string, int> restart_attempts_;
    
    // ✅ 重启记录追踪 (对齐 OpenClaw: restartRecords)
    mutable std::mutex restart_records_mutex_;
    std::map<std::string, RestartRecord> restart_records_;
    
    std::thread health_monitor_thread_;
    std::atomic<bool> health_monitor_running_{false};
    ChannelHealthMonitorConfig health_config_;
    
    static constexpr int MAX_RESTART_ATTEMPTS = 10;
    BackoffPolicy restart_policy_;
};

// ============================================================================
// ChannelManager Public Interface
// ============================================================================

ChannelManager::ChannelManager(const Options& options)
    : impl_(std::make_unique<ChannelManagerImpl>(options)) {}

ChannelManager::~ChannelManager() {
    // Destructor must be defined after ChannelManagerImpl is complete
}

// Factory method
std::shared_ptr<ChannelManager> ChannelManager::create() {
    Options options;
    options.load_config = []() { return core::Config(); };
    return std::make_shared<ChannelManager>(options);
}

core::Result<void> ChannelManager::start_channel(
    ChannelId channel_id,
    const std::string& account_id,
    const StartChannelOptions& options
) {
    return impl_->start_channel(channel_id, account_id, options);
}

core::Result<void> ChannelManager::start_channels(const std::vector<ChannelId>& channel_ids) {
    return impl_->start_channels(channel_ids);
}

core::Result<void> ChannelManager::stop_channel(ChannelId channel_id, const std::string& account_id) {
    return impl_->stop_channel(channel_id, account_id);
}

core::Result<void> ChannelManager::mark_channel_logged_out(ChannelId channel_id, const std::string& account_id) {
    return impl_->mark_channel_logged_out(channel_id, account_id);
}

ChannelRuntimeSnapshot ChannelManager::get_runtime_snapshot() const {
    return impl_->get_runtime_snapshot();
}

std::vector<ChannelId> ChannelManager::list_channels() const {
    auto snapshot = impl_->get_runtime_snapshot();
    std::vector<ChannelId> result;
    for (const auto& pair : snapshot.channels) {
        result.push_back(pair.first);
    }
    return result;
}

bool ChannelManager::is_manually_stopped(ChannelId channel_id, const std::string& account_id) const {
    return impl_->is_manually_stopped(channel_id, account_id);
}

void ChannelManager::reset_restart_attempts(ChannelId channel_id, const std::string& account_id) {
    impl_->reset_restart_attempts(channel_id, account_id);
}

void ChannelManager::start_health_monitor(const ChannelHealthMonitorConfig& config) {
    impl_->start_health_monitor(config);
}

void ChannelManager::stop_health_monitor() {
    impl_->stop_health_monitor();
}

void ChannelManager::set_dispatcher(std::shared_ptr<auto_reply::MessageDispatcher> dispatcher) {
    impl_->set_dispatcher(std::move(dispatcher));
}

void ChannelManager::record_channel_activity(
    const ChannelId& channel_id,
    const std::string& account_id,
    ChannelDirection direction,
    std::optional<int64_t> at
) {
    impl_->record_channel_activity(channel_id, account_id, direction, at);
}

ChannelActivityEntry ChannelManager::get_channel_activity(
    const ChannelId& channel_id,
    const std::string& account_id
) const {
    return impl_->get_channel_activity(channel_id, account_id);
}

// ============================================================================
// ChannelHealthMonitor
// ============================================================================

ChannelHealthMonitor::ChannelHealthMonitor(
    std::shared_ptr<ChannelManager> manager,
    const ChannelHealthMonitorConfig& config
) : manager_(manager), config_(config) {}

ChannelHealthMonitor::~ChannelHealthMonitor() {
    stop();
}

void ChannelHealthMonitor::start() {
    if (running_.load()) {
        return;
    }
    
    running_.store(true);
    monitor_thread_ = std::thread([this]() {
        monitor_loop();
    });
}

void ChannelHealthMonitor::stop() {
    if (!running_.load()) {
        return;
    }
    
    running_.store(false);
    if (monitor_thread_.joinable()) {
        monitor_thread_.join();
    }
}

bool ChannelHealthMonitor::is_running() const {
    return running_.load();
}

void ChannelHealthMonitor::monitor_loop() {
    while (running_.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(config_.check_interval_ms));
        check_all_channels();
    }
}

void ChannelHealthMonitor::check_all_channels() {
    // Get snapshot and check each channel
    auto snapshot = manager_->get_runtime_snapshot();
    
    for (const auto& [channel_id, accounts] : snapshot.channel_accounts) {
        for (const auto& [account_id, runtime] : accounts) {
            // Check health...
        }
    }
}

std::vector<ChannelHealth> ChannelHealthMonitor::get_all_health() const {
    std::vector<ChannelHealth> result;
    
    auto snapshot = manager_->get_runtime_snapshot();
    for (const auto& [channel_id, accounts] : snapshot.channel_accounts) {
        for (const auto& [account_id, runtime] : accounts) {
            ChannelHealth health;
            health.channel_id = channel_id;
            health.account_id = account_id;
            // Derive status from optional fields
            bool is_connected = runtime.connected.value_or(false);
            bool is_running   = runtime.running.value_or(false);
            if (is_connected) {
                health.status = ChannelAccountStatus::Connected;
            } else if (is_running) {
                health.status = ChannelAccountStatus::Starting;
            } else if (runtime.last_error.has_value()) {
                health.status = ChannelAccountStatus::Error;
            } else {
                health.status = ChannelAccountStatus::Stopped;
            }
            health.is_healthy = is_connected;
            health.last_check_time = core::utils::now();
            result.push_back(health);
        }
    }
    
    return result;
}

} // namespace channels
} // namespace hunterclaw
