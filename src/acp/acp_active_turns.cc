// 严格对齐 OpenClaw:
//   - src/acp/control-plane/active-turns.ts
//   - src/acp/control-plane/manager.runtime-resume-state.ts

#include "hunterclaw/acp/acp_active_turns.h"

#include <algorithm>
#include <mutex>
#include <regex>
#include <set>

namespace hunterclaw {
namespace acp {

namespace {

/// 对齐 OC: normalizeActorKey —— trim + lowercase
std::string normalize_actor_key_local(const std::string& session_key) {
    size_t begin = 0;
    while (begin < session_key.size() &&
           static_cast<unsigned char>(session_key[begin]) <= 0x20) {
        ++begin;
    }
    size_t end = session_key.size();
    while (end > begin && static_cast<unsigned char>(session_key[end - 1]) <= 0x20) {
        --end;
    }
    std::string out = session_key.substr(begin, end - begin);
    std::transform(out.begin(), out.end(), out.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}

/// 对齐 OC: resolveGlobalSingleton(ACP_ACTIVE_TURN_STATE_KEY)
/// C++ 用函数内 static 保证线程安全初始化；mutex 保护 set（真源单线程无需）。
struct ActiveTurnRegistry {
    std::mutex mutex;
    std::set<std::string> active_turn_keys;
};

ActiveTurnRegistry& registry() {
    static ActiveTurnRegistry instance;
    return instance;
}

} // namespace

void mark_acp_turn_active(const std::string& session_key) {
    if (session_key.empty()) return;
    auto& reg = registry();
    std::lock_guard<std::mutex> lock(reg.mutex);
    reg.active_turn_keys.insert(normalize_actor_key_local(session_key));
}

void clear_acp_turn_active(const std::string& session_key) {
    if (session_key.empty()) return;
    auto& reg = registry();
    std::lock_guard<std::mutex> lock(reg.mutex);
    reg.active_turn_keys.erase(normalize_actor_key_local(session_key));
}

bool is_acp_turn_active(const std::string& session_key) {
    if (session_key.empty()) return false;
    auto& reg = registry();
    std::lock_guard<std::mutex> lock(reg.mutex);
    return reg.active_turn_keys.count(normalize_actor_key_local(session_key)) > 0;
}

bool is_recoverable_manager_acpx_exit_error(const std::string& message) {
    // 对齐 OC: /^acpx exited with (code \d+|signal [a-z0-9]+)/i.test(message.trim())
    size_t begin = 0;
    while (begin < message.size() && static_cast<unsigned char>(message[begin]) <= 0x20) {
        ++begin;
    }
    size_t end = message.size();
    while (end > begin && static_cast<unsigned char>(message[end - 1]) <= 0x20) {
        --end;
    }
    const std::string trimmed = message.substr(begin, end - begin);

    static const std::regex acpx_exit_re(
        "^acpx exited with (code [0-9]+|signal [a-z0-9]+)",
        std::regex::icase | std::regex::optimize);
    return std::regex_search(trimmed, acpx_exit_re);
}

bool is_recoverable_missing_persistent_session_error(const AcpRuntimeError& error) {
    // 对齐 OC：按结构化 detailCode 判定，不解析人类可读文本。
    // C++ 侧 AcpRuntimeError 没有 cause 链，故不需要真源的 depth<8 环检测。
    return error.detail_code.has_value() &&
           *error.detail_code == ACP_SESSION_RESUME_REQUIRED_DETAIL_CODE;
}

FreshHandleRetryDecision prepare_fresh_runtime_handle_retry(
    int attempt,
    const AcpRuntimeError& error,
    bool saw_turn_output,
    bool has_runtime,
    bool is_persistent_mode)
{
    FreshHandleRetryDecision decision;

    // 对齐 OC: if (params.attempt > 0 || params.sawTurnOutput) return false
    // 一次性重试：attempt>0 说明已重试过；sawTurnOutput 说明重试会重复输出。
    if (attempt > 0 || saw_turn_output) {
        return decision;
    }

    // 分支 1：acpx 进程早退 —— 换新 handle 直接重试，无需清 resume 状态
    if (is_recoverable_manager_acpx_exit_error(error.what())) {
        decision.should_retry = true;
        decision.needs_clear_resume_state = false;
        return decision;
    }

    // 分支 2：persistent 会话的 resume 目标已失效
    if (!has_runtime || !is_persistent_mode ||
        !is_recoverable_missing_persistent_session_error(error)) {
        return decision;
    }

    decision.should_retry = true;
    decision.needs_clear_resume_state = true;
    return decision;
}

} // namespace acp
} // namespace hunterclaw
