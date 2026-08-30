#pragma once

// ============================================================================
// remote_a2a/agent_registry.h
// 远端 A2A agent 的注册表 —— 対标 mcp::McpToolRegistry。
//
// 职责：
//   ① 按配置构造 CardResolver / RemoteAgentClient / ExecutionEngine 三件套
//   ② 把工具注册进 ToolManager（N 个发送工具 + 2 个全局跟进工具）
//   ③ 提供 handle → engine 的反查（status/cancel 工具需要）
//   ④ 后台 sweep 线程（清理过期记录 + 判死泄漏的在途任务）
//
// ── ★ PendingTaskStore 是全局单例，不是每 agent 一份 ───────────────────────
// 理由：handle 必须能在【不知道属于哪个 agent】的情况下被查到 ——
// status 工具只收 handle。若每 agent 一份 store，status 就得遍历所有 store，
// 且 handle 唯一性无法保证（两个 store 可能生成同样的序号）。
//
// ── ★ 卡拉取失败【不】阻断注册 ─────────────────────────────────────────────
// 対标 tool_manager.cc:667-671 的 MCP 处理：
//   "不返回错误:客户端可能可用但工具发现失败(例如 server 离线)"
// 远端离线时工具仍注册（description 用兜底文案），调用时才报错。
// 反过来做的后果：远端重启期间 HunterClaw 启动 → 工具永久缺失，
// 直到下次重启才恢复。
//
// ── 并发原语约束（项目级）────────────────────────────────────────────────
// ✅ thread / mutex / condition_variable / atomic
// ❌ future / promise / async / packaged_task
// ============================================================================

#include <atomic>
#include <condition_variable>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "hunterclaw/core/types.h"
#include "hunterclaw/infra/http_client.h"
#include "hunterclaw/remote_a2a/agent_tool.h"
#include "hunterclaw/remote_a2a/callback_endpoint.h"
#include "hunterclaw/remote_a2a/config.h"
#include "hunterclaw/remote_a2a/execution_engine.h"
#include "hunterclaw/remote_a2a/pending_tasks.h"
#include "hunterclaw/remote_a2a/remote_agent_client.h"
#include "hunterclaw/tools/tool_manager.h"

namespace hunterclaw {
namespace remote_a2a {

/// 单个远端的运行时三件套
struct RemoteAgentRuntime {
    std::string name;
    /// 实际注册的工具名（已消杀去重）
    std::string tool_name;
    std::shared_ptr<CardResolver> cards;
    std::shared_ptr<RemoteAgentClient> client;
    std::shared_ptr<ExecutionEngine> engine;
    /// 卡是否已成功拉到（未拉到时工具仍注册，见文件头）
    bool card_ready = false;
    std::string card_error;
};

class RemoteA2aRegistry : public std::enable_shared_from_this<RemoteA2aRegistry> {
public:
    static std::shared_ptr<RemoteA2aRegistry> create(
        const RemoteA2aConfig& cfg, infra::HttpClient::Ptr http = nullptr);

    RemoteA2aRegistry(const RemoteA2aConfig& cfg, infra::HttpClient::Ptr http);
    ~RemoteA2aRegistry();

    /// 构造所有远端的运行时。
    /// ★ 单个远端失败只记 warning 并跳过，不影响其他远端。
    /// @return 成功构造的远端数量
    core::Result<int> build();

    /// 注册工具到 ToolManager。
    ///
    /// ⚠ 必须先 build()。
    /// ⚠ ToolContext 从 manager.context() 取 —— session_key 在那里，
    ///   绝不从工具 params 读（见 agent_tool.h 文件头）。
    /// @return 注册的工具总数（含 status/cancel）
    core::Result<int> register_tools(tools::ToolManager& manager);

    /// 刷新所有卡（TTL 到期或手动触发）。
    /// ★ 刷新失败保留旧卡（card_resolver.cc:126-147），故不会造成工具抖动。
    /// @return 刷新成功的数量
    int refresh_cards();

    /// 启动后台 sweep 线程（清理过期记录）。幂等。
    ///
    /// ⚠ 一般不要直接调 —— 用 activate_remote_a2a_registry()，
    ///   它保证 sweeper 与 interrupt 恢复通道成套就位。
    void start_sweeper(int64_t interval_ms = 60000);

    /// 停止 sweeper，并【关闭受理闸】（见 accepting()）。幂等。
    void stop_sweeper();

    /// 是否仍受理新的远端交互。
    ///
    /// ★★ 为什么需要它: interrupt 恢复函数持 weak_ptr 指向本对象，而
    ///   RemoteA2aRuntime::shutdown() 【刻意保留】registry_ 不清空
    ///   （让在途回调仍能 lock 成功、走正常路径而非崩溃）。
    ///   于是 weak_ptr.lock() 在 shutdown 之后依然成功 ——
    ///   ⚠ 单靠 lock() 判活，进程退出期间用户的一句"确认"仍会被发往远端，
    ///     把一个正在被放弃的流程实际提交出去（发邮件、下单等副作用）。
    ///   恢复路径必须显式查这个闸。
    bool accepting() const { return accepting_.load(); }

