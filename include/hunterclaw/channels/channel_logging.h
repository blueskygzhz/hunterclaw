// ✅ 严格对齐 OpenClaw: src/channels/logging.ts (34行)
// 通道日志辅助函数 — logInboundDrop / logTypingFailure / logAckFailure
#pragma once

#include <functional>
#include <string>

namespace hunterclaw {
namespace channels {

// ✅ 对齐 OpenClaw: LogFn type
using LogFn = std::function<void(const std::string&)>;

// ✅ 对齐 OpenClaw: logInboundDrop (L3-11)
// 记录入站消息被丢弃
void log_inbound_drop(LogFn log, const std::string& channel,
                       const std::string& reason,
                       const std::string& target = "");

// ✅ 对齐 OpenClaw: logTypingFailure (L13-23)
// 记录打字指示器操作失败
void log_typing_failure(LogFn log, const std::string& channel,
                         const std::string& error,
                         const std::string& target = "",
                         const std::string& action = "");

// ✅ 对齐 OpenClaw: logAckFailure (L25-33)
// 记录 ack 清理失败
void log_ack_failure(LogFn log, const std::string& channel,
                      const std::string& error,
                      const std::string& target = "");

}  // namespace channels
}  // namespace hunterclaw
