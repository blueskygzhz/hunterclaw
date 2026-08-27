#pragma once

// ============================================================================
// remote_a2a/callback_endpoint.h
// 远端 A2A 的回调接收端（pushNotification）。
//
// ── ★ 回调只是「信号」，不是数据源 ─────────────────────────────────────────
// 收到回调后【必须】反过来做一次 tasks/get 才算拿到结果。
//
// 理由不是洁癖，而是攻击面：回调 payload 由远端构造、经公网传输。
// 若直接采信，攻击者猜到 handle + token 就能注入「任务已完成，结果是 X」，
// 而那个 X 会被当作可信内容写进 transcript 并进入下一轮 prompt。
// tasks/get 走的是我们主动发起、已鉴权的通道，是唯一可信来源。
//
// 因此本端点的处理逻辑极简：
//   ① 校验 token（恒定时间）
//   ② 落一个「有更新」的标记
//   ③ 交给 engine->poll_once() 做权威拉取
//   ④ 回 2xx
// payload 里的 task state / artifacts 【全部丢弃】，只用它做日志。
//
// ── ★ 鉴权旁路：必须绕过 AuthMiddleware ────────────────────────────────────
// 回调带的是【我们发给该任务的一次性 token】，不是 gateway 共享 token
// （http_gateway.cc 的 AuthMiddleware 只放行 /health 与 /metrics）。
// 若不旁路，远端的回调会全部 401 —— 而远端会重试到超时，
// 表现为「回调配了但从来不生效」，且日志里只有一堆 401。
//
// 旁路的代价用三道闸门补偿：
//   闸门 1  handle 必须存在且【已绑定 token】（未绑定一律拒，
//           见 pending_tasks.cc:verify_callback_token —— 空 token 返回 false）
//   闸门 2  token 恒定时间比较（防时序侧信道）
//   闸门 3  失败计数 + 节流（防 handle/token 暴力枚举）
//
// ── 一期内存存储的后果 ─────────────────────────────────────────────────────
// 多进程部署时回调可能打到没有该任务的进程 → 返回 404，远端重试。
// ★ 404 是刻意的（而非 200）：回 200 会让远端认为投递成功并不再重试，
//   那么任务结果就永久丢失了。404 让远端继续重试，有机会命中正确进程。
//
// ── 并发原语约束（项目级）────────────────────────────────────────────────
// ✅ thread / mutex / condition_variable / atomic
// ❌ future / promise / async / packaged_task
// ============================================================================

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>

#include <nlohmann/json.hpp>

#include "hunterclaw/core/types.h"
#include "hunterclaw/remote_a2a/execution_engine.h"
#include "hunterclaw/remote_a2a/pending_tasks.h"

namespace hunterclaw {
namespace remote_a2a {

/// 回调路径前缀。★ 与 build_push_config（execution_engine.cc）保持一致。
constexpr const char* CALLBACK_PATH_PREFIX = "/remote-a2a/callback/";

// ── 结果 ────────────────────────────────────────────────────────────────────

struct CallbackOutcome {
    int http_status = 200;
    /// 响应体（JSON）
    nlohmann::json body;

    /// 是否触发了权威拉取
    bool polled = false;
    /// 拉取后是否已到终态
    bool terminal = false;

    std::string handle;
    /// 内部诊断（★ 绝不含 token）
    std::string note;
};

// ── 配置 ────────────────────────────────────────────────────────────────────

struct CallbackEndpointConfig {
    /// 单个来源在窗口内允许的失败次数。超限后一律 429。
    int max_failures_per_window = 10;
    int64_t failure_window_ms = 60000;

    /// 请求体上限。★ 远端可控 —— 不设上限就是免费的内存 DoS。
    ///   回调 payload 只用于日志，用不着大体积。
    size_t max_body_bytes = 64 * 1024;

    /// 是否在响应体里回显 handle。
    /// ⚠ 默认 false：回显等于给枚举者一个「这个 handle 存在」的确认信号。
    bool echo_handle = false;
};

// ── 端点 ────────────────────────────────────────────────────────────────────

/// handle → engine 反查（由 registry 提供）
using CallbackEngineLookupFn =
    std::function<std::shared_ptr<ExecutionEngine>(const std::string& handle)>;

class CallbackEndpoint {
public:
    static std::shared_ptr<CallbackEndpoint> create(
        std::shared_ptr<PendingTaskStore> store, CallbackEngineLookupFn lookup,
        const CallbackEndpointConfig& cfg = {});

    CallbackEndpoint(std::shared_ptr<PendingTaskStore> store,
                     CallbackEngineLookupFn lookup, const CallbackEndpointConfig& cfg);

    /// 处理一次回调。
    ///
    /// @param path         完整请求路径（用于提取 handle）
    /// @param auth_header  Authorization 头原文（可空）
    /// @param body         请求体原文
    /// @param peer         对端标识（IP 等，仅用于失败节流分桶）
    ///
    /// ★ 任何失败都【不】泄漏「handle 存在但 token 错」与「handle 不存在」
    ///   的区别 —— 两者都回 404。区分开就是一个 handle 枚举探针。
    CallbackOutcome handle_callback(const std::string& path,
                                   const std::string& auth_header,
                                   const std::string& body, const std::string& peer);

    /// 当前失败计数（监控用）
    int failure_count(const std::string& peer) const;

    /// 累计统计
    uint64_t total_accepted() const { return accepted_.load(); }
    uint64_t total_rejected() const { return rejected_.load(); }

private:
    /// 记录一次失败并判断是否应节流。
    /// @return true 表示已超限（调用方应回 429）
    bool note_failure_and_check(const std::string& peer);

    std::shared_ptr<PendingTaskStore> store_;
    CallbackEngineLookupFn lookup_;
    CallbackEndpointConfig cfg_;

    struct FailureBucket {
        int count = 0;
        int64_t window_start_ms = 0;
    };
    mutable std::mutex mu_;
    std::map<std::string, FailureBucket> failures_;

    std::atomic<uint64_t> accepted_{0};
    std::atomic<uint64_t> rejected_{0};
};

// ── 辅助 ────────────────────────────────────────────────────────────────────

/// 从回调路径提取 handle。
///
/// 校验：
///   · 必须以 CALLBACK_PATH_PREFIX 开头
///   · handle 段不得含 '/'（★ 防路径穿越：".../callback/../../admin"）
///   · handle 只允许 [A-Za-z0-9_-]（我们生成的格式）
///   · 长度 [4, 128]
/// @return 空 optional 表示非法
std::optional<std::string> extract_callback_handle(const std::string& path);

/// 从 Authorization 头提取 Bearer token。
/// ★ scheme 比较大小写不敏感（RFC 7235）；缺 scheme 时【拒绝】而非当裸 token
///   —— 接受裸 token 会让 "Authorization: <handle>" 这类误配意外通过。
std::optional<std::string> extract_bearer_token(const std::string& auth_header);

}  // namespace remote_a2a
}  // namespace hunterclaw
