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
    void start_sweeper(int64_t interval_ms = 60000);
    void stop_sweeper();

    /// handle → engine 反查。
    /// ★ 通过 store 查出 agent_name 再映射，而不是把引擎指针塞进 handle ——
    ///   后者会让 handle 变成不可序列化的东西（二期落盘时无法恢复）。
    std::shared_ptr<ExecutionEngine> engine_for_handle(const std::string& handle) const;

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

    mutable std::mutex mu_;
    /// agent_name → runtime
    std::map<std::string, RemoteAgentRuntime> runtimes_;
    std::vector<std::string> warnings_;
    bool built_ = false;

    std::thread sweeper_;
    std::atomic<bool> sweeping_{false};
    std::mutex sweep_mu_;
    std::condition_variable sweep_cv_;
};

/// 便捷入口：解析配置 → build → register_tools。
///
/// 対标 tool_manager.cc:639-677 的 enable_mcp。
/// ★ 与 enable_mcp 一致的语义：工具发现失败【不】返回 Err，
///   只在「配置本身无效」或「一个远端都没构造成功」时返回 Err。
core::Result<int> enable_remote_a2a(tools::ToolManager& manager,
                                   const nlohmann::json& config_node,
                                   std::shared_ptr<RemoteA2aRegistry>* out_registry,
                                   infra::HttpClient::Ptr http = nullptr);

}  // namespace remote_a2a
}  // namespace hunterclaw
