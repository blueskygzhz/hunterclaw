#pragma once

#include "hunterclaw/react/typing_mode.h"
#include "hunterclaw/react/reply_operation.h"
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

// 严格对齐 OpenClaw src/auto-reply/reply/followup-runner.ts (404行)
// Followup turn 执行器 — 处理队列中等待的消息

namespace hunterclaw {
namespace react {

// ============================================================================
// FollowupRunPayload (简化的 payload 结构)
// ============================================================================
struct FollowupRunPayload {
    std::string text;
    std::optional<std::string> media_url;
    std::vector<std::string> media_urls;
    bool is_error = false;
    bool is_reasoning = false;
    bool is_compaction_notice = false;
};

// ============================================================================
// FollowupRunItem (对齐 queue/types.ts FollowupRun 简化版)
// ============================================================================
struct FollowupRunItem {
    std::string prompt;
    std::optional<std::string> message_id;
    std::optional<std::string> summary_line;
    int64_t enqueued_at = 0;

    // 路由信息
    std::optional<std::string> originating_channel;
    std::optional<std::string> originating_to;
    std::optional<std::string> originating_account_id;
    std::optional<std::string> originating_thread_id;

    // Run 参数
    struct RunParams {
        std::string agent_id;
        std::string session_id;
        std::string session_key;
        std::string session_file;
        std::string workspace_dir;
        std::string provider;
        std::string model;
        std::optional<std::string> auth_profile_id;
        int timeout_ms = 60000;
        std::string block_reply_break = "text_end";
    } run;
};

// ============================================================================
// FollowupRunnerParams (对齐 followup-runner.ts:38-59)
// ============================================================================
struct FollowupRunnerParams {
    TypingController* typing = nullptr;
    TypingMode typing_mode = TypingMode::Never;
    bool is_heartbeat = false;
    std::string session_key;
    std::string default_model;
    int agent_cfg_context_tokens = 0;

    // 回调: 执行 agent 运行
    std::function<std::vector<FollowupRunPayload>(const FollowupRunItem& item)> run_agent;

    // 回调: 发送 payload
    std::function<void(const FollowupRunPayload& payload, const FollowupRunItem& item)> send_payload;

    // 回调: 路由到原始频道
    std::function<bool(const FollowupRunPayload& payload, const FollowupRunItem& item)> route_to_originating;

    // AbortSignal
    std::shared_ptr<std::atomic<bool>> abort_signal;
};

// ============================================================================
// createFollowupRunner (对齐 followup-runner.ts:38)
// 返回一个可调用对象，每次调用处理一个 followup turn
// ============================================================================
std::function<void(const FollowupRunItem& item)> create_followup_runner(
    const FollowupRunnerParams& params
);

}  // namespace react
}  // namespace hunterclaw
