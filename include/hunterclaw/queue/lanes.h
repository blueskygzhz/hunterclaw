#pragma once

#include <string>

namespace hunterclaw {
namespace queue {

// 命令队列通道
enum class CommandLane {
    Main,      // 主通道（串行执行，默认）
    Cron,      // 定时任务通道
    Subagent,  // 子代理通道
    Nested     // 嵌套通道
};

// 获取通道名称
inline const char* GetCommandLaneName(CommandLane lane) {
    switch (lane) {
        case CommandLane::Main:     return "main";
        case CommandLane::Cron:     return "cron";
        case CommandLane::Subagent: return "subagent";
        case CommandLane::Nested:   return "nested";
        default:                    return "unknown";
    }
}

// 从名称解析通道
inline CommandLane ParseCommandLane(const std::string& name) {
    if (name == "main")     return CommandLane::Main;
    if (name == "cron")     return CommandLane::Cron;
    if (name == "subagent") return CommandLane::Subagent;
    if (name == "nested")   return CommandLane::Nested;
    return CommandLane::Main;  // 默认
}

// ============================================================================
// Dynamic String Lane Helpers (对齐 OpenClaw pi-embedded-runner/lanes.ts)
// ============================================================================

// 解析 session lane 名称
// 对齐 OpenClaw: resolveSessionLane(key) -> "session:<key>"
// 每个 session 有独立的串行 Lane，保证同一 session 的请求串行执行
inline std::string ResolveSessionLane(const std::string& session_key) {
    if (session_key.empty()) {
        return "session:main";
    }
    // 已经是 session: 前缀则直接返回
    if (session_key.rfind("session:", 0) == 0) {
        return session_key;
    }
    return "session:" + session_key;
}

// 解析 global lane 名称
// 对齐 OpenClaw: resolveGlobalLane(lane?) -> "main" | "nested" | lane
// Cron 内部操作使用 nested 避免死锁
inline std::string ResolveGlobalLane(const std::string& lane = "") {
    if (lane.empty()) {
        return "main";
    }
    // Cron lane 内部操作使用 nested 避免死锁
    if (lane == "cron") {
        return "nested";
    }
    return lane;
}

}  // namespace queue
}  // namespace hunterclaw
