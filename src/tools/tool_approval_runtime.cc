// ============================================================================
// tool_approval_runtime.cc
// 见 tool_approval_runtime.h 的对齐说明。
// ============================================================================

#include "hunterclaw/tools/tool_approval_runtime.h"

#include "hunterclaw/core/logger.h"

#include <spdlog/spdlog.h>

namespace hunterclaw {
namespace tools {

namespace {
std::shared_ptr<spdlog::logger> log() {
    return core::get_logger("tool_approval");
}
}  // namespace

ToolApprovalRuntime& ToolApprovalRuntime::instance() {
    // ★ 函数内 static: C++11 起初始化是线程安全的,
    //   且天然避免了跨 TU 的静态初始化顺序问题
    //   (main 在 gateway 就绪后注入, 而某些 TU 的静态对象可能更早构造)。
    static ToolApprovalRuntime inst;
    return inst;
}

bool ToolApprovalRuntime::set_broker(std::shared_ptr<ToolApprovalBroker> broker) {
    std::lock_guard<std::mutex> lk(mu_);
    if (broker_) {
        // ⚠ 见头文件: 重复注入会让先前已发起的审批变成孤儿。
        //   记 error 而非抛异常 —— 一处重复注入不该让整个 daemon 起不来,
        //   但必须留下能查到的痕迹(否则表现为"审批莫名超时")。
        log()->error("tool approval broker already set; ignoring re-registration");
        return false;
    }
    if (!broker) {
        // 显式拒绝注入空值: 那与"没注入"无法区分, 却会占掉唯一的注入机会。
        log()->error("refusing to set a null tool approval broker");
        return false;
    }
    broker_ = std::move(broker);
    log()->info("tool approval broker registered");
    return true;
}

std::shared_ptr<ToolApprovalBroker> ToolApprovalRuntime::broker() const {
    std::lock_guard<std::mutex> lk(mu_);
    return broker_;
}

bool ToolApprovalRuntime::has_broker() const {
    std::lock_guard<std::mutex> lk(mu_);
    return static_cast<bool>(broker_);
}

void ToolApprovalRuntime::reset_for_testing() {
    std::lock_guard<std::mutex> lk(mu_);
    broker_.reset();
}

}  // namespace tools
}  // namespace hunterclaw
