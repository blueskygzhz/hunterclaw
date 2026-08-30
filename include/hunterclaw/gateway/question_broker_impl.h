// ============================================================================
// question_broker_impl.h — QuestionBroker 的 gateway 侧实现
//
// 🎯 严格对齐 OpenClaw: gateway-question.ts 里那三个 gatewayCall
//    ("question.request" / "question.waitAnswer" / "question.resolve")
//    在 HunterClaw 里退化成对本类的直接方法调用。
//
// ★ 依赖方向: gateway → tools（实现 tools 侧声明的接口）。
//   ⚠ 反过来会成环: tools → gateway → agents → tools。
//
// ★★ 为什么不走真正的 RPC 自调用:
//   OpenClaw 的 harness 与 gateway 可以是两个进程，所以必须走 RPC。
//   HunterClaw 的 agent 与 gateway 在同一进程内，绕一圈 WebSocket
//   只会引入一次序列化与一次线程切换，并把"登记是否同步完成"
//   变成不确定 —— 而 PendingQuestionRegistry 的无 buffered 设计
//   恰恰依赖登记是同步的（见其头文件说明）。
// ============================================================================

#pragma once

#include "hunterclaw/gateway/question_manager.h"
#include "hunterclaw/tools/question_broker.h"

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace hunterclaw {
namespace gateway {

class GatewayQuestionBroker : public tools::QuestionBroker {
public:
    using BroadcastFn =
        std::function<void(const std::string& event, const nlohmann::json& payload)>;

    /// 是否存在能应答提问的客户端。
    /// ★ 用回调注入而非直接依赖 GatewayServer: 本类只需要一个 bool，
    ///   引入整个 server 会让单测必须起一个 gateway。
    using HasClientsFn = std::function<bool()>;

    GatewayQuestionBroker(std::shared_ptr<QuestionManager> manager,
                          BroadcastFn broadcast, HasClientsFn has_clients);

    std::string request_question(const std::vector<UserInputQuestion>& questions,
                                const std::string& session_key,
                                const std::string& agent_id,
                                const std::string& run_id,
                                int64_t timeout_ms,
                                const std::string& explicit_id = {}) override;

    tools::QuestionWaitResult wait_question_answer(
        const std::string& question_id, int64_t timeout_ms,
        const std::atomic<bool>* aborted = nullptr) override;

    bool resolve_question(const std::string& question_id,
                          const UserInputAnswers& answers,
                          const std::string& resolved_by) override;

    bool cancel_question(const std::string& question_id,
                        const std::string& resolved_by) override;

    bool has_question_clients() const override;

private:
    /// 广播收敛事件（answered / cancelled / expired）。
    /// ★ 抽成私有方法而非在三处各写一遍 —— 与 remote_a2a 两个出口共用判据
    ///   抽取同理: 散落多处必然漂移，而这里漂移的是"终端能不能把卡片收掉"。
    void broadcast_resolved(const std::string& question_id, QuestionStatus status,
                            const UserInputAnswers* answers);

    std::shared_ptr<QuestionManager> manager_;
    BroadcastFn broadcast_;
    HasClientsFn has_clients_;
};

}  // namespace gateway
}  // namespace hunterclaw
