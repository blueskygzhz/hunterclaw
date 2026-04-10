#pragma once

#include "hunterclaw/react/abort_primitives.h"
#include "hunterclaw/react/abort_cutoff.h"
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

// 严格对齐 OpenClaw src/auto-reply/reply/abort.ts (345行)
// 完整 abort 系统：格式化回复、子代理级联停止、session abort 流程

namespace hunterclaw {
namespace react {

// ============================================================================
// formatAbortReplyText (对齐 abort.ts:93-98)
// ============================================================================
std::string format_abort_reply_text(int stopped_subagents = 0);

// ============================================================================
// AbortResult (对齐 abort.ts 中 handleAbortCommand 返回结构)
// ============================================================================
struct AbortResult {
    bool aborted = false;
    std::string reply_text;
    int stopped_subagents = 0;
    std::optional<AbortCutoff> cutoff;
};

// ============================================================================
// AbortSessionParams (对齐 abort.ts:100-180 handleAbortCommand 参数)
// ============================================================================
struct AbortSessionParams {
    std::string session_key;
    std::string target_session_key;  // 可以 abort 其他 session
    std::string command_text;
    int64_t timestamp_ms = 0;

    // 回调：中止嵌入式 PI 运行 (对齐 abortEmbeddedPiRun)
    std::function<void(const std::string& session_key)> abort_embedded_run;

    // 回调：列举子代理运行 (对齐 listSubagentRunsForController)
    std::function<std::vector<std::string>(const std::string& controller_session_key)>
        list_subagent_runs;

    // 回调：终止子代理运行 (对齐 markSubagentRunTerminated)
    std::function<void(const std::string& child_session_key)> terminate_subagent;

    // 回调：清除 session 队列 (对齐 clearSessionQueues)
    std::function<void(const std::string& session_key)> clear_session_queues;
};

// ============================================================================
// handleAbortCommand (对齐 abort.ts:100-345)
// 处理 /abort 或 /stop 命令
// ============================================================================
AbortResult handle_abort_command(const AbortSessionParams& params);

// ============================================================================
// stopSubagentsForRequester (对齐 abort.ts:220-260)
// 级联停止所有子代理
// ============================================================================
struct StopSubagentsResult {
    int stopped = 0;
};

StopSubagentsResult stop_subagents_for_requester(
    const std::string& requester_session_key,
    const std::function<std::vector<std::string>(const std::string&)>& list_subagent_runs,
    const std::function<void(const std::string&)>& abort_embedded_run,
    const std::function<void(const std::string&)>& terminate_subagent
);

}  // namespace react
}  // namespace hunterclaw
