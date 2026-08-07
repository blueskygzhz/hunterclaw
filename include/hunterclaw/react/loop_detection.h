#pragma once
// ============================================================================
// loop_detection.h
// ✅ 严格对齐 OpenClaw: src/agents/tool-loop-detection.ts
// ============================================================================

#include "hunterclaw/core/json.h"
#include "hunterclaw/tools/tool.h"
#include <string>
#include <vector>
#include <memory>
#include <chrono>

namespace hunterclaw {
namespace react {

// ── LoopDetectorKind ──────────────────────────────────────────────────────────
// ✅ 对齐 OpenClaw: LoopDetectorKind
enum class LoopDetectorKind {
    GenericRepeat,         // "generic_repeat"
    KnownPollNoProgress,   // "known_poll_no_progress"
    GlobalCircuitBreaker,  // "global_circuit_breaker"
    PingPong,// "ping_pong"
    UnknownToolRepeat,     // "unknown_tool_repeat" (对齐 OpenClaw)
    ArgumentChurn          // "argument_churn" (对齐 OpenClaw, warning-only)
};

// ── LoopDetectionResult ───────────────────────────────────────────────────────
// ✅ 对齐 OpenClaw: LoopDetectionResult (union type: { stuck:false } | { stuck:true, ... })
struct LoopDetectionResult {
    bool stuck = false;

    enum class Level {
        None,
        Warning,   // ✅ "warning"
        Critical   // ✅ "critical"
    } level = Level::None;

    LoopDetectorKind detector = LoopDetectorKind::GenericRepeat;
    int count = 0;
    std::string message;
    std::string paired_tool_name;  // ✅ OpenClaw: pairedToolName (ping_pong)
    std::string warning_key;       // ✅ OpenClaw: warningKey
    // 🎯 对齐 OpenClaw: livenessSignal?: "argument_churn" —
    //   附加在 poll/ping_pong warning 上的活性信号，供上层判定"虽在循环但参数在变化"。
    //   空表示未设置。
    std::string liveness_signal;
};

// ── ToolCallRecord ────────────────────────────────────────────────────────────
// ✅ 对齐 OpenClaw: ToolCallRecord (diagnostic-session-state.ts)
// { toolName, argsHash, toolCallId?, resultHash?, timestamp }
// argsHash = hashToolCall(toolName, params) = "${toolName}:${SHA256(stableStringify(params))}"
struct ToolCallRecord {
    std::string tool_name;
    std::string args_hash;       // ✅ 对齐 OpenClaw: argsHash = toolName:SHA256(stableStringify(params))
    std::string tool_call_id;    // optional
    std::string result_hash;     // 由 record_tool_call_outcome 回填
    // 🎯 对齐 OpenClaw ToolCallRecord.unknownToolName: 当 outcome 是
    //   "unknown/unavailable tool" 错误时，从 error 解析出的工具名，用于
    //   unknown_tool_repeat 检测器。
    std::string unknown_tool_name;
    // 🎯 对齐 OpenClaw ToolCallRecord.noProgress: 该次调用的 outcome 被判定为
    //   "无进展"（如 error、write unchanged、poll unchanged）。argument_churn
    //   检测器依赖此标志。
    bool no_progress = false;
    int64_t timestamp_ms = 0;
};

// ── LoopDetectionConfig ───────────────────────────────────────────────────────
// ✅ 对齐 OpenClaw: ResolvedLoopDetectionConfig
// 默认值与 OpenClaw DEFAULT_LOOP_DETECTION_CONFIG 完全一致
struct LoopDetectionConfig {
    // ✅ 对齐 OpenClaw: enabled=false (默认关闭，需显式开启)
    bool enabled = false;

    // ✅ 对齐 OpenClaw: historySize=30
    int history_size = 30;

    // ✅ 对齐 OpenClaw: warningThreshold=10, criticalThreshold=20, globalCircuitBreakerThreshold=30
    int warning_threshold = 10;
    int critical_threshold = 20;
    int global_circuit_breaker_threshold = 30;
    // 🎯 对齐 OpenClaw: unknownToolThreshold=10 (UNKNOWN_TOOL_THRESHOLD)
    int unknown_tool_threshold = 10;

    // ✅ 对齐 OpenClaw: detectors (默认全开)
    struct Detectors {
        bool generic_repeat = true;
        bool known_poll_no_progress = true;
        bool ping_pong = true;
    } detectors;
};

// ── LoopDetectionState ────────────────────────────────────────────────────────
// ✅ 对齐 OpenClaw: SessionState.toolCallHistory[]
// HunterClaw 将 history 和检测逻辑封装在此类中
class LoopDetectionState {
public:
    static std::shared_ptr<LoopDetectionState> create(
        const LoopDetectionConfig& config = LoopDetectionConfig{}
    );

    // ✅ 对齐 OpenClaw: detectToolCallLoop(state, toolName, params, config?)
    // 在工具执行前调用，返回 stuck=false 或 stuck=true+level
    LoopDetectionResult detect_tool_call_loop(
        const std::string& tool_name,
        const core::JsonValue& params
    );

    // ✅ 对齐 OpenClaw: recordToolCall(state, toolName, params, toolCallId?)
    // 在工具执行前（detect 之后）调用，记录到滑动窗口
    void record_tool_call(
        const std::string& tool_name,
        const core::JsonValue& params,
        const std::string& tool_call_id = ""
    );

    // ✅ 对齐 OpenClaw: recordToolCallOutcome(state, { toolName, toolParams, toolCallId?, result?, error? })
    // 在工具执行后调用，回填 result_hash
    void record_tool_call_outcome(
        const std::string& tool_name,
        const tools::ToolResult& result,
        const std::string& error,
        const std::string& tool_call_id,
        const core::JsonValue& params
    );

    // 向后兼容的简化版本（不带 tool_call_id 和 params）
    void record_tool_call_outcome(
        const std::string& tool_name,
        const tools::ToolResult& result,
        const std::string& error = ""
    );

    // 重置所有状态（每次 run 开始时调用）
    void reset();

    // 获取配置
    const LoopDetectionConfig& config() const { return config_; }

    // 统计信息
    int total_tool_calls() const { return static_cast<int>(history_.size()); }
    std::vector<std::string> recent_tool_names(int count = 10) const;

    // 提供 history 的只读访问（用于 create_loop_summary）
    const std::vector<ToolCallRecord>& history() const { return history_; }

private:
    explicit LoopDetectionState(const LoopDetectionConfig& config);

    LoopDetectionConfig config_;
    std::vector<ToolCallRecord> history_;  // 滑动窗口，最多 history_size 条
};

// ── 辅助函数 ──────────────────────────────────────────────────────────────────
namespace loop_detection {

// 创建循环摘要消息
std::string create_loop_summary(
    const LoopDetectionResult& result,
    const std::vector<ToolCallRecord>& recent_calls
);

// ✅ 对齐 OpenClaw: isKnownPollToolCall(toolName, params)
// 带 params 版本：检查 command_status 或 process.poll/process.log
bool is_known_polling_tool(
    const std::string& tool_name,
    const core::JsonValue& params
);

// 向后兼容的无参版本（仅检查工具名）
bool is_known_polling_tool(const std::string& tool_name);

} // namespace loop_detection

} // namespace react
} // namespace hunterclaw
