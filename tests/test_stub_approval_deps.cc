// ============================================================================
// test_stub_approval_deps.cc — test_approval_gate 的链接期占位实现
//
// ★ 为什么需要 stub 而不是直接链真实实现:
//   before_tool_call_policy.cc 依赖三个跨子系统符号:
//     · core::get_logger        (在 hunterclaw_core, 其实现文件难以单独摘出)
//     · core::utils::*          (uuid / 时间戳)
//     · security::is_gateway_owner_only_core_tool
//     · ToolHookManager 的两个方法
//   把它们的真实实现全链进来会拖入 llm/sessions/memory 等一大串,
//   而本套测试只关心 approval gate 的分支逻辑。
//
// ⚠ 这些 stub 的【行为必须与真实实现在被测路径上一致】, 否则测试会失真:
//   · is_gateway_owner_only_core_tool 返回 false —— 测试用
//     ctx.tool_is_owner_only 显式控制 owner-only, 不依赖名单。
//   · has_before_tool_call_hooks 返回 false —— 测试不覆盖插件 hook
//     (那是 Stage 3, 有独立测试)。
//   · generate_uuid 必须【真的唯一】, 否则同一进程内多次审批的 id 会撞,
//     ApprovalManager 用 id 做 map key, 撞了会让"批准 A"误解为"批准 B"。
// ============================================================================

#include "hunterclaw/core/logger.h"
#include "hunterclaw/core/utils.h"
#include "hunterclaw/security/dangerous_tools.h"
#include "hunterclaw/tools/tool_hooks.h"

#include <spdlog/sinks/null_sink.h>
#include <spdlog/spdlog.h>

#include <atomic>
#include <chrono>
#include <mutex>
#include <sstream>

namespace hunterclaw {
namespace core {

std::shared_ptr<spdlog::logger> get_logger(const std::string& name) {
    // ⚠ 用 null sink: 测试输出不该被审批日志淹没,
    //   但仍需返回【非空】logger —— 被测代码会无条件 ->warn()/->info(),
    //   返回 nullptr 会直接段错误。
    static std::mutex mu;
    std::lock_guard<std::mutex> lk(mu);
    auto existing = spdlog::get(name);
    if (existing) return existing;
    auto lg = std::make_shared<spdlog::logger>(
        name, std::make_shared<spdlog::sinks::null_sink_mt>());
    spdlog::register_logger(lg);
    return lg;
}

namespace utils {

int64_t current_timestamp_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

std::string generate_uuid() {
    // ★ 必须真唯一 —— ApprovalManager 以 id 为 map key。
    //   若两次审批撞 id, "批准 A" 会被当成 "批准 B", 而这类错误
    //   在测试里表现为随机失败, 极难定位。
    static std::atomic<uint64_t> seq{1};
    std::ostringstream ss;
    ss << "test-uuid-" << seq.fetch_add(1);
    return ss.str();
}

}  // namespace utils
}  // namespace core

namespace security {

bool is_gateway_owner_only_core_tool(const std::string& /*tool_name*/) {
    // 测试用 ctx.tool_is_owner_only 显式控制 owner-only 分支,
    // 不依赖内置名单 —— 名单变化不该影响 approval gate 的测试结果。
    return false;
}

}  // namespace security

namespace tools {

bool ToolHookManager::has_before_tool_call_hooks() const { return false; }

std::optional<plugins::PluginHookBeforeToolCallResult>
ToolHookManager::run_before_tool_call(
    const plugins::PluginHookBeforeToolCallEvent& /*event*/,
    const plugins::PluginHookToolContext& /*ctx*/) {
    return std::nullopt;
}

}  // namespace tools
}  // namespace hunterclaw
