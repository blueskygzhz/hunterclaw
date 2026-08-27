// ============================================================================
// remote_a2a/agent_registry.cc
// 远端 A2A agent 注册表实现。
// ============================================================================

#include "hunterclaw/remote_a2a/agent_registry.h"

#include <chrono>

namespace hunterclaw {
namespace remote_a2a {

std::shared_ptr<RemoteA2aRegistry> RemoteA2aRegistry::create(const RemoteA2aConfig& cfg,
                                                             infra::HttpClient::Ptr http) {
    return std::make_shared<RemoteA2aRegistry>(cfg, std::move(http));
}

RemoteA2aRegistry::RemoteA2aRegistry(const RemoteA2aConfig& cfg,
                                     infra::HttpClient::Ptr http)
    : cfg_(cfg),
      http_(http ? std::move(http) : infra::HttpClient::create()),
      // ★ 全局单例：status 工具只收 handle，必须能跨 agent 查到
      store_(PendingTaskStore::create(cfg.store)) {
    warnings_ = cfg.warnings;  // 承接解析期警告
}

RemoteA2aRegistry::~RemoteA2aRegistry() { stop_sweeper(); }

core::Result<int> RemoteA2aRegistry::build() {
    {
        std::lock_guard<std::mutex> lk(mu_);
        if (built_) {
            return core::Result<int>::Ok(static_cast<int>(runtimes_.size()));
        }
    }

    // 工具名去重表。★ 按配置顺序推进，保证名字稳定
    //   （safe_remote_a2a_tool_name 的后缀基于传入顺序，见其注释）
    std::vector<std::string> reserved;
    reserved.push_back(TOOL_STATUS);
    reserved.push_back(TOOL_CANCEL);

    // ⚠ 整个循环在【锁外】执行：ensure_card() 会发 HTTP。
    //   持锁做网络 IO 会把 engine_for_handle / callback_endpoint 等
    //   热路径全部阻塞数秒（N 个远端串行拉卡）。
    std::map<std::string, RemoteAgentRuntime> built;
    std::vector<std::string> new_warnings;

    for (const auto& ac : cfg_.agents) {
        RemoteAgentRuntime rt;
        rt.name = ac.name;
        rt.tool_name = safe_remote_a2a_tool_name(ac.name, reserved);

        rt.cards = CardResolver::create(ac.card, http_);
        rt.client = RemoteAgentClient::create(ac.client, rt.cards, http_);

        ExecutionConfig ec = ac.exec;
        // 回调基址统一由全局配置提供（config.cc 已校验其为 https/loopback）
        ec.callback_base_url = cfg_.callback_base_url;
        rt.engine = ExecutionEngine::create(rt.client, store_, ec);

        // ★ 预热拉卡：失败【不】跳过该远端。
        //   対标 tool_manager.cc:667-671 —— 远端离线时工具仍注册，
        //   否则远端重启期间启动 HunterClaw 会导致工具永久缺失。
        auto card = rt.client->ensure_card();
        if (card.is_ok()) {
            rt.card_ready = true;
        } else {
            rt.card_ready = false;
            rt.card_error = card.error;
            new_warnings.push_back("远端 '" + ac.name +
                                  "' 的 AgentCard 拉取失败（工具仍已注册，调用时会重试）: " +
                                  card.error);
        }

        built[ac.name] = std::move(rt);
    }

    if (built.empty()) {
        return core::Result<int>::Err("没有任何远端 A2A agent 构造成功");
    }

    // ── 回调端点（仅在确有 agent 启用时创建）──────────────────────────
    // ⚠ 不无条件创建：一个存在的回调端点意味着一个鉴权旁路路径，
    //   没人用的时候不该存在。
    std::shared_ptr<CallbackEndpoint> cb;
    if (callback_enabled()) {
        // ⚠ weak_ptr 捕获：端点会被挂到 HTTP 路由上，生命周期可能长于 registry。
        //   shared_ptr 会循环引用，裸 this 会悬垂。
        std::weak_ptr<RemoteA2aRegistry> weak = weak_from_this();
        CallbackEngineLookupFn lookup = [weak](const std::string& h)
            -> std::shared_ptr<ExecutionEngine> {
            auto self = weak.lock();
            if (!self) return nullptr;
            return self->engine_for_handle(h);
        };
        cb = CallbackEndpoint::create(store_, lookup, cfg_.callback);
    }

    {
        std::lock_guard<std::mutex> lk(mu_);
        // ⚠ 双检：并发 build() 时后到者放弃自己的成果，沿用先到者的。
        //   否则两个 runtime 集合会互相覆盖，其中一份的引擎变成孤儿
        //   （已登记的任务反查不到引擎）。
        if (built_) {
            return core::Result<int>::Ok(static_cast<int>(runtimes_.size()));
        }
        runtimes_ = std::move(built);
        for (auto& w : new_warnings) warnings_.push_back(std::move(w));
        callback_ = std::move(cb);
        built_ = true;
        return core::Result<int>::Ok(static_cast<int>(runtimes_.size()));
    }
}

std::shared_ptr<CallbackEndpoint> RemoteA2aRegistry::callback_endpoint() const {
    std::lock_guard<std::mutex> lk(mu_);
    return callback_;
}

bool RemoteA2aRegistry::callback_enabled() const {
    for (const auto& a : cfg_.agents) {
        if (a.exec.enable_callback) return true;
    }
    return false;
}

core::Result<int> RemoteA2aRegistry::register_tools(tools::ToolManager& manager) {
    {
        std::lock_guard<std::mutex> lk(mu_);
        if (!built_) {
            return core::Result<int>::Err("请先调用 build()");
        }
    }

    // ★ session_key 从 ToolManager 的 context 取，绝不从工具 params 取
    //   （params 由模型构造，允许指定 session_key 等于让模型把任务
    //    挂到别人的会话上 —— 见 agent_tool.h 文件头）
    const tools::ToolContext& ctx = manager.context();

    int count = 0;
    std::vector<std::pair<std::string, RemoteAgentRuntime>> snapshot;
    {
        std::lock_guard<std::mutex> lk(mu_);
        for (const auto& kv : runtimes_) snapshot.emplace_back(kv.first, kv.second);
    }

    // 找回每个 agent 的策略（配置里）
    auto policy_of = [&](const std::string& name) -> AgentToolPolicy {
        for (const auto& a : cfg_.agents) {
            if (a.name == name) return a.policy;
        }
        return AgentToolPolicy{};
    };

    for (const auto& [name, rt] : snapshot) {
        auto tool = std::make_shared<RemoteA2aAgentTool>(
            rt.tool_name, name, rt.client, rt.engine, policy_of(name), ctx);
        // 卡已就绪时把描述灌进去（未就绪则用兜底文案，调用时再拉）
        if (rt.card_ready) tool->warm_up();
        manager.register_tool(tool);
        ++count;
    }

    // ── 两个全局跟进工具 ────────────────────────────────────────────────
    // ★ 不按 agent 拆：跟进只需要 handle，拆开只会让工具清单膨胀 3 倍，
    //   而工具数量直接吃 context。
    //
    // ⚠ lookup 用 weak_ptr 捕获 this：工具的生命周期由 ToolManager 管理，
    //   可能长于 registry。用 shared_ptr 会造成循环引用（registry 持工具、
    //   工具持 registry），用裸 this 则 registry 析构后是悬垂指针。
    std::weak_ptr<RemoteA2aRegistry> weak = weak_from_this();
    EngineLookupFn lookup = [weak](const std::string& handle)
        -> std::shared_ptr<ExecutionEngine> {
        auto self = weak.lock();
        if (!self) return nullptr;
        return self->engine_for_handle(handle);
    };

    manager.register_tool(std::make_shared<RemoteA2aStatusTool>(store_, lookup, ctx));
    ++count;

    // cancel 的审批策略：任一远端要求审批 → cancel 也要求审批。
    // ⚠ 取「任一」而非「全部」：cancel 会终止远端计费任务，
    //   宁可多问一次也不能让高敏远端的任务被无审批取消。
    bool cancel_needs_approval = false;
    for (const auto& a : cfg_.agents) {
        if (a.policy.requires_approval) cancel_needs_approval = true;
    }
    manager.register_tool(
        std::make_shared<RemoteA2aCancelTool>(store_, lookup, ctx, cancel_needs_approval));
    ++count;

    return core::Result<int>::Ok(count);
}

int RemoteA2aRegistry::refresh_cards() {
    std::vector<std::shared_ptr<CardResolver>> resolvers;
    std::vector<std::string> names;
    {
        std::lock_guard<std::mutex> lk(mu_);
        for (const auto& kv : runtimes_) {
            resolvers.push_back(kv.second.cards);
            names.push_back(kv.first);
        }
    }

    int ok = 0;
    for (size_t i = 0; i < resolvers.size(); ++i) {
        if (!resolvers[i]) continue;
        // ⚠ 在锁外做网络 IO：refresh() 会发 HTTP，
        //   持锁会把 engine_for_handle 等热路径全部阻塞。
        auto r = resolvers[i]->refresh();
        if (r.is_ok()) {
            ++ok;
            std::lock_guard<std::mutex> lk(mu_);
            auto it = runtimes_.find(names[i]);
            if (it != runtimes_.end()) {
                // ★ from_cache=true 说明是"刷新失败降级为旧卡"，
                //   card_ready 仍应为 true（旧卡可用），但不清空 card_error
                it->second.card_ready = true;
                if (!r.get().from_cache) it->second.card_error.clear();
            }
        }
    }
    return ok;
}

std::shared_ptr<ExecutionEngine> RemoteA2aRegistry::engine_for_handle(
    const std::string& handle) const {
    if (!store_) return nullptr;
    auto t = store_->get(handle);
    if (!t) return nullptr;
    std::lock_guard<std::mutex> lk(mu_);
    auto it = runtimes_.find(t->agent_name);
    if (it == runtimes_.end()) return nullptr;  // 远端已被移出配置
    return it->second.engine;
}

std::vector<RemoteAgentRuntime> RemoteA2aRegistry::runtimes() const {
    std::vector<RemoteAgentRuntime> out;
    std::lock_guard<std::mutex> lk(mu_);
    for (const auto& kv : runtimes_) out.push_back(kv.second);
    return out;
}

std::vector<std::string> RemoteA2aRegistry::warnings() const {
    std::lock_guard<std::mutex> lk(mu_);
    return warnings_;
}

size_t RemoteA2aRegistry::agent_count() const {
    std::lock_guard<std::mutex> lk(mu_);
    return runtimes_.size();
}

// ── sweeper ─────────────────────────────────────────────────────────────────

void RemoteA2aRegistry::start_sweeper(int64_t interval_ms) {
    bool expected = false;
    if (!sweeping_.compare_exchange_strong(expected, true)) return;  // 幂等
    if (interval_ms < 1000) interval_ms = 1000;
    sweeper_ = std::thread([this, interval_ms]() { sweeper_loop(interval_ms); });
}

void RemoteA2aRegistry::stop_sweeper() {
    if (!sweeping_.exchange(false)) return;
    // ★ 用 cv 唤醒而不是等它自己 sleep 完 ——
    //   interval 是 60s，析构时等一分钟不可接受。
    {
        std::lock_guard<std::mutex> lk(sweep_mu_);
    }
    sweep_cv_.notify_all();
    if (sweeper_.joinable()) sweeper_.join();
}

void RemoteA2aRegistry::sweeper_loop(int64_t interval_ms) {
    while (sweeping_.load()) {
        {
            std::unique_lock<std::mutex> lk(sweep_mu_);
            sweep_cv_.wait_for(lk, std::chrono::milliseconds(interval_ms),
                              [this]() { return !sweeping_.load(); });
        }
        if (!sweeping_.load()) break;
        if (store_) store_->sweep();
    }
}

// ── 便捷入口 ────────────────────────────────────────────────────────────────

core::Result<int> enable_remote_a2a(tools::ToolManager& manager,
                                   const nlohmann::json& config_node,
                                   std::shared_ptr<RemoteA2aRegistry>* out_registry,
                                   infra::HttpClient::Ptr http) {
    auto parsed = parse_remote_a2a_config(config_node);
    if (parsed.is_err()) {
        return core::Result<int>::Err("remoteA2a 配置无效: " + parsed.error);
    }
    const RemoteA2aConfig& cfg = parsed.get();

    if (!cfg.enabled) {
        return core::Result<int>::Ok(0);  // 未启用不是错误
    }

    auto reg = RemoteA2aRegistry::create(cfg, std::move(http));
    auto built = reg->build();
    if (built.is_err()) {
        return core::Result<int>::Err(built.error);
    }

    auto registered = reg->register_tools(manager);
    if (registered.is_err()) {
        return core::Result<int>::Err(registered.error);
    }

    reg->start_sweeper();
    if (out_registry) *out_registry = reg;
    return registered;
}

}  // namespace remote_a2a
}  // namespace hunterclaw
