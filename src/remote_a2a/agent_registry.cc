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

        // ── 挂起态主动通知（SSE / 回调 / 后台跟进 三条异步通道）──────────────
        //
        // ★★ 这三条通道在【别的线程】发现远端挂起，此时可能已经没有任何
        //   tool_call 在等结果（尤其 background_follow_up: 模型早就让渡了）。
        //   没有这段注入，"远端要求用户确认"只能等模型下次主动 poll 才被
        //   发现 —— 模型若判断"已让渡，等下轮"或直接收尾，用户永远不会
        //   被问到，远端任务挂到 HardTimeout。
        //
        // ★ 这里【只做推送通知】，不发起等待:
        //   ⚠ 异步线程上没有 run 上下文，不能占 InterruptCoordinator 的
        //     会话槽位 —— 占了会让真正在 run 内的 interrupt 注册失败。
        //   真正的"问 + 等 + 恢复"仍由 ReactEngine 在下一次工具调用时完成
        //   （follow_up 会拿到挂起态并上抛 interrupt）。
        //
        // ⚠ 用 weak_ptr 捕获 engine: resolver 存在 engine 内部，
        //   捕 shared_ptr 会形成 engine→resolver→engine 强引用环，
        //   engine 永不析构，detached 后台线程也永不退出。
        {
            std::weak_ptr<ExecutionEngine> weak_engine = rt.engine;
            auto store = store_;
            const std::string agent_name = ac.name;
            rt.engine->set_suspend_resolver(
                [weak_engine, store, agent_name](const std::string& handle) {
                    auto engine = weak_engine.lock();
                    if (!engine || !store) return;
                    auto snap = store->get(handle);
                    if (!snap || snap->session_key.empty()) return;

                    // ★★ 去重闸: 同一次挂起会被【多条通道重复触发】。
                    //
                    //   settle_terminal 是唯一落终态入口，但它自己被调用多次:
                    //     · ingest_task    (execution_engine.cc:421) —— 同步探测
                    //       ⚠ 而 poll_once(:999) 与 background_loop→run_probe
                    //         都经由 ingest_task，回调还会因远端未收到 2xx 而重投
                    //     · run_stream    (execution_engine.cc:632) —— SSE 线程
                    //   每次都会走到 ④ maybe_notify_suspended。
                    //
                    //   ⚠ 没有这道闸，用户会为同一件事被推 2~5 条"远端在等你"。
                    //     这不只是噪音: deliver_progress 传的是 hb=false（正式
                    //     事件，刻意不被心跳合并），而 system_events 只有 20 条
                    //     上限 —— 重复推送会把真正的完成/失败事件挤出队列。
                    //
                    //   ★ engine 刻意不替 resolver 去重（见 execution_engine.h:313）:
                    //     闸门必须是【跨线程跨通道】唯一的，而 CAS 语义只能由
                    //     store 提供。这里是契约要求的实现方。
                    if (!store->try_claim_question(handle)) return;

                    // ★ 只推一条提示，告诉用户远端在等他。
                    //   ⚠ 不在这里问、不在这里等 —— 见上方说明。
                    std::string text = "远端任务需要你确认后才能继续";
                    if (!snap->progress_text.empty()) text = snap->progress_text;
                    if (!snap->pending_token_ids.empty()) {
                        text += "（待确认 " +
                                std::to_string(snap->pending_token_ids.size()) + " 项）";
                    }
                    // hb=false: 这是必须让用户看到的正式事件，
                    // ⚠ 传 true 会被当心跳合并掉，用户可能永远看不到。
                    deliver_progress(snap->session_key, agent_name, text, false);
                });
        }

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
    // ★★ 受理闸先关，且【无条件】关 —— 不能藏在下面的 sweeping_ 判断里。
    //   sweeping_ 为 false 有两种情况: 从未 start（如测试或未 activate 的
    //   registry）、或已 stop 过。前者若被提前 return 跳过，闸门就永远开着。
    //   ⚠ 恢复路径靠这个闸判断"还能不能往远端发确认"，漏关会让进程退出
    //     期间用户的一句"确认"把正在被放弃的流程真的提交出去。
    accepting_.store(false);

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

// ── 进程级接线 ──────────────────────────────────────────────────────────────

void activate_remote_a2a_registry(const std::shared_ptr<RemoteA2aRegistry>& reg) {
    if (!reg) return;

    // ── ① 注册 interrupt 恢复函数 ────────────────────────────────────────
    //
    // ★★ 这是"用户输入后由 ReactEngine 传给 remote_a2a 恢复远端执行"的
    //   remote_a2a 那一半。ReactEngine 拿到用户答复后按 kind
    //   ("remote_a2a.input_required") 路由到这里。
    //
    // ⚠ 必须在【工具可能被调用之前】完成:
    //   工具已经能产出 interrupt 了，恢复函数却还没注册的话，
    //   ReactEngine 的前置检查会 fail-closed 成
    //   "未注册中断恢复处理器" —— 用户白答一次，远端任务只能等超时。
    //
    // ★ 注册是【进程级一次性】的（InterruptCoordinator 拒绝重复注册同 kind），
    //   多次调用时后面的会被拒绝并保留第一个 registry ——
    //   ⚠ 这在热重载场景下会让恢复路由指向旧 registry。当前流程只调一次，
    //     若将来支持热重载需要显式反注册。
    register_remote_a2a_resume_handler(reg);

    // ── ② 启动后台 sweeper ───────────────────────────────────────────────
    // ★ 放在 handler 之后: sweeper 一起就可能判死在途任务并触发状态变更，
    //   此时恢复通道应当已经就绪。
    reg->start_sweeper();
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

    // ★ 接线序列收敛在 activate 里 —— 这里【不】重复它的步骤。
    //   ⚠ 曾经这里内联了 register_resume_handler + start_sweeper，
    //     而 RemoteA2aRuntime::init 只抄到了后者，于是生产路径静默缺失
    //     恢复通道。见 activate_remote_a2a_registry 的说明。
    activate_remote_a2a_registry(reg);

    if (out_registry) *out_registry = reg;
    return registered;
}

}  // namespace remote_a2a
}  // namespace hunterclaw