    /// handle → engine 反查。
    /// ★ 通过 store 查出 agent_name 再映射，而不是把引擎指针塞进 handle ——
    ///   后者会让 handle 变成不可序列化的东西（二期落盘时无法恢复）。
    std::shared_ptr<ExecutionEngine> engine_for_handle(const std::string& handle) const;

    /// 回调接收端。★ 仅在有 agent 启用了 enableCallback 时非空。
    ///
    /// 调用方需自行把它挂到 HTTP 路由上（见 make_callback_http_handler），
    /// 并对 CALLBACK_PATH_PREFIX 注册鉴权旁路。
    /// ⚠ 注册路由但【忘记】注册旁路 = 所有回调 401；
    ///   注册旁路但【忘记】注册路由 = 所有回调 404。两者都是静默失效。
    std::shared_ptr<CallbackEndpoint> callback_endpoint() const;

    /// 是否有任何 agent 启用了回调（决定是否需要挂路由）
    bool callback_enabled() const;

    std::shared_ptr<PendingTaskStore> store() const { return store_; }

    std::vector<RemoteAgentRuntime> runtimes() const;

    /// 解析期 + 构造期的全部警告
    std::vector<std::string> warnings() const;

    size_t agent_count() const;

private:
    void sweeper_loop(int64_t interval_ms);

    RemoteA2aConfig cfg_;
    infra::HttpClient::Ptr http_;
    /// ★ 全局单例（见文件头说明）
    std::shared_ptr<PendingTaskStore> store_;
    /// 仅在有 agent 启用回调时创建
    std::shared_ptr<CallbackEndpoint> callback_;

    mutable std::mutex mu_;
    /// agent_name → runtime
    std::map<std::string, RemoteAgentRuntime> runtimes_;
    std::vector<std::string> warnings_;
    bool built_ = false;

    std::thread sweeper_;
    std::atomic<bool> sweeping_{false};
    /// 受理闸（见 accepting()）。★ 初值 true: build 期间就可能有交互进来。
    std::atomic<bool> accepting_{true};
    std::mutex sweep_mu_;
    std::condition_variable sweep_cv_;
};

/// 激活一个已 build() 完成的 registry —— ★★【唯一】的进程级接线点。
///
/// 做两件事（顺序固定）：
///   ① 注册 interrupt 恢复函数（kRemoteA2aInterruptKind → 本 registry）
///   ② 启动后台 sweeper 线程
///
/// ── ★★ 为什么必须收敛成一个函数 ──────────────────────────────────────────
/// 本子系统有两条入口：`enable_remote_a2a`（单 manager 便捷入口 / 测试）与
/// `RemoteA2aRuntime::init`（进程级，生产路径）。接线序列曾被【复制两份】，
/// 结果生产路径漏掉了 resume handler 注册 ——
///   工具照常产出 interrupt，但 ReactEngine 的前置检查
///   (`has_resume_handler`) 必然失败，所有远端 input-required 一律
///   fail-closed 成"未注册中断恢复处理器"。
///   ⚠ 这个失效是静默的：日志里只有一句工具报错，没人会想到是接线漏了。
/// 因此新增任何进程级接线步骤都只能加在这里，绝不允许在入口处各写一遍。
///
/// ── ⚠ 调用时机：必须在「确认本 registry 是最终采用的那一个」之后 ────────
/// resume handler 【无法反注册】（InterruptCoordinator 只保留先注册的）。
/// 若在并发 init 的双检之前调用，败者可能先注册上，恢复路由就永久指向一个
/// 被丢弃的 registry —— 其 weak_ptr 失效后，用户的每一次确认都会收到
/// "注册表已不可用"。sweeper 可以 stop 回滚，handler 不能。
void activate_remote_a2a_registry(const std::shared_ptr<RemoteA2aRegistry>& reg);

/// 便捷入口：解析配置 → build → register_tools → activate。
///
/// 対标 tool_manager.cc:639-677 的 enable_mcp。
/// ★ 与 enable_mcp 一致的语义：工具发现失败【不】返回 Err，
///   只在「配置本身无效」或「一个远端都没构造成功」时返回 Err。
///
/// ⚠ 每次调用都会新建一个 registry（含独立的 store / sweeper）。
///   进程内长期运行的服务应改用 RemoteA2aRuntime（进程级单例）——
///   多个 store 会让回调按 handle 查不到任务（见 runtime.h 文件头）。
core::Result<int> enable_remote_a2a(tools::ToolManager& manager,
                                   const nlohmann::json& config_node,
                                   std::shared_ptr<RemoteA2aRegistry>* out_registry,
                                   infra::HttpClient::Ptr http = nullptr);

}  // namespace remote_a2a
}  // namespace hunterclaw
