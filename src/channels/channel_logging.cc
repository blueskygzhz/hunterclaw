// ✅ 严格对齐 OpenClaw: src/channels/logging.ts (34行)
// 通道日志辅助函数实现

#include "hunterclaw/channels/channel_logging.h"

namespace hunterclaw {
namespace channels {

// ✅ 对齐 OpenClaw: logInboundDrop (L3-11)
void log_inbound_drop(LogFn log, const std::string& channel,
                       const std::string& reason,
                       const std::string& target) {
    std::string target_str = target.empty() ? "" : " target=" + target;
    log(channel + ": drop " + reason + target_str);
}

// ✅ 对齐 OpenClaw: logTypingFailure (L13-23)
void log_typing_failure(LogFn log, const std::string& channel,
                         const std::string& error,
                         const std::string& target,
                         const std::string& action) {
    std::string target_str = target.empty() ? "" : " target=" + target;
    std::string action_str = action.empty() ? "" : " action=" + action;
    log(channel + " typing" + action_str + " failed" + target_str + ": " + error);
}

// ✅ 对齐 OpenClaw: logAckFailure (L25-33)
void log_ack_failure(LogFn log, const std::string& channel,
                      const std::string& error,
                      const std::string& target) {
    std::string target_str = target.empty() ? "" : " target=" + target;
    log(channel + " ack cleanup failed" + target_str + ": " + error);
}

}  // namespace channels
}  // namespace hunterclaw
