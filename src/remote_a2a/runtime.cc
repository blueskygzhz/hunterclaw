// ============================================================================
// remote_a2a/runtime.cc
// 进程级 Remote A2A 运行时。
// ============================================================================

#include "hunterclaw/remote_a2a/runtime.h"

#include <cstdlib>

namespace hunterclaw {
namespace remote_a2a {

RemoteA2aRuntime& RemoteA2aRuntime::instance() {
    // ★ 函数内 static：C++11 起保证线程安全初始化，且避免静态初始化顺序问题
    //   （本类可能在其他全局对象构造期被访问）。
    static RemoteA2aRuntime inst;
    return inst;
}

bool RemoteA2aRuntime::disabled_by_env() {
    // 対标 agent_instance.cc:193 的 HUNTERCLAW_MCP=0 逃生阀。
    // ⚠ 只认 "0"：空串或未设置都视为启用（与 MCP 的判据一致，
    //   否则运维习惯不一致会踩坑）。
    const char* v = std::getenv("HUNTERCLAW_REMOTE_A2A");
    return v != nullptr && std::string(v) == "0";
}

core::Result<int> RemoteA2aRuntime::init(const nlohmann::json& config_node) {
    {
        std::lock_guard<std::mutex> lk(mu_);
        if (inited_) {
            // 幂等：重复 init 不重建（重建会让已登记的让渡任务失去引擎）
            return core::Result<int>::Ok(enabled_ ? static_cast<int>(
                                                       registry_ ? registry_->agent_count() : 0)
                                                  : 0);
        }
    }

    if (disabled_by_env()) {
        std::lock_guard<std::mutex> lk(mu_);
        inited_ = true;
        enabled_ = false;
        warnings_.push_back("remote_a2a 已由 HUNTERCLAW_REMOTE_A2A=0 强制禁用");
        return core::Result<int>::Ok(0);
    }

    auto parsed = parse_remote_a2a_config(config_node);
    if (parsed.is_err()) {
        // ★ 配置无效是【真错误】：与「远端离线」不同，
        //   前者是运维写错了，静默忽略会让人以为功能在跑。
        std::lock_guard<std::mutex> lk(mu_);
        inited_ = true;
        enabled_ = false;
        return core::Result<int>::Err("remoteA2a 配置无效: " + parsed.error);
    }

    const RemoteA2aConfig& cfg = parsed.get();
    if (!cfg.enabled) {
        std::lock_guard<std::mutex> lk(mu_);
        inited_ = true;
        enabled_ = false;
        for (const auto& w : cfg.warnings) warnings_.push_back(w);
        return core::Result<int>::Ok(0);
    }

    // ⚠ build() 会串行拉 N 张卡（有网络 IO），必须在锁外做。
    auto reg = RemoteA2aRegistry::create(cfg);
    auto built = reg->build();
    if (built.is_err()) {
        std::lock_guard<std::mutex> lk(mu_);
        inited_ = true;
        enabled_ = false;
        for (const auto& w : cfg.warnings) warnings_.push_back(w);
        return core::Result<int>::Err(built.error);
    }

    // sweeper 只启动一次（进程级）
    reg->start_sweeper();

    std::lock_guard<std::mutex> lk(mu_);
    // 双检：并发 init 时后到者放弃自己的成果
    if (inited_) {
        reg->stop_sweeper();  // ★ 必须停掉，否则泄漏一个线程
        return core::Result<int>::Ok(enabled_ && registry_
                                        ? static_cast<int>(registry_->agent_count())
                                        : 0);
    }
    registry_ = reg;
    for (auto& w : reg->warnings()) warnings_.push_back(std::move(w));
    inited_ = true;
    enabled_ = true;
    return built;
}

bool RemoteA2aRuntime::enabled() const {
    std::lock_guard<std::mutex> lk(mu_);
    return enabled_ && registry_ != nullptr;
}

bool RemoteA2aRuntime::callback_needed() const {
    std::lock_guard<std::mutex> lk(mu_);
    if (!enabled_ || !registry_) return false;
    return registry_->callback_enabled();
}

std::shared_ptr<RemoteA2aRegistry> RemoteA2aRuntime::registry() const {
    std::lock_guard<std::mutex> lk(mu_);
    return registry_;
}

std::vector<std::string> RemoteA2aRuntime::warnings() const {
    std::lock_guard<std::mutex> lk(mu_);
    return warnings_;
}

core::Result<int> RemoteA2aRuntime::register_into(tools::ToolManager& manager) {
    std::shared_ptr<RemoteA2aRegistry> reg;
    {
        std::lock_guard<std::mutex> lk(mu_);
        if (!enabled_ || !registry_) {
            // 未启用不是错误 —— 対标 enable_remote_a2a 的语义
            return core::Result<int>::Ok(0);
        }
        reg = registry_;
    }
    // ⚠ 锁外调用：register_tools 内部会 warm_up()（可能读卡缓存），
    //   不该持 runtime 锁。且它自己有锁保护 registry 内部状态。
    return reg->register_tools(manager);
}

bool RemoteA2aRuntime::attach_callback(
    const std::function<bool(const std::shared_ptr<RemoteA2aRegistry>&)>& mounter) {
    std::shared_ptr<RemoteA2aRegistry> reg;
    {
        std::lock_guard<std::mutex> lk(mu_);
        if (callback_mounted_) return true;  // 幂等：路由不能挂两次
        if (!enabled_ || !registry_) return false;
        if (!registry_->callback_enabled()) return false;
        reg = registry_;
    }

    if (!mounter) return false;
    // ⚠ 锁外调用 mounter：它会碰 gateway 的路由表，不该持 runtime 锁
    const bool ok = mounter(reg);

    std::lock_guard<std::mutex> lk(mu_);
    if (ok) callback_mounted_ = true;
    return ok;
}

void RemoteA2aRuntime::shutdown() {
    std::shared_ptr<RemoteA2aRegistry> reg;
    {
        std::lock_guard<std::mutex> lk(mu_);
        reg = registry_;
        // ★ 只置 enabled_=false，【不】清空 registry_：
        //   · enabled_=false 让后续 register_into 返回 0（进程正在退出，
        //     不该再往新会话注册工具）
        //   · 保留 registry_ 让在途回调仍能 lock 到 weak_ptr，
        //     从而返回 200/503 而不是崩溃
        //   ⚠ 清空 registry_ 会让持有 weak_ptr 的回调路由在 shutdown 与
        //     进程真正退出之间的窗口里拿到 nullptr，回 503 后远端重试，
        //     而此时我们已经不打算处理了 —— 让它 lock 成功并走正常路径更干净。
        enabled_ = false;
    }
    if (reg) reg->stop_sweeper();
}

}  // namespace remote_a2a
}  // namespace hunterclaw
