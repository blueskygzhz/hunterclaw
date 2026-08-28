// ============================================================================
// tool_approval_runtime.h — 工具审批通道的进程级持有者
//
// 🎯 严格对齐 OpenClaw approval-gateway-runtime-context.ts:
//     resolveGlobalSingleton<AsyncLocalStorage<GatewayNativeApprovalRuntime>>(...)
//   OpenClaw 把审批 runtime 放在【进程级单例】里, 而【不是】塞进
//   HookContext / 工具上下文。本类是该机制的 C++ 对应物。
//
// ★★ 为什么必须是进程级单例, 不能走 ToolContext:
//
//   ① ToolContext 是【按值拷贝】的, 且 agent_instance.cc 为【每个会话】
//      新建一个 ToolManager。若把 broker 放进去, N 个会话会各持一个 ——
//      产生 N 个待审列表, 而客户端只有一个 approval.resolve 入口。
//      决策打到哪个列表取决于实现细节, 其余列表的请求【永远等不到决策】
//      → 表现为随机超时而非报错, 是最难定位的一类故障。
//
//   ② ToolContext 的依赖注入区(http_client / gateway_client /
//      node_registry / llm_client)【四个字段全部从未被赋值】——
//      gateway_client 面对的正是同一个"工具要访问 gateway"的问题,
//      而它的结局是字段留着没人填。再加一个只会得到第五个死字段。
//
// ★ 本头文件【只依赖 ToolApprovalBroker 接口】, 不引用任何 gateway 类型:
//   实现类(GatewayToolApprovalBroker)在 gateway 侧创建后注入。
//   否则会成环: tools → gateway → agents → tools。
// ============================================================================

#pragma once

#include "hunterclaw/tools/before_tool_call_policy.h"  // ToolApprovalBroker

#include <memory>
#include <mutex>

namespace hunterclaw {
namespace tools {

class ToolApprovalRuntime {
public:
    static ToolApprovalRuntime& instance();

    /// 注入审批通道（由 main 在 gateway 就绪后调用一次）。
    ///
    /// ⚠⚠ 只允许设置【一次】。重复设置意味着有两处在注入,
    ///   而后设的会让先前已发起的审批变成孤儿 ——
    ///   等待方仍在【旧 broker】上等决策, 新决策却落到新 broker,
    ///   于是那批审批必然等满超时。
    ///   第二次调用会被忽略并记 error, 不抛异常(不该因此让进程起不来)。
    ///
    /// @return 是否真正设置成功（false = 已被设置过，本次忽略）
    bool set_broker(std::shared_ptr<ToolApprovalBroker> broker);

    /// 取当前审批通道。
    ///
    /// ★ 返回 shared_ptr 而非裸指针: 调用方(策略链)会持有它跨越
    ///   一次阻塞等待(可长达 120s)。返回裸指针的话, 若期间单例被替换,
    ///   等待中的调用就在访问已销毁对象。
    /// ⚠ 为空表示审批面不可用 —— Stage 1 会据此【拒绝执行】而非放行。
    std::shared_ptr<ToolApprovalBroker> broker() const;

    /// 是否已注入（诊断/启动日志用）
    bool has_broker() const;

    /// 仅供测试: 清空已注入的 broker。
    /// ⚠ 生产代码【不要】调用 —— 清空会让在途审批的等待方拿不到决策。
    void reset_for_testing();

private:
    ToolApprovalRuntime() = default;

    mutable std::mutex mu_;
    std::shared_ptr<ToolApprovalBroker> broker_;
};

}  // namespace tools
}  // namespace hunterclaw
