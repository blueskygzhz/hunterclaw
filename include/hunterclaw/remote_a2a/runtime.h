#pragma once

// ============================================================================
// remote_a2a/runtime.h
// 进程级 Remote A2A 运行时持有者。
//
// ── 为什么必须有这一层 ────────────────────────────────────────────────────
// 生命周期不匹配：
//   · ToolManager  是【每会话】创建的（agent_instance.cc:183）
//   · PendingTaskStore / CallbackEndpoint 必须【进程级】：
//       - 回调路由只挂一次，要能查到【任意会话】的 handle
//       - 让渡任务跨会话轮次存活（模型在下一轮才来取结果）
//       - 后台 sweeper 线程只该有一个
//
// 若把 registry 建在 ToolManager 里，会得到：
//   ① N 个会话 → N 个 store → 回调按 handle 查不到（各 store 互不可见）
//   ② N 个 sweeper 线程
//   ③ N 份卡缓存 → 每个会话都去拉一遍卡
//
// 因此：registry 进程级构造一次；每会话只做「把工具注册进这个会话的
// ToolManager」这一件事（工具本身很轻，持有共享的 client/engine）。
//
// ── ★ 会话隔离由 ToolContext 保证，不由 store 隔离保证 ────────────────────
// store 是共享的，但每个会话注册的工具实例带着【自己的 session_key】
// （agent_tool.cc 构造函数从 ToolContext 取）。
// 跨会话读取由 check_handle_ownership 拦下（agent_tool.cc）。
// 所以「共享 store」不等于「跨会话可见」—— 隔离在工具层，不在存储层。
//
// ── 初始化顺序约束 ────────────────────────────────────────────────────────
//   init()                    ← 进程启动，读配置
//   attach_to_gateway()       ← HttpGateway 就绪后（可选，仅回调需要）
//   register_into(tool_mgr)   ← 每个会话创建 ToolManager 后
//
// ⚠ attach_to_gateway() 必须在 gateway->start() 【之前】调用：
//   httplib 的路由表在 listen 之后不接受新注册（register_route 会写
//   http_server_ 的 handler map，与请求分发线程竞争）。
//
// ── 并发原语约束（项目级）────────────────────────────────────────────────
// ✅ mutex / atomic     ❌ future / promise / async
// ============================================================================

#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "hunterclaw/core/types.h"
#include "hunterclaw/remote_a2a/agent_registry.h"
#include "hunterclaw/tools/tool_manager.h"

namespace hunterclaw {
namespace remote_a2a {

/// 进程级单例。
///
/// ★ 用单例而非依赖注入的理由：接线点分散在三处（main 读配置、
///   gateway 启动、每会话创建 ToolManager），且中间隔着现有代码
///   （agent_instance.cc 拿不到 main 的局部变量）。
///   把它做成显式单例比在三条调用链上各加一个参数改动小得多。
class RemoteA2aRuntime {
public:
    static RemoteA2aRuntime& instance();

    /// 初始化（幂等）。
    ///
    /// @param config_node  配置里的 remoteA2a 节点（缺失即视为未启用）
    ///
    /// ★ 返回 Err 只在【配置本身无效】时；远端离线不算错误
    ///   （卡拉取失败时工具仍注册，见 agent_registry.cc）。
    /// ★ 未启用时返回 Ok 且 enabled()==false —— 调用方无需特殊处理。
    core::Result<int> init(const nlohmann::json& config_node);

    /// 环境变量强制禁用检查（対标 agent_instance.cc:193 的 HUNTERCLAW_MCP=0）。
    /// HUNTERCLAW_REMOTE_A2A=0 时 init() 直接返回未启用。
    static bool disabled_by_env();

    bool enabled() const;

    /// 把工具注册进某个会话的 ToolManager。
    ///
    /// ⚠ 必须在 init() 之后。未启用时返回 Ok(0)，不是错误。
    /// ⚠ 每个会话都要调一次 —— 工具实例携带该会话的 session_key。
    core::Result<int> register_into(tools::ToolManager& manager);

    /// 挂载回调路由（可选，仅当有 agent 启用 enableCallback）。
    ///
    /// ★ 这里【不】直接调 mount_remote_a2a_callback ——
    ///   那会把 httplib 拖进本头文件的所有使用者。
    ///   改为由调用方（已经依赖 gateway 的那一层）传入挂载函数。
    ///
    /// @param mounter 实际执行挂载的函数；返回是否挂载成功
    /// @return 是否已挂载（未启用回调时返回 false，这不是错误）
    bool attach_callback(
        const std::function<bool(const std::shared_ptr<RemoteA2aRegistry>&)>& mounter);

    /// 是否需要挂载回调路由（调用方据此决定是否要拿 gateway）
    bool callback_needed() const;

    std::shared_ptr<RemoteA2aRegistry> registry() const;

    /// 初始化与构造期的全部警告（供启动日志打印）
    std::vector<std::string> warnings() const;

    /// 停止后台线程（进程退出时调用）。幂等。
    void shutdown();

private:
    RemoteA2aRuntime() = default;
    RemoteA2aRuntime(const RemoteA2aRuntime&) = delete;
    RemoteA2aRuntime& operator=(const RemoteA2aRuntime&) = delete;

    mutable std::mutex mu_;
    std::shared_ptr<RemoteA2aRegistry> registry_;
    std::vector<std::string> warnings_;
    bool inited_ = false;
    bool enabled_ = false;
    bool callback_mounted_ = false;
};

}  // namespace remote_a2a
}  // namespace hunterclaw
