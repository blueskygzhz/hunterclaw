// 严格对齐 OpenClaw: src/acp/control-plane/manager.turn-timeout.ts

#include "hunterclaw/acp/acp_turn_timeout.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <future>
#include <thread>

#include <spdlog/spdlog.h>

namespace hunterclaw {
namespace acp {

namespace {

/// 对齐 OC: logVerbose
void log_turn_timeout_verbose(const std::string& message) {
    auto logger = spdlog::get("acp_manager");
    if (!logger) logger = spdlog::get("");
    if (logger) {
        logger->warn("acp-manager: {}", message);
    }
}

/// 对齐 OC: clampTimerTimeoutMs(value, minMs)
/// 真源语义：非有限值 → undefined（调用方退化为无限等待）；否则夹在 [minMs, MAX_SAFE]。
int64_t clamp_timer_timeout_ms(int64_t value, int64_t min_ms) {
    if (value < min_ms) return min_ms;
    if (value > ACP_MAX_SAFE_TIMEOUT_MS) return ACP_MAX_SAFE_TIMEOUT_MS;
    return value;
}

/// 对齐 OC: awaitCleanupWithGrace
/// 返回 true 表示在宽限内完成（无论成功失败），false 表示超出宽限。
/// C++ 差异：cancel/close 是同步阻塞调用，必须放进 async 才能施加宽限；
/// 超时时该线程被 detach，故 promise 用 shared_ptr 保活。
bool await_cleanup_with_grace(const std::string& session_key,
                              const char* label,
                              const std::function<void()>& cleanup_body) {
    auto promise = std::make_shared<std::promise<std::optional<std::string>>>();
    auto future = promise->get_future();

    std::thread([promise, cleanup_body, session_key, label]() {
        try {
            cleanup_body();
            promise->set_value(std::nullopt);
        } catch (const std::exception& e) {
            promise->set_value(std::string(e.what()));
        } catch (...) {
            promise->set_value(std::string("unknown error"));
        }
    }).detach();

    const auto status =
        future.wait_for(std::chrono::milliseconds(ACP_TURN_TIMEOUT_CLEANUP_GRACE_MS));
    if (status != std::future_status::ready) {
        // 对齐 OC：超出宽限只记日志，迟到的失败由后台线程写入（此处不再等待）
        log_turn_timeout_verbose(std::string("timed-out turn ") + label +
                                 " cleanup exceeded " +
                                 std::to_string(ACP_TURN_TIMEOUT_CLEANUP_GRACE_MS) + "ms for " +
                                 session_key);
        return false;
    }
    const auto error = future.get();
    if (error) {
        log_turn_timeout_verbose(std::string("timed-out turn ") + label +
                                 " cleanup failed for " + session_key + ": " + *error);
    }
    return true;
}

} // namespace

bool is_cancellation_stop_reason(const std::optional<std::string>& stop_reason) {
    if (!stop_reason) return false;
    // 对齐 OC: "cancel" | "cancelled" | "manual-cancel"
    return *stop_reason == "cancel" || *stop_reason == "cancelled" ||
           *stop_reason == "manual-cancel";
}

std::string resolve_acp_turn_terminal_status(const AcpRuntimeEvent& done_event) {
    // 对齐 OC: event.status ?? (isCancellationStopReason(event.stopReason) ? "cancelled" : "completed")
    // 注意 HunterClaw 的 AcpRuntimeEvent::status 字段在 ToolCall 场景复用为工具状态，
    // 这里只在 Done 事件上读取，且只认这两个合法值，避免把工具状态误当终态。
    if (done_event.status) {
        if (*done_event.status == "completed" || *done_event.status == "cancelled") {
            return *done_event.status;
        }
    }
    return is_cancellation_stop_reason(done_event.stop_reason) ? "cancelled" : "completed";
}

int64_t resolve_turn_timeout_ms(const AcpSessionRuntimeOptions& options) {
    // 对齐 OC: resolveRuntimeOptionsFromMeta(meta).timeoutSeconds
    if (options.timeout_seconds && *options.timeout_seconds > 0) {
        const int64_t ms = static_cast<int64_t>(*options.timeout_seconds) * 1000;
        return clamp_timer_timeout_ms(ms, ACP_TURN_TIMEOUT_MIN_MS);
    }
    // 对齐 OC: resolveAgentTimeoutMs({ cfg, minMs: 1_000 })
    // 复用 agents 侧同一默认值，但不引入 agents 依赖（避免 acp→agents 反向依赖）
    const int64_t base_ms = ACP_DEFAULT_AGENT_TIMEOUT_SECONDS * 1000;
    return clamp_timer_timeout_ms(std::min(base_ms, ACP_MAX_SAFE_TIMEOUT_MS),
                                  ACP_TURN_TIMEOUT_MIN_MS);
}

AcpRuntimeError make_turn_timeout_error(int64_t timeout_label_ms) {
    // 对齐 OC: `ACP turn timed out after ${Math.max(1, Math.round(labelMs / 1000))}s.`
    const int64_t seconds = std::max<int64_t>(
        1, static_cast<int64_t>(std::llround(static_cast<double>(timeout_label_ms) / 1000.0)));
    return AcpRuntimeError(AcpErrorCode::ACP_TURN_FAILED,
                           "ACP turn timed out after " + std::to_string(seconds) + "s.",
                           std::string(ACP_TURN_TIMEOUT_DETAIL_CODE));
}

AwaitTurnResult await_turn_with_timeout(
    const std::string& session_key,
    int64_t timeout_ms,
    int64_t /*timeout_label_ms*/,
    const std::function<AcpTurnStreamOutcome()>& run_turn_body)
{
    struct TurnOutcomeBox {
        AcpTurnStreamOutcome outcome;
        std::optional<AcpRuntimeError> error;
    };

    auto promise = std::make_shared<std::promise<TurnOutcomeBox>>();
    auto future = promise->get_future();

    // turn 体跑在独立线程：超时后主线程返回，但该线程无法强杀，
    // 会继续跑到 runtime 自己结束。promise 用 shared_ptr 保活。
    std::thread([promise, run_turn_body]() {
        TurnOutcomeBox box;
        try {
            box.outcome = run_turn_body();
        } catch (const AcpRuntimeError& e) {
            box.error = e;
        } catch (const std::exception& e) {
            box.error = AcpRuntimeError(AcpErrorCode::ACP_TURN_FAILED, e.what());
        } catch (...) {
            box.error = AcpRuntimeError(AcpErrorCode::ACP_TURN_FAILED,
                                        "ACP turn failed before completion.");
        }
        promise->set_value(std::move(box));
    }).detach();

    // 对齐 OC: timeoutMs <= 0 → 无限等待
    if (timeout_ms <= 0) {
        auto box = future.get();
        AwaitTurnResult result;
        if (box.error) {
            result.kind = AwaitTurnResult::Kind::Failed;
            result.error = box.error;
        } else {
            result.kind = AwaitTurnResult::Kind::Completed;
            result.outcome = box.outcome;
        }
        return result;
    }

    const int64_t effective_ms = clamp_timer_timeout_ms(timeout_ms, 1);
    const auto status = future.wait_for(std::chrono::milliseconds(effective_ms));

    if (status != std::future_status::ready) {
        // 对齐 OC：迟到的错误只记日志，不改变已判定的超时结果。
        // 用 shared_future 让后台观察线程与主线程共享同一结果。
        auto shared = std::make_shared<std::shared_future<TurnOutcomeBox>>(future.share());
        std::thread([shared, session_key]() {
            try {
                const auto& late = shared->get();
                if (late.error) {
                    log_turn_timeout_verbose("detached late turn error after timeout for " +
                                             session_key + ": " + late.error->what());
                }
            } catch (...) {
                // 观察线程自身异常无需上报
            }
        }).detach();

        AwaitTurnResult result;
        result.kind = AwaitTurnResult::Kind::TimedOut;
        return result;
    }

    auto box = future.get();
    AwaitTurnResult result;
    if (box.error) {
        result.kind = AwaitTurnResult::Kind::Failed;
        result.error = box.error;
    } else {
        result.kind = AwaitTurnResult::Kind::Completed;
        result.outcome = box.outcome;
    }
    return result;
}

void cleanup_timed_out_turn(
    const std::string& session_key,
    const std::shared_ptr<AcpRuntime>& runtime,
    const AcpRuntimeHandle& handle,
    AcpRuntimeSessionMode mode,
    const std::function<void()>& clear_cached_runtime_state)
{
    if (!runtime) {
        return;
    }

    // 对齐 OC: activeTurn.abortController.abort() —— C++ 侧由调用方置 cancelled 标志，
    // 此处直接进入 runtime.cancel。
    const bool cancel_finished = await_cleanup_with_grace(
        session_key, "cancel",
        [runtime, handle]() { runtime->cancel(handle, ACP_TURN_TIMEOUT_REASON); });

    // 对齐 OC: persistent 会话超时后保留 handle，仅 oneshot 才 close
    if (mode != AcpRuntimeSessionMode::Oneshot) {
        return;
    }

    const bool close_finished = await_cleanup_with_grace(
        session_key, "close",
        [runtime, handle]() { runtime->close(handle, ACP_TURN_TIMEOUT_REASON); });

    if (cancel_finished && close_finished) {
        if (clear_cached_runtime_state) clear_cached_runtime_state();
        return;
    }

    // 对齐 OC: void Promise.allSettled([...]).then(() => clearCached...)
    // 有一方超出宽限 —— 不能立刻清缓存（handle 可能还在被使用），
    // 交给后台线程在宽限外再补一次清理。
    if (clear_cached_runtime_state) {
        std::thread([clear_cached_runtime_state]() {
            std::this_thread::sleep_for(
                std::chrono::milliseconds(ACP_TURN_TIMEOUT_CLEANUP_GRACE_MS));
            try {
                clear_cached_runtime_state();
            } catch (...) {
                // 清缓存失败不影响主流程
            }
        }).detach();
    }
}

} // namespace acp
} // namespace hunterclaw
