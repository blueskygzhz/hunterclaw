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

}  // namespace queue
}  // namespace hunterclaw
