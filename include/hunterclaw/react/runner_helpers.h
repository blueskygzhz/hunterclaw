#pragma once

#include <functional>
#include <optional>
#include <regex>
#include <string>
#include <vector>

// 严格对齐 OpenClaw src/auto-reply/reply/agent-runner-reminder-guard.ts (66行)
// + src/auto-reply/reply/agent-runner-usage-line.ts (69行)
// Reminder commitment 检测 + Usage 统计格式化

namespace hunterclaw {
namespace react {

// ============================================================================
// Reminder Guard (对齐 agent-runner-reminder-guard.ts)
// ============================================================================

// 对齐: UNSCHEDULED_REMINDER_NOTE
extern const std::string UNSCHEDULED_REMINDER_NOTE;

// 对齐: hasUnbackedReminderCommitment (agent-runner-reminder-guard.ts:13-22)
bool has_unbacked_reminder_commitment(const std::string& text);

// 对齐: appendUnscheduledReminderNote (agent-runner-reminder-guard.ts:49-65)
// 修改 payload text，在首个包含 reminder commitment 的文本后追加提示
std::string append_unscheduled_reminder_note(const std::string& text);

// ============================================================================
// Usage Line (对齐 agent-runner-usage-line.ts)
// ============================================================================

struct UsageLineParams {
    std::optional<int64_t> input_tokens;
    std::optional<int64_t> output_tokens;
    std::optional<int64_t> cache_read_tokens;
    std::optional<int64_t> cache_write_tokens;
    bool show_cost = false;
    // cost per million tokens
    struct CostConfig {
        double input = 0.0;
        double output = 0.0;
        double cache_read = 0.0;
        double cache_write = 0.0;
    };
    std::optional<CostConfig> cost_config;
};

// 对齐: formatResponseUsageLine (agent-runner-usage-line.ts:4-45)
std::optional<std::string> format_response_usage_line(const UsageLineParams& params);

// 对齐: appendUsageLine (agent-runner-usage-line.ts:47-68)
std::string append_usage_line(const std::string& text, const std::string& line);

// ============================================================================
// Audio Payload Detection (对齐 agent-runner-helpers.ts:12-16)
// ============================================================================

// 对齐: isAudioPayload — checks if any media URL has audio extension
bool is_audio_payload(const std::vector<std::string>& media_urls);

// ============================================================================
// Verbose Gate (对齐 agent-runner-helpers.ts:18-55)
// ============================================================================

using VerboseGate = std::function<bool()>;

// 对齐: createShouldEmitToolResult — returns true unless verbose == "off"
VerboseGate create_should_emit_tool_result(const std::string& resolved_verbose_level);

// 对齐: createShouldEmitToolOutput — returns true only when verbose == "full"
VerboseGate create_should_emit_tool_output(const std::string& resolved_verbose_level);

// ============================================================================
// Session Hints (对齐 body.ts:13-53)
// ============================================================================

// 对齐: applySessionHints — prepend abort hint to body if aborted
std::string apply_session_hints(
    const std::string& base_body,
    bool aborted_last_run
);

}  // namespace react
}  // namespace hunterclaw
