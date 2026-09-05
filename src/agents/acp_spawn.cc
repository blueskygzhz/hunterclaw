// acp_spawn.cc
// Aligned with openclaw:
//   src/agents/acp-spawn.ts          (spawnAcpDirect)
//   src/agents/acp-spawn-parent-stream.ts (startAcpSpawnParentStreamRelay)

#include "hunterclaw/agents/acp_spawn.h"
#include "hunterclaw/agents/subagent.h"
#include "hunterclaw/acp/acp_session_manager.h"
#include "hunterclaw/gateway/client.h"
#include "hunterclaw/infra/agent_events.h"
#include "hunterclaw/infra/system_events.h"
#include "hunterclaw/infra/heartbeat_wake.h"
#include "hunterclaw/core/types.h"
#include "hunterclaw/tasks/task_executor.h"
#include <spdlog/spdlog.h>
#include <nlohmann/json.hpp>
#include <thread>
#include <mutex>
#include <atomic>
#include <chrono>
#include <random>
#include <sstream>
#include <algorithm>
#include <fstream>
#include <filesystem>
#include <map>
#include <set>
#include <vector>

using json = nlohmann::json;

namespace hunterclaw {
namespace agents {

namespace {

// ── ACP tag visibility ───────────────────────────────────────────────────────
// Aligned with openclaw: ACP_TAG_VISIBILITY_DEFAULTS (acp-stream-settings.ts:15-26)
// 只有 agent_message_chunk 默认可见；其余 session-update tag 默认不投递到父会话。
const std::map<std::string, bool>& acp_tag_visibility_defaults() {
    static const std::map<std::string, bool> kDefaults = {
        {"agent_message_chunk",        true},
        {"tool_call",                  false},
        {"tool_call_update",           false},
        {"usage_update",               false},
        {"available_commands_update",  false},
        {"current_mode_update",        false},
        {"config_option_update",       false},
        {"session_info_update",        false},
        {"plan",                       false},
        {"agent_thought_chunk",        false},
    };
    return kDefaults;
}

// Aligned with openclaw: isAcpSessionUpdateTag (acp-stream-settings.ts:28-30)
bool is_acp_session_update_tag(const std::string& tag) {
    return acp_tag_visibility_defaults().count(tag) > 0;
}

// ── RelayProjectionSettings ──────────────────────────────────────────────────
// relay 只需要 tagVisibility 覆盖表；其余 projection 字段（deliveryMode 等）
// 由 AcpReplyProjector 使用，与 relay 无关。
struct RelayProjectionSettings {
    std::map<std::string, bool> tag_visibility;  // 显式覆盖（缺省走 defaults 表）
};

// Aligned with openclaw: resolveAcpProjectionSettings — 取 acp.projection.tagVisibility
RelayProjectionSettings resolve_relay_projection_settings(const std::optional<json>& cfg) {
    RelayProjectionSettings settings;
    if (!cfg.has_value() || !cfg->is_object()) return settings;
    const json acp = cfg->value("acp", json::object());
    if (!acp.is_object()) return settings;
    const json projection = acp.value("projection", json::object());
    if (!projection.is_object()) return settings;
    const json tag_vis = projection.value("tagVisibility", json::object());
    if (!tag_vis.is_object()) return settings;
    for (auto it = tag_vis.begin(); it != tag_vis.end(); ++it) {
        if (it.value().is_boolean()) settings.tag_visibility[it.key()] = it.value().get<bool>();
    }
    return settings;
}

// Aligned with openclaw: isAcpTagVisible (acp-stream-settings.ts:118-134)
//   1. 无 tag              → 可见
//   2. 非 session-update tag → 可见
//   3. 有显式覆盖          → 用覆盖值
//   4. 否则                → 查默认表
bool is_acp_tag_visible(const RelayProjectionSettings& settings, const std::string& tag) {
    if (tag.empty()) return true;
    if (!is_acp_session_update_tag(tag)) return true;
    auto override_it = settings.tag_visibility.find(tag);
    if (override_it != settings.tag_visibility.end()) return override_it->second;
    const auto& defaults = acp_tag_visibility_defaults();
    auto default_it = defaults.find(tag);
    // 真源在缺省时 throw；这里返回 false（保守：不投递未知 tag）以免拖垮 relay 线程。
    return default_it != defaults.end() ? default_it->second : false;
}

// Aligned with openclaw: shouldRelayAcpStatusProgress (acp-spawn-parent-stream.ts:184-194)
bool should_relay_acp_status_progress(const std::string& event_type,
                                     const std::string& tag,
                                     const std::string& text,
                                     const RelayProjectionSettings& settings) {
    if (event_type != "status" || text.empty()) return false;
    return is_acp_tag_visible(settings, tag);
}

// Aligned with openclaw: resolveParentProgressCommentary (acp-spawn-parent-stream.ts:174-182)
//   最终归结到 resolveChannelStreamingProgressCommentary(entry, /*fallback=*/true)
//   → 缺配置时默认 true。这里读 streaming.progressCommentary 覆盖。
bool resolve_parent_progress_commentary(const std::optional<json>& cfg) {
    if (!cfg.has_value() || !cfg->is_object()) return true;
    const json streaming = cfg->value("streaming", json::object());
    if (streaming.is_object()) {
        const auto it = streaming.find("progressCommentary");
        if (it != streaming.end() && it->is_boolean()) return it->get<bool>();
    }
    return true;
}

// Aligned with openclaw: formatProxyEnvSummary
std::string format_proxy_env_summary(const std::vector<std::string>& keys) {
    if (keys.empty()) return "no proxy env";
    std::string out = "proxy env: ";
    for (size_t i = 0; i < keys.size(); ++i) {
        if (i > 0) out += ", ";
        out += keys[i];
    }
    return out;
}

// UTF-8 安全的尾部截取（保留最后 max_bytes 字节，不切断多字节字符）
// Aligned with openclaw: sliceUtf16Safe(text, -STREAM_BUFFER_MAX_CHARS)
std::string slice_tail_utf8_safe(const std::string& text, size_t max_bytes) {
    if (text.size() <= max_bytes) return text;
    size_t start = text.size() - max_bytes;
    while (start < text.size() && (static_cast<unsigned char>(text[start]) & 0xC0) == 0x80) ++start;
    return text.substr(start);
}

auto log() {
    static auto logger = spdlog::get("acp_spawn");
    if (!logger) logger = spdlog::stdout_logger_mt("acp_spawn");
    return logger;
}

static std::string generate_uuid() {
    static std::random_device rd;
    static std::mt19937_64 gen(rd());
    static std::uniform_int_distribution<uint64_t> dis;
    uint64_t hi = dis(gen);
    uint64_t lo = dis(gen);
    hi = (hi & 0xFFFFFFFFFFFF0FFFULL) | 0x0000000000004000ULL;
    lo = (lo & 0x3FFFFFFFFFFFFFFFULL) | 0x8000000000000000ULL;
    char buf[37];
    snprintf(buf, sizeof(buf),
        "%08x-%04x-%04x-%04x-%012llx",
        (uint32_t)(hi >> 32),
        (uint16_t)(hi >> 16),
        (uint16_t)(hi),
        (uint16_t)(lo >> 48),
        (unsigned long long)(lo & 0x0000FFFFFFFFFFFFULL));
    return std::string(buf);
}

static std::string compact_whitespace(const std::string& s) {
    std::string result;
    result.reserve(s.size());
    bool last_space = false;
    for (char c : s) {
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
            if (!last_space && !result.empty()) {
                result += ' ';
                last_space = true;
            }
        } else {
            result += c;
            last_space = false;
        }
    }
    // trim trailing space
    while (!result.empty() && result.back() == ' ') result.pop_back();
    return result;
}

static std::string truncate_str(const std::string& s, size_t max_chars) {
    if (s.size() <= max_chars) return s;
    if (max_chars <= 1) return s.substr(0, max_chars);
    // UTF-8 safe truncation (approximate)
    std::string result = s.substr(0, max_chars - 1);
    result += "\xe2\x80\xa6"; // UTF-8 ellipsis '…'
    return result;
}

} // namespace

// ============================================================================
// resolve_acp_spawn_runtime_policy_error
// Aligned with openclaw: resolveAcpSpawnRuntimePolicyError
// ============================================================================

std::optional<std::string> resolve_acp_spawn_runtime_policy_error(
    const AcpSpawnConfig& cfg,
    const std::optional<std::string>& requester_session_key,
    bool requester_sandboxed,
    const std::optional<std::string>& sandbox_mode
) {
    // Aligned with openclaw: sandboxed sessions cannot spawn ACP
    if (requester_sandboxed) {
        return "Sandboxed sessions cannot spawn ACP sessions because runtime=\"acp\" runs on the host. "
               "Use runtime=\"subagent\" from sandboxed sessions.";
    }
    // Aligned with openclaw: sandbox="require" is unsupported for ACP
    if (sandbox_mode.has_value() && *sandbox_mode == "require") {
        return "sessions_spawn sandbox=\"require\" is unsupported for runtime=\"acp\" because ACP sessions "
               "run outside the sandbox. Use runtime=\"subagent\" or sandbox=\"inherit\".";
    }
    return std::nullopt;
}

// ============================================================================
// start_acp_spawn_parent_stream_relay
// Aligned with openclaw: startAcpSpawnParentStreamRelay
// ============================================================================

AcpSpawnParentRelayHandle start_acp_spawn_parent_stream_relay(
    const std::string& run_id,
    const std::string& parent_session_key,
    const std::string& child_session_key,
    const std::string& agent_id,
    const std::optional<std::string>& log_path,
    int stream_flush_ms,
    int no_output_notice_ms,
    int no_output_poll_ms,
    int max_relay_lifetime_ms,
    bool emit_start_notice,
    const std::optional<json>& delivery_context,
    bool surface_updates,
    const std::optional<json>& cfg
) {
    if (run_id.empty() || parent_session_key.empty()) {
        return { [](){}, [](){} };
    }

    // Aligned with openclaw: STREAM_SNIPPET_MAX_CHARS = 220
    static constexpr size_t STREAM_SNIPPET_MAX_CHARS = 220;
    // Aligned with openclaw: STREAM_BUFFER_MAX_CHARS = 4000
    static constexpr size_t STREAM_BUFFER_MAX_CHARS = 4000;

    const std::string relay_label = truncate_str(compact_whitespace(agent_id), 40).empty()
        ? "ACP child" : truncate_str(compact_whitespace(agent_id), 40);
    const std::string context_prefix = "acp-spawn:" + run_id;

    // Aligned with openclaw: acp-spawn-parent-stream.ts:358-362
    //   shouldRelayProgressCommentary = resolveParentProgressCommentary({ cfg, deliveryContext })
    //   acpProjectionSettings         = resolveAcpProjectionSettings(cfg ?? {})
    const bool should_relay_progress_commentary = resolve_parent_progress_commentary(cfg);
    const RelayProjectionSettings projection_settings = resolve_relay_projection_settings(cfg);

    // Shared relay state
    struct RelayState {
        std::atomic<bool> disposed{false};
        std::string pending_text;
        std::mutex text_mutex;
        std::atomic<int64_t> last_progress_at{0};
        std::atomic<bool> stall_notified{false};

        // Aligned with openclaw: pendingProgressKind
        // 当前 pending_text 归属的进度来源（"assistant:xxx" / "item:<id>" / "acp:status"）。
        // kind 切换时必须先 flush，避免不同来源的文本被拼成一条。
        // 与 pending_text 共用 text_mutex。
        std::string pending_progress_kind;

        // Aligned with openclaw: replaceableAssistantSnapshot
        // 可替换的 assistant 快照（后续快照覆盖前一份），落地时才走 append_visible_progress。
        std::optional<std::string> replaceable_assistant_snapshot;
        std::mutex replaceable_mutex;

        // Aligned with openclaw: itemProgressTextById
        // item 进度快照按 itemId 累积，用于计算增量（前缀更新只发新增部分）。
        std::map<std::string, std::string> item_progress_text_by_id;
        std::mutex item_mutex;

        // ── stall 分级诊断所需状态 ──────────────────────────────────────────
        // Aligned with openclaw: promptSubmittedAt / firstRuntimeEventAt /
        //   firstVisibleOutputAt / lastRuntimeEventType / proxyEnvKeysAtPrompt
        // buildNoOutputNotice 靠这四项区分「没提交 prompt」「提交了但没 runtime 事件」
        // 「有 runtime 活动但无可见输出」「有过输出后卡住」四种停滞原因。
        std::atomic<int64_t> prompt_submitted_at{0};
        std::atomic<int64_t> first_runtime_event_at{0};
        std::atomic<int64_t> first_visible_output_at{0};
        std::string last_runtime_event_type;
        std::vector<std::string> proxy_env_keys_at_prompt;
        std::mutex diag_mutex;

        // Aligned with openclaw: scheduleFlush timer
        // A background thread wakes every streamFlushMs to flush pending text
        // even if buffer hasn't hit the immediate-flush threshold.
        std::atomic<bool> flush_scheduled{false};

        // Log file
        std::optional<std::string> log_path_opt;
        std::mutex log_mutex;
        std::string pending_log_lines;
    };
    auto state = std::make_shared<RelayState>();
    state->last_progress_at.store(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()
        ).count()
    );
    if (log_path.has_value() && !log_path->empty()) {
        state->log_path_opt = *log_path;
    }

    // Helper: write log line (best-effort)
    auto write_log_line = [state](const json& entry) {
        if (!state->log_path_opt.has_value()) return;
        try {
            std::lock_guard<std::mutex> lock(state->log_mutex);
            state->pending_log_lines += entry.dump() + "\n";
            if (state->pending_log_lines.size() >= 16384) {
                // flush
                std::filesystem::create_directories(
                    std::filesystem::path(*state->log_path_opt).parent_path()
                );
                std::ofstream f(*state->log_path_opt, std::ios::app);
                f << state->pending_log_lines;
                state->pending_log_lines.clear();
            }
        } catch (...) {}
    };

    // Helper: flush log
    auto flush_log = [state]() {
        if (!state->log_path_opt.has_value()) return;
        try {
            std::lock_guard<std::mutex> lock(state->log_mutex);
            if (state->pending_log_lines.empty()) return;
            std::filesystem::create_directories(
                std::filesystem::path(*state->log_path_opt).parent_path()
            );
            std::ofstream f(*state->log_path_opt, std::ios::app);
            f << state->pending_log_lines;
            state->pending_log_lines.clear();
        } catch (...) {}
    };

    // Helper: wake parent heartbeat
    auto wake = [parent_session_key, surface_updates]() {
        if (!surface_updates) return;
        // Aligned with openclaw: requestHeartbeat(scopedHeartbeatWakeOptionsForPolicy(
        //   parentSessionKey, { source: "acp-spawn", intent: "event", reason: "acp:spawn:stream" }))
        infra::request_heartbeat_now("acp:spawn:stream", std::nullopt, parent_session_key,
                                     infra::HeartbeatWakeIntent::Event,
                                     infra::HeartbeatWakeSource::AcpSpawn);
    };

    // Helper: emit system event to parent
    auto emit = [state, parent_session_key, context_prefix, wake, write_log_line,
                 delivery_context, surface_updates](
        const std::string& text, const std::string& context_key
    ) {
        std::string cleaned = text;
        // trim
        size_t start = cleaned.find_first_not_of(" \t\r\n");
        if (start == std::string::npos) return;
        cleaned = cleaned.substr(start);
        size_t end = cleaned.find_last_not_of(" \t\r\n");
        if (end != std::string::npos) cleaned = cleaned.substr(0, end + 1);
        if (cleaned.empty()) return;

        write_log_line({
            {"ts", std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count()},
            {"kind", "system_event"},
            {"contextKey", context_key},
            {"text", cleaned}
        });

        if (!surface_updates) return;

        // Aligned with openclaw: enqueueSystemEvent(cleaned, { sessionKey, contextKey, trusted: false })
        // Note: delivery_context routing is handled by the gateway layer in HunterClaw
        infra::enqueue_system_event(cleaned, parent_session_key, context_key);
        wake();
    };

    // Helper: flush pending text
    // Aligned with openclaw: flushPending — 同时清空 pendingProgressKind
    auto flush_pending = [state, relay_label, context_prefix, emit]() {
        std::string text_to_flush;
        {
            std::lock_guard<std::mutex> lock(state->text_mutex);
            text_to_flush = std::move(state->pending_text);
            state->pending_text.clear();
            state->pending_progress_kind.clear();
        }
        if (text_to_flush.empty()) return;
        std::string snippet = truncate_str(compact_whitespace(text_to_flush), STREAM_SNIPPET_MAX_CHARS);
        if (snippet.empty()) return;
        emit(relay_label + ": " + snippet, context_prefix + ":progress");
    };

    // ── append_visible_progress ──────────────────────────────────────────────
    // Aligned with openclaw: appendVisibleProgress (acp-spawn-parent-stream.ts:466-494)
    //
    // 三个 stream 分支（assistant / item / acp）共用的唯一进度累积入口。职责：
    //   1. stall 恢复通知（stallNotified 复位 + recordTaskRunProgress + emit）
    //   2. 刷新 lastProgressAt，首次记录 firstVisibleOutputAt
    //   3. kind 切换时先 flush 旧内容，避免不同来源文本被拼接
    //   4. 累积并按 STREAM_BUFFER_MAX_CHARS 做 UTF-8 安全尾部截断
    //   5. 达到 snippet 阈值或遇到段落分隔（\n\n）立即 flush，否则交由定时线程
    auto append_visible_progress = [state, relay_label, context_prefix, run_id,
                                    emit, flush_pending, child_session_key](
        const std::string& delta, const std::string& kind
    ) {
        // 1. stall 恢复
        if (state->stall_notified.exchange(false)) {
            try {
                tasks::get_task_executor().record_task_progress_by_run_id(
                    run_id, "Resumed output.");
            } catch (...) {}
            emit(relay_label + " resumed output.", context_prefix + ":resumed");
        }

        const int64_t now_ts = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();

        // 2. 进度时间戳
        state->last_progress_at.store(now_ts);
        int64_t expected_visible = 0;
        state->first_visible_output_at.compare_exchange_strong(expected_visible, now_ts);

        // 3~5. kind 隔离 + 累积 + flush 判定
        bool need_flush_previous = false;
        bool should_flush_now = false;
        {
            std::lock_guard<std::mutex> lock(state->text_mutex);
            if (!state->pending_text.empty() &&
                !state->pending_progress_kind.empty() &&
                state->pending_progress_kind != kind) {
                need_flush_previous = true;
            }
        }
        if (need_flush_previous) {
            flush_pending();
        }
        {
            std::lock_guard<std::mutex> lock(state->text_mutex);
            state->pending_progress_kind = kind;
            state->pending_text += delta;
            if (state->pending_text.size() > STREAM_BUFFER_MAX_CHARS) {
                state->pending_text =
                    slice_tail_utf8_safe(state->pending_text, STREAM_BUFFER_MAX_CHARS);
            }
            should_flush_now =
                state->pending_text.size() >= STREAM_SNIPPET_MAX_CHARS ||
                delta.find("\n\n") != std::string::npos;
        }
        if (should_flush_now) flush_pending();
        // 未达阈值时由 flush 定时线程兜底（对齐 scheduleFlush）
    };

    // ── flush_replaceable_assistant_snapshot ─────────────────────────────────
    // Aligned with openclaw: flushReplaceableAssistantSnapshot (:496-503)
    auto flush_replaceable_assistant_snapshot = [state, append_visible_progress]() {
        std::optional<std::string> snapshot;
        {
            std::lock_guard<std::mutex> lock(state->replaceable_mutex);
            snapshot = std::move(state->replaceable_assistant_snapshot);
            state->replaceable_assistant_snapshot.reset();
        }
        if (!snapshot.has_value()) return;
        if (snapshot->find_first_not_of(" \t\r\n") == std::string::npos) return;
        append_visible_progress(*snapshot, "assistant:replaceable");
    };

    // ── append_item_progress_snapshot ────────────────────────────────────────
    // Aligned with openclaw: appendItemProgressSnapshot (:505-519)
    //
    // item 进度是「快照」而非增量：同一 itemId 的后续文本通常是前一份的前缀扩展。
    //   - 文本未变         → 丢弃
    //   - 是前缀扩展       → 只投递新增部分
    //   - 非前缀（被重写） → 丢弃同 kind 的未 flush 内容，整份重投
    auto append_item_progress_snapshot = [state, append_visible_progress](
        const std::string& item_id, const std::string& text
    ) {
        std::string previous;
        {
            std::lock_guard<std::mutex> lock(state->item_mutex);
            auto it = state->item_progress_text_by_id.find(item_id);
            if (it != state->item_progress_text_by_id.end()) previous = it->second;
        }
        if (text == previous) return;

        const std::string kind = "item:" + item_id;
        const bool is_prefix_update =
            !previous.empty() && text.rfind(previous, 0) == 0;

        if (!previous.empty() && !is_prefix_update) {
            // Aligned with openclaw: 快照被重写时丢弃同 kind 的 pending，避免重复文本
            std::lock_guard<std::mutex> lock(state->text_mutex);
            if (state->pending_progress_kind == kind && !state->pending_text.empty()) {
                state->pending_text.clear();
            }
        }
        {
            std::lock_guard<std::mutex> lock(state->item_mutex);
            state->item_progress_text_by_id[item_id] = text;
        }
        const std::string delta =
            is_prefix_update ? text.substr(previous.size()) : text;
        if (delta.empty()) return;
        append_visible_progress(delta, kind);
    };

    // Emit start notice if requested
    // Aligned with openclaw: emitStartNotice() calls recordTaskRunProgressByRunId + emit
    auto emit_start_notice_fn = [state, relay_label, context_prefix, child_session_key,
                                  run_id, emit]() {
        if (state->disposed.load()) return;
        // Aligned with openclaw: recordTaskRunProgressByRunId({ runId, runtime:"acp", sessionKey, lastEventAt, eventSummary:"Started." })
        try {
            tasks::get_task_executor().record_task_progress_by_run_id(run_id, "Started.");
        } catch (...) {}
        emit(
            "Started " + relay_label + " session " + child_session_key +
            ". Streaming progress updates to parent session.",
            context_prefix + ":start"
        );
    };

    if (emit_start_notice) {
        emit_start_notice_fn();
    }

    // Subscribe to agent events
    // Aligned with openclaw: onAgentEvent(event => { if (event.runId !== runId) return; ... })
    auto unsubscribe = infra::on_agent_event([
        state, run_id, relay_label, context_prefix,
        emit, flush_pending, write_log_line,
        no_output_notice_ms, child_session_key,
        append_visible_progress, append_item_progress_snapshot,
        flush_replaceable_assistant_snapshot,
        should_relay_progress_commentary, projection_settings
    ](const infra::AgentEvent& event) {
        if (state->disposed.load()) return;
        if (event.run_id != run_id) return;

        const int64_t now_ts = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();

        // ── stream === "assistant" ───────────────────────────────────────────
        // Aligned with openclaw: acp-spawn-parent-stream.ts:593-635
        if (event.stream == "assistant") {
            const std::string assistant_phase = event.assistant_phase();

            // Aligned with openclaw: snapshot = text ?? delta；replaceable 优先
            const std::string text_candidate  = event.data.value("text", std::string{});
            const std::string delta_candidate = event.data.value("delta", std::string{});
            const std::string snapshot =
                !text_candidate.empty() ? text_candidate : delta_candidate;

            // Aligned with openclaw: data.replaceable === true → 存快照，不立即投递
            const bool replaceable =
                event.data.contains("replaceable") &&
                event.data["replaceable"].is_boolean() &&
                event.data["replaceable"].get<bool>();
            if (replaceable) {
                if (!snapshot.empty() &&
                    snapshot.find_first_not_of(" \t\r\n") != std::string::npos) {
                    {
                        std::lock_guard<std::mutex> lock(state->replaceable_mutex);
                        state->replaceable_assistant_snapshot = snapshot;
                    }
                    state->last_progress_at.store(now_ts);
                    write_log_line({
                        {"ts", now_ts},
                        {"kind", "assistant_replaceable_snapshot"},
                        {"text", snapshot},
                        {"phase", assistant_phase.empty() ? json(nullptr) : json(assistant_phase)}
                    });
                }
                return;
            }

            // Aligned with openclaw: delta = deltaCandidate ?? snapshot
            const std::string delta =
                !delta_candidate.empty() ? delta_candidate : snapshot;
            if (delta.empty() ||
                delta.find_first_not_of(" \t\r\n") == std::string::npos) {
                return;
            }

            write_log_line({
                {"ts", now_ts},
                {"kind", "assistant_delta"},
                {"delta", delta},
                {"phase", assistant_phase.empty() ? json(nullptr) : json(assistant_phase)}
            });

            // Aligned with openclaw: commentary 且旁白关闭 → 只刷新进度，不投递
            if (assistant_phase == "commentary" && !should_relay_progress_commentary) {
                state->last_progress_at.store(now_ts);
                return;
            }

            // Aligned with openclaw: replaceableAssistantSnapshot = undefined
            // 真实 delta 到来后废弃暂存快照，避免重复投递。
            {
                std::lock_guard<std::mutex> lock(state->replaceable_mutex);
                state->replaceable_assistant_snapshot.reset();
            }
            append_visible_progress(
                delta,
                "assistant:" + (assistant_phase.empty() ? std::string("unknown") : assistant_phase)
            );
            return;
        }

        // ── stream === "item" ────────────────────────────────────────────────
        // Aligned with openclaw: acp-spawn-parent-stream.ts:637-655
        // 只有 kind == "preamble" 且带 progressText 的 item 参与父会话进度回传。
        if (event.stream == "item") {
            const std::string item_id       = event.data.value("itemId", std::string{});
            const std::string item_kind     = event.data.value("kind", std::string{});
            const std::string progress_text = event.data.value("progressText", std::string{});

            write_log_line({
                {"ts", now_ts},
                {"kind", "item"},
                {"itemKind", item_kind.empty() ? json(nullptr) : json(item_kind)},
                {"itemId", item_id.empty() ? json(nullptr) : json(item_id)},
                {"data", event.data}
            });

            if (item_kind == "preamble" && !progress_text.empty()) {
                state->last_progress_at.store(now_ts);
                if (should_relay_progress_commentary && !item_id.empty()) {
                    append_item_progress_snapshot(item_id, progress_text);
                }
            }
            return;
        }

        // Aligned with openclaw: acp-spawn-parent-stream.ts:657-698 (stream === "acp")
        //
        // ACP 路径的事件全部走 stream=="acp"（由 gateway agent.cc 的
        // emit_acp_runtime_event / emit_acp_prompt_submitted 桥接而来），
        // 不是 "assistant"。缺这个分支时 ACP 子会话进度会被整体忽略，
        // relay 只会在 no_output_notice_ms 后误报 stall。
        if (event.stream == "acp") {
            const std::string phase = event.phase();
            write_log_line({
                {"ts", now_ts},
                {"kind", "acp"},
                {"phase", phase.empty() ? "unknown" : phase},
                {"data", event.data}
            });

            if (phase == "prompt_submitted") {
                // Aligned with openclaw: promptSubmittedAt ??= at;
                //   proxyEnvKeysAtPrompt = data.proxyEnvKeys; lastProgressAt = Date.now()
                int64_t at = now_ts;
                if (event.data.contains("at") && event.data["at"].is_number()) {
                    at = event.data["at"].get<int64_t>();
                }
                int64_t expected = 0;
                state->prompt_submitted_at.compare_exchange_strong(expected, at);
                if (event.data.contains("proxyEnvKeys") && event.data["proxyEnvKeys"].is_array()) {
                    std::vector<std::string> keys;
                    for (const auto& k : event.data["proxyEnvKeys"]) {
                        if (k.is_string()) keys.push_back(k.get<std::string>());
                    }
                    std::lock_guard<std::mutex> lock(state->diag_mutex);
                    state->proxy_env_keys_at_prompt = std::move(keys);
                }
                state->last_progress_at.store(now_ts);
                return;
            }

            if (phase == "runtime_event") {
                const std::string event_type = event.data.value("eventType", std::string{});
                const std::string text       = event.data.value("text", std::string{});
                const std::string tag        = event.data.value("tag", std::string{});

                // Aligned with openclaw: firstRuntimeEventAt ??= now;
                //   lastRuntimeEventType = eventType
                int64_t expected_first = 0;
                state->first_runtime_event_at.compare_exchange_strong(expected_first, now_ts);
                {
                    std::lock_guard<std::mutex> lock(state->diag_mutex);
                    state->last_runtime_event_type = event_type;
                }

                // Aligned with openclaw: shouldRelayProgressCommentary &&
                //   shouldRelayAcpStatusProgress({ eventType, tag, text, projectionSettings })
                if (should_relay_progress_commentary &&
                    should_relay_acp_status_progress(event_type, tag, text, projection_settings)) {
                    // Aligned with openclaw: appendVisibleProgress(`${text}\n\n`, "acp:status")
                    append_visible_progress(text + "\n\n", "acp:status");
                    return;
                }

                // 其余 runtime_event（text_delta / tool_call / done / error / 隐藏 tag 的
                // status）只刷新进度时间戳——对齐真源 `lastProgressAt = Date.now(); return;`
                state->last_progress_at.store(now_ts);
                return;
            }
            return;
        }

        if (event.stream != "lifecycle") return;

        std::string phase = event.phase();
        write_log_line({
            {"ts", std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count()},
            {"kind", "lifecycle"},
            {"phase", phase.empty() ? "unknown" : phase},
            {"data", event.data}
        });

        if (phase == "end") {
            // Aligned with openclaw: flushReplaceableAssistantSnapshot() 先于 flushPending()
            // 未落地的可替换快照必须在收尾前投递，否则最后一段进度会丢。
            flush_replaceable_assistant_snapshot();
            flush_pending();
            // Aligned with openclaw: emit "run completed in Xs"
            int64_t started_at = 0, ended_at = 0;
            if (event.data.contains("startedAt") && event.data["startedAt"].is_number())
                started_at = event.data["startedAt"].get<int64_t>();
            if (event.data.contains("endedAt") && event.data["endedAt"].is_number())
                ended_at = event.data["endedAt"].get<int64_t>();
            int64_t duration_ms = (started_at > 0 && ended_at >= started_at)
                ? (ended_at - started_at) : 0;
            std::string done_text;
            if (duration_ms > 0) {
                int64_t secs = std::max((int64_t)1, duration_ms / 1000);
                done_text = relay_label + " run completed in " + std::to_string(secs) + "s.";
            } else {
                done_text = relay_label + " run completed.";
            }
            // Aligned with openclaw: recordTaskRunProgressByRunId on end
            try { tasks::get_task_executor().record_task_progress_by_run_id(run_id, done_text); } catch (...) {}
            emit(done_text, context_prefix + ":done");
            state->disposed.store(true);
            return;
        }

        if (phase == "error") {
            // Aligned with openclaw: flushReplaceableAssistantSnapshot() 先于 flushPending()
            flush_replaceable_assistant_snapshot();
            flush_pending();
            std::string error_text;
            if (event.data.contains("error") && event.data["error"].is_string())
                error_text = event.data["error"].get<std::string>();
            // trim
            size_t s = error_text.find_first_not_of(" \t\r\n");
            if (s != std::string::npos) error_text = error_text.substr(s);
            size_t e = error_text.find_last_not_of(" \t\r\n");
            if (e != std::string::npos) error_text = error_text.substr(0, e + 1);

            std::string fail_text = error_text.empty()
                ? (relay_label + " run failed.")
                : (relay_label + " run failed: " + error_text);
            // Aligned with openclaw: recordTaskRunProgressByRunId on error
            try { tasks::get_task_executor().record_task_progress_by_run_id(run_id, fail_text); } catch (...) {}
            emit(fail_text, context_prefix + ":error");
            state->disposed.store(true);
        }
    });

    // Flush timer thread (aligned with openclaw: scheduleFlush / setTimeout(flushPending, streamFlushMs))
    // Wakes every streamFlushMs to flush pending text even if buffer hasn't hit immediate-flush threshold.
    // This ensures partial lines are surfaced to the parent within streamFlushMs.
    if (stream_flush_ms > 0) {
        auto flush_timer_thread = std::make_shared<std::thread>([
            state, flush_pending, stream_flush_ms
        ]() {
            while (!state->disposed.load()) {
                std::this_thread::sleep_for(std::chrono::milliseconds(stream_flush_ms));
                if (state->disposed.load()) break;
                // Aligned with openclaw: scheduleFlush → flushPending()
                // Only flush if there's pending text (avoid empty emits)
                {
                    std::lock_guard<std::mutex> lock(state->text_mutex);
                    if (state->pending_text.empty()) continue;
                }
                flush_pending();
            }
        });
        flush_timer_thread->detach();
    }

    // Stall watcher thread (aligned with openclaw: noOutputWatcherTimer)
    // Aligned with openclaw: every noOutputPollMs, check if no output for noOutputNoticeMs
    auto stall_thread = std::make_shared<std::thread>([
        state, relay_label, context_prefix, emit,
        no_output_notice_ms, no_output_poll_ms, max_relay_lifetime_ms,
        run_id, child_session_key
    ]() {
        int64_t relay_start = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()
        ).count();

        while (!state->disposed.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(no_output_poll_ms));
            if (state->disposed.load()) break;

            int64_t now = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch()
            ).count();

            // Aligned with openclaw: maxRelayLifetimeMs timeout
            if (max_relay_lifetime_ms > 0 && (now - relay_start) >= max_relay_lifetime_ms) {
                int64_t secs = std::max((int64_t)1, (int64_t)max_relay_lifetime_ms / 1000);
                emit(relay_label + " stream relay timed out after " +
                     std::to_string(secs) + "s without completion.",
                     context_prefix + ":timeout");
                state->disposed.store(true);
                break;
            }

            // Aligned with openclaw: noOutputNoticeMs stall detection
            if (no_output_notice_ms > 0 && !state->stall_notified.load()) {
                int64_t last = state->last_progress_at.load();
                if ((now - last) >= no_output_notice_ms) {
                    state->stall_notified.store(true);
                    const int64_t secs =
                        std::max((int64_t)1, (int64_t)llround(no_output_notice_ms / 1000.0));
                    const std::string secs_str = std::to_string(secs);

                    // ── buildNoOutputNotice ──────────────────────────────────
                    // Aligned with openclaw: acp-spawn-parent-stream.ts:521-547
                    // 按停滞发生的阶段给出四级诊断，而非一句笼统的「无输出」。
                    std::string summary;   // 记入 task run progress
                    std::string stall_text; // 投递给父会话
                    if (state->prompt_submitted_at.load() == 0) {
                        // 阶段 1：子会话起来了，但从未观测到 prompt 提交
                        summary = "No prompt submission observed for " + secs_str +
                                  "s after child start.";
                        stall_text = relay_label + " session started but no prompt submission "
                                     "was observed for " + secs_str + "s.";
                    } else if (state->first_runtime_event_at.load() == 0) {
                        // 阶段 2：prompt 提交了，但上游一个 runtime 事件都没回
                        std::vector<std::string> keys;
                        {
                            std::lock_guard<std::mutex> lock(state->diag_mutex);
                            keys = state->proxy_env_keys_at_prompt;
                        }
                        const std::string proxy_summary = format_proxy_env_summary(keys);
                        summary = "Prompt submitted but no ACP runtime event for " + secs_str +
                                  "s (" + proxy_summary + ").";
                        stall_text = relay_label + " prompt was submitted but no ACP runtime "
                                     "event arrived for " + secs_str + "s (" + proxy_summary +
                                     "). Check upstream connectivity, auth, or proxy/network "
                                     "access in the gateway child environment.";
                    } else if (state->first_visible_output_at.load() == 0) {
                        // 阶段 3：runtime 有活动，但从未产出可见输出
                        std::string last_type;
                        {
                            std::lock_guard<std::mutex> lock(state->diag_mutex);
                            last_type = state->last_runtime_event_type;
                        }
                        const std::string last_event =
                            last_type.empty() ? "" : (" Last ACP event: " + last_type + ".");
                        summary = "ACP runtime active but no visible assistant output for " +
                                  secs_str + "s." + last_event;
                        stall_text = relay_label + " has ACP runtime activity but no visible "
                                     "assistant output for " + secs_str + "s." + last_event +
                                     " It may be working, blocked on a tool, or failing before "
                                     "visible output.";
                    } else {
                        // 阶段 4：曾有可见输出后卡住 —— 最可能在等交互输入
                        summary = "No visible output for " + secs_str +
                                  "s. It may be waiting for input.";
                        stall_text = relay_label + " has produced no visible output for " +
                                     secs_str + "s. It may be waiting for interactive input.";
                    }

                    // Aligned with openclaw: recordTaskRunProgressByRunId(summary) + emit(text)
                    try {
                        tasks::get_task_executor().record_task_progress_by_run_id(run_id, summary);
                    } catch (...) {}
                    emit(stall_text, context_prefix + ":stall");
                }
            }
        }
    });
    stall_thread->detach();

    auto dispose_fn = [state, unsubscribe, flush_log]() mutable {
        if (state->disposed.exchange(true)) return;
        unsubscribe();
        flush_log();
    };

    return { dispose_fn, emit_start_notice_fn };
}

// ============================================================================
// cleanup_failed_acp_spawn
// Aligned with openclaw: cleanupFailedAcpSpawn (control-plane/spawn.ts)
// ============================================================================

void cleanup_failed_acp_spawn(
    const std::string& session_key,
    bool should_delete_session,
    bool delete_transcript,
    std::shared_ptr<gateway::GatewayClient> gw,
    const AcpSpawnRuntimeCloseHandle* runtime_close_handle
) {
    // Step 1: Close the ACP runtime handle via AcpSessionManager
    // Aligned with openclaw: acpManager.closeSession({ reason: "spawn-failed", allowBackendUnavailable: true, requireAcpSession: false })
    try {
        acp::get_acp_session_manager().close_session(
            session_key,
            "spawn-failed",
            /*clear_meta=*/false,
            /*allow_backend_unavailable=*/true,
            /*require_acp_session=*/false
        );
    } catch (...) {
        log()->debug("acp-spawn: manager cleanup close failed for {}", session_key);
    }

    // Step 2: Unbind any thread binding (best-effort)
    // Aligned with openclaw: getSessionBindingService().unbind({ targetSessionKey, reason: "spawn-failed" })
    // Note: session_binding_service is handled by the outbound module in HunterClaw
    // TODO: wire up when outbound/session_binding_service is available
    // try {
    //     infra::get_session_binding_service().unbind(session_key, "spawn-failed");
    // } catch (...) {}

    // Step 3: Delete the Gateway session record (best-effort)
    // Aligned with openclaw: callGateway("sessions.delete", { key, deleteTranscript, emitLifecycleHooks: false })
    if (!should_delete_session || !gw) return;
    try {
        gw->request("sessions.delete", json{
            {"key", session_key},
            {"deleteTranscript", delete_transcript},
            {"emitLifecycleHooks", false}
        });
    } catch (...) {
        // Best-effort cleanup only
    }
}

// ============================================================================
// initialize_acp_spawn_runtime (internal helper)
// Aligned with openclaw: initializeAcpSpawnRuntime (agents/acp-spawn.ts)
// ============================================================================

struct AcpSpawnInitializedRuntime {
    acp::AcpSessionMeta meta;
    AcpSpawnRuntimeCloseHandle close_handle;
};

static std::optional<AcpSpawnInitializedRuntime> initialize_acp_spawn_runtime(
    const std::string& session_key,
    const std::string& target_agent_id,
    acp::AcpRuntimeSessionMode runtime_mode,
    const std::optional<std::string>& cwd,
    const std::optional<std::string>& backend_id,
    const std::optional<std::string>& resume_session_id,
    std::string& out_error
) {
    try {
        // Aligned with openclaw: getAcpSessionManager().initializeSession({ sessionKey, agent, mode, resumeSessionId, cwd, backendId })
        auto meta = acp::get_acp_session_manager().initialize_session(
            session_key,
            target_agent_id,
            runtime_mode,
            cwd,
            backend_id,
            resume_session_id
        );

        AcpSpawnRuntimeCloseHandle close_handle;
        close_handle.session_key = session_key;
        close_handle.backend = meta.backend;
        close_handle.runtime_session_name = meta.runtime_session_name;

        return AcpSpawnInitializedRuntime{ std::move(meta), std::move(close_handle) };
    } catch (const acp::AcpRuntimeError& e) {
        out_error = e.what();
        return std::nullopt;
    } catch (const std::exception& e) {
        out_error = e.what();
        return std::nullopt;
    }
}

// ============================================================================
// spawn_acp_direct
// Aligned with openclaw: spawnAcpDirect
// ============================================================================

SpawnAcpResult spawn_acp_direct(
    const SpawnAcpParams& params,
    const SpawnAcpContext& ctx,
    std::shared_ptr<gateway::GatewayClient> gw,
    const AcpSpawnConfig& cfg
) {
    // ✅ 对齐 OC createAcpSpawnFailure: 统一的错误返回，带 error_code
    auto make_failure = [](const std::string& status,
                           const std::string& error_code,
                           const std::string& error,
                           std::optional<std::string> session_key = std::nullopt,
                           std::optional<std::string> mode = std::nullopt) -> SpawnAcpResult {
        SpawnAcpResult r;
        r.status = status;
        r.child_session_key = session_key;
        r.mode = mode;
        r.error = error;
        r.error_code = error_code;
        return r;
    };

    // ── Step 1: Policy checks ─────────────────────────────────────────────
    // Aligned with openclaw: isAcpEnabledByPolicy(cfg)
    // HunterClaw: cfg.enable_acp
    if (!cfg.enable_acp) {
        return make_failure("forbidden", "acp_disabled",
                            "ACP is disabled by policy (`acp.enabled=false`).");
    }

    const bool stream_to_parent_requested = params.stream_to.has_value() && *params.stream_to == "parent";
    const std::string parent_session_key = ctx.agent_session_key.value_or("");

    if (stream_to_parent_requested && parent_session_key.empty()) {
        return make_failure("error", "requester_session_required",
                            "sessions_spawn streamTo=\"parent\" requires an active requester session context.");
    }

    // Aligned with openclaw: resolveAcpSpawnRuntimePolicyError
    auto policy_error = resolve_acp_spawn_runtime_policy_error(
        cfg,
        ctx.agent_session_key,
        ctx.sandboxed,
        params.sandbox
    );
    if (policy_error.has_value()) {
        return make_failure("forbidden", "runtime_policy", *policy_error);
    }

    // ── Step 2: Resolve spawn mode ────────────────────────────────────────
    // Aligned with openclaw: resolveSpawnMode
    const bool request_thread_binding = params.thread;
    std::string spawn_mode;
    if (params.mode.has_value() && (*params.mode == "run" || *params.mode == "session")) {
        spawn_mode = *params.mode;
    } else {
        spawn_mode = request_thread_binding ? "session" : "run";
    }

    if (spawn_mode == "session" && !request_thread_binding) {
        return make_failure("error", "thread_required",
                            "mode=\"session\" requires thread=true so the ACP session can stay bound to a thread.");
    }

    // ── Step 3: Resolve target agent ID ──────────────────────────────────
    // Aligned with openclaw: resolveTargetAcpAgentId
    std::string target_agent_id;
    if (params.agent_id.has_value() && !params.agent_id->empty()) {
        target_agent_id = *params.agent_id;
    } else if (!cfg.acp_default_agent.empty()) {
        target_agent_id = cfg.acp_default_agent;
    } else {
        // Fallback: derive from requester session key
        // e.g. "agent:main:subagent:xxx" → "main"
        if (!parent_session_key.empty()) {
            // parse "agent:{agentId}:..."
            const std::string prefix = "agent:";
            if (parent_session_key.rfind(prefix, 0) == 0) {
                std::string rest = parent_session_key.substr(prefix.size());
                size_t colon = rest.find(':');
                if (colon != std::string::npos) {
                    target_agent_id = rest.substr(0, colon);
                } else {
                    target_agent_id = rest;
                }
            }
        }
        if (target_agent_id.empty()) {
            return make_failure("error", "target_agent_required",
                "ACP target agent is not configured. Pass `agentId` in `sessions_spawn` or set `acp.defaultAgent` in config.");
        }
    }

    // Aligned with openclaw: resolveAcpAgentPolicyError (allowedAgents whitelist)
    if (!cfg.acp_allowed_agents.empty()) {
        bool allowed = false;
        for (const auto& a : cfg.acp_allowed_agents) {
            if (a == target_agent_id) { allowed = true; break; }
        }
        if (!allowed) {
            return make_failure("forbidden", "agent_forbidden",
                "ACP agent \"" + target_agent_id + "\" is not in the allowed agents list.");
        }
    }

    // ── Step 4: Resolve requester internal key ────────────────────────────
    // Aligned with openclaw: resolveRequesterInternalSessionKey
    std::string requester_internal_key = parent_session_key;
    if (requester_internal_key.empty()) {
        requester_internal_key = "agent:" + target_agent_id + ":main";
    }

    // ── Step 5: implicitStreamToParent ────────────────────────────────────────
    // Aligned with openclaw: resolveAcpSpawnStreamPlan
    //
    // implicitStreamToParent = !streamToParentRequested
    //   && spawnMode == "run"
    //   && !requestThreadBinding
    //   && requester.isSubagentSession
    //   && !requester.hasActiveSubagentBinding
    //   && !requester.hasThreadContext
    //   && requester.heartbeatEnabled
    //   && requester.heartbeatRelayRouteUsable
    //
    // Note: infra::resolve_implicit_stream_to_parent encapsulates the heartbeat config check.
    // TODO: wire up when infra helper is available. For now, only explicit streamTo="parent" is supported.
    bool implicit_stream_to_parent = false;
    // if (!stream_to_parent_requested && spawn_mode == "run" && !request_thread_binding
    //     && !parent_session_key.empty()) {
    //     implicit_stream_to_parent = infra::resolve_implicit_stream_to_parent(parent_session_key, cfg);
    // }
    const bool effective_stream_to_parent = stream_to_parent_requested || implicit_stream_to_parent;
    // ── Step 5b: subagentEnvelopeState check ─────────────────────────────────
    // Aligned with openclaw: resolveAcpSubagentEnvelopeState
    //   → maxSpawnDepth / maxChildrenPerAgent / requireAgentId / allowAgents
    //   → errorCode: "subagent_policy"
    if (!parent_session_key.empty() && is_subagent_envelope_session(parent_session_key)) {
        // Resolve capabilities for the parent session to check spawn limits
        SubagentCapabilities parent_caps = resolve_stored_subagent_capabilities(parent_session_key);

        // Aligned with openclaw: requireAgentId — if parent is a subagent envelope, agentId must be explicit
        if (!params.agent_id.has_value() || params.agent_id->empty()) {
            return make_failure("forbidden", "subagent_policy",
                "sessions_spawn from a subagent session requires an explicit agentId.");
        }

        // Aligned with openclaw: maxSpawnDepth check
        // Default max depth = 3 (main → orchestrator → leaf)
        constexpr int DEFAULT_MAX_SPAWN_DEPTH = 3;
        if (parent_caps.depth >= DEFAULT_MAX_SPAWN_DEPTH) {
            return make_failure("forbidden", "subagent_policy",
                "Maximum ACP spawn depth (" + std::to_string(DEFAULT_MAX_SPAWN_DEPTH) +
                ") reached. Cannot spawn further nested subagents.");
        }

        // Aligned with openclaw: can_spawn check (leaf role cannot spawn)
        if (!parent_caps.can_spawn) {
            return make_failure("forbidden", "subagent_policy",
                "This subagent session (role=\"" + parent_caps.role +
                "\") is not permitted to spawn child agents.");
        }
    }

    // ── Step 6: Generate child session key ───────────────────────────────────
    // Aligned with openclaw: `agent:${targetAgentId}:acp:${crypto.randomUUID()}`
    const std::string session_key = "agent:" + target_agent_id + ":acp:" + generate_uuid();
    // ── Step 6b: Resolve effective Gateway client ─────────────────────────
    // 🎯 对齐 OpenClaw: 真正的跨进程 ACP 路由
    //
    // OpenClaw 设计:
    //   callGateway("agent", params) 通过 WebSocket 连接发到另一个 Gateway 实例。
    //   targetAgentId 决定路由到哪个远端 Gateway（不同进程/机器）。
    //
    // HunterClaw 实现:
    //   - 如果 cfg.acp_gateway_url 非空: 创建真正的 WebSocket GatewayClient 连接到远端
    //   - 否则: 使用传入的 gw（同进程内 method_handler，向后兼容）
    //
    // 注意: 远端 GatewayClient 在 spawn 完成后会被 stop()，因为 ACP 是 fire-and-forget。
    // 父子通信通过 ParentStreamRelay（进程内事件总线）或 subagent announce（跨进程消息）完成。
    std::shared_ptr<gateway::GatewayClient> effective_gw = gw;
    std::shared_ptr<gateway::GatewayClient> remote_gw;  // owned by this spawn call

    if (!cfg.acp_gateway_url.empty()) {
        // 🎯 对齐 OC: 创建真正的 WebSocket GatewayClient 连接到远端 Gateway
        gateway::GatewayClientOptions gw_opts;
        gw_opts.url = cfg.acp_gateway_url;
        gw_opts.client_name = "hunterclaw-acp-spawn";
        gw_opts.client_version = "1.0.0";
        gw_opts.mode = "headless";
        gw_opts.role = "operator";
        gw_opts.scopes = {"admin"};
        if (!cfg.acp_gateway_token.empty()) {
            gw_opts.token = cfg.acp_gateway_token;
        }
        if (!cfg.acp_gateway_instance_id.empty()) {
            gw_opts.instance_id = cfg.acp_gateway_instance_id;
        }
        // 短超时的 tick watch（spawn 完成后立即 stop）
        gw_opts.tick_watch_min_interval_ms = 60000;

        auto gw_result = gateway::create_gateway_client(gw_opts);
        if (!gw_result.is_ok()) {
            return make_failure("error", "spawn_failed",
                "Failed to create remote gateway client: " + gw_result.error,
                session_key, spawn_mode);
        }
        remote_gw = gw_result.get();
        remote_gw->start();

        // Wait for connection (up to 10s)
        // 🎯 对齐 OC: GatewayClient 连接是异步的，需要等待 hello.ok
        constexpr int CONNECT_TIMEOUT_MS = 10000;
        constexpr int POLL_INTERVAL_MS = 50;
        int waited_ms = 0;
        while (!remote_gw->is_connected() && waited_ms < CONNECT_TIMEOUT_MS) {
            std::this_thread::sleep_for(std::chrono::milliseconds(POLL_INTERVAL_MS));
            waited_ms += POLL_INTERVAL_MS;
        }
        if (!remote_gw->is_connected()) {
            remote_gw->stop();
            return make_failure("error", "spawn_failed",
                "Timeout connecting to remote gateway: " + cfg.acp_gateway_url,
                session_key, spawn_mode);
        }

        effective_gw = remote_gw;
        log()->info("spawn_acp_direct: using remote gateway {} for agent {}",
            cfg.acp_gateway_url, target_agent_id);
    }

    if (!effective_gw) {
        return make_failure("error", "spawn_failed", "Gateway not available", session_key, spawn_mode);
    }

    // ── Step 6b: Resolve runtime cwd (with filesystem validation) ───────────
    // Aligned with openclaw: resolveSpawnedWorkspaceInheritance + resolveRuntimeCwdForAcpSpawn
    //   → if cwd path does not exist (ENOENT), silently drop it (return undefined)
    //   → if other error, return errorCode: "cwd_resolution_failed"
    std::optional<std::string> runtime_cwd;
    {
        // resolveSpawnedWorkspaceInheritance: use explicit cwd if provided, otherwise inherit from parent
        // Note: infra::resolve_spawned_workspace_inheritance is implemented in the agents/spawned-context module.
        // For now, use params.cwd directly (explicit cwd takes priority).
        std::optional<std::string> resolved_cwd = params.cwd;
        if (!resolved_cwd.has_value() || resolved_cwd->empty()) {
            // Aligned with openclaw: resolveSpawnedWorkspaceInheritance
            // Inherit cwd from parent ACP session meta if available.
            if (!parent_session_key.empty()) {
                try {
                    auto parent_res = acp::get_acp_session_manager().resolve_session(parent_session_key);
                    if (parent_res.kind == acp::AcpSessionResolution::Kind::Ready && parent_res.meta) {
                        if (parent_res.meta->cwd.has_value() && !parent_res.meta->cwd->empty()) {
                            resolved_cwd = parent_res.meta->cwd;
                        } else if (parent_res.meta->runtime_options.cwd.has_value() &&
                                   !parent_res.meta->runtime_options.cwd->empty()) {
                            resolved_cwd = parent_res.meta->runtime_options.cwd;
                        }
                    }
                } catch (...) {
                    // AcpSessionManager not initialized or parent not an ACP session — skip
                }
            }
        }
        if (resolved_cwd.has_value() && !resolved_cwd->empty()) {
            if (params.cwd.has_value() && !params.cwd->empty()) {
                // Explicit cwd: use as-is
                runtime_cwd = resolved_cwd;
            } else {
                // Inherited cwd: validate existence
                std::error_code ec;
                bool exists = std::filesystem::exists(*resolved_cwd, ec);
                if (ec) {
                    if (remote_gw) remote_gw->stop();
                    return make_failure("error", "cwd_resolution_failed",
                        "Failed to resolve cwd: " + ec.message());
                }
                if (exists) {
                    runtime_cwd = resolved_cwd;
                }
            }
        }
    }

    // ── Step 7: Create Gateway session (sessions.patch) ──────────────────
    // 🎯 对齐 OpenClaw: callGateway("sessions.patch", { key, spawnedBy, ...subagentEnvelopePatch, label })
    // 使用 effective_gw（可能是远端 WebSocket 客户端）
    bool session_created = false;
    {
        json patch = {
            {"key", session_key},
            {"spawnedBy", requester_internal_key}
        };
        if (params.label.has_value() && !params.label->empty()) {
            patch["label"] = *params.label;
        }
        // Aligned with openclaw: subagentEnvelopeState.childSessionPatch
        // (spawnDepth, subagentRole, subagentControlScope)
        // Resolve child capabilities from parent session to set correct depth/role/controlScope
        if (!parent_session_key.empty()) {
            SubagentCapabilities parent_caps = resolve_stored_subagent_capabilities(parent_session_key);
            int child_depth = parent_caps.depth + 1;
            // Aligned with openclaw: resolveSubagentRoleForDepth
            SubagentCapabilities child_caps = resolve_subagent_capabilities_for_depth(child_depth);
            // Inject subagentEnvelopePatch fields into sessions.patch
            patch["spawnDepth"] = child_depth;
            if (!child_caps.role.empty()) patch["subagentRole"] = child_caps.role;
            if (!child_caps.control_scope.empty()) patch["subagentControlScope"] = child_caps.control_scope;
        }
        auto patch_result = effective_gw->request("sessions.patch", patch);
        if (!patch_result.is_ok()) {
            if (remote_gw) remote_gw->stop();
            return make_failure("error", "spawn_failed",
                                patch_result.error, session_key, spawn_mode);
        }
        session_created = true;
    }

    // ── Step 7b: Initialize ACP runtime via AcpSessionManager ────────────
    // 🎯 对齐 OpenClaw: initializeAcpSpawnRuntime()
    //   → getAcpSessionManager().initializeSession({ sessionKey, agent, mode, cwd, backendId })
    //   → 持久化 session meta（acp 字段）到 session store
    //   → 缓存 runtime handle（供后续 runTurn 复用）
    //
    // 这是 HunterClaw 与 OpenClaw 对齐的关键步骤：
    //   - OpenClaw: agents/acp-spawn.ts → initializeAcpSpawnRuntime() → AcpSessionManager.initializeSession()
    //   - HunterClaw: 同样通过 acp::get_acp_session_manager().initialize_session() 完成
    //
    // 注意: AcpSessionManager 需要已通过 AcpSessionManager::set_instance() 初始化。
    // 如果未初始化（singleton 未设置），initialize_acp_spawn_runtime 会捕获异常并返回 nullopt。
    std::optional<AcpSpawnRuntimeCloseHandle> initialized_runtime_handle;
    {
        // Resolve runtime mode: "run" → oneshot, "session" → persistent
        acp::AcpRuntimeSessionMode runtime_mode =
            (spawn_mode == "session")
                ? acp::AcpRuntimeSessionMode::Persistent
                : acp::AcpRuntimeSessionMode::Oneshot;

        std::string init_error;
        auto initialized = initialize_acp_spawn_runtime(
            session_key,
            target_agent_id,
            runtime_mode,
            runtime_cwd,  // validated cwd (may be nullopt if path not found)
            std::nullopt,  // backend_id: use default from AcpSessionManager
            params.resume_session_id,
            init_error
        );

        if (!initialized.has_value()) {
            // Aligned with openclaw: cleanupFailedAcpSpawn on initializeAcpSpawnRuntime failure
            cleanup_failed_acp_spawn(session_key, session_created, true, effective_gw, nullptr);
            if (remote_gw) remote_gw->stop();
            return make_failure("error", "spawn_failed", init_error, session_key, spawn_mode);
        }

        initialized_runtime_handle = std::move(initialized->close_handle);
    }

    // ── Step 8: Start parent stream relay (before dispatch) ──────────────
    // Aligned with openclaw: register relay BEFORE dispatch so fast lifecycle failures are not missed
    const std::string child_idem = generate_uuid();
    std::string child_run_id = child_idem;

    // Aligned with openclaw: resolveAcpSpawnStreamLogPath({ childSessionKey })
    std::optional<std::string> stream_log_path;
    if (effective_stream_to_parent && !parent_session_key.empty()) {
        // Aligned with openclaw: resolveAcpSpawnStreamLogPath reads ACP session meta to find cwd,
        // then builds: <cwd>/.hunterclaw/acp-spawn-logs/<session_key_hash>.jsonl
        try {
            auto child_res = acp::get_acp_session_manager().resolve_session(session_key);
            std::string log_cwd;
            if (child_res.kind == acp::AcpSessionResolution::Kind::Ready && child_res.meta) {
                if (child_res.meta->cwd.has_value() && !child_res.meta->cwd->empty()) {
                    log_cwd = *child_res.meta->cwd;
                }
            }
            if (log_cwd.empty() && runtime_cwd.has_value()) {
                log_cwd = *runtime_cwd;
            }
            if (!log_cwd.empty()) {
                // Build log path: <cwd>/.hunterclaw/acp-spawn-logs/<session_key>.jsonl
                // Sanitize session_key for use as filename (replace ':' with '_')
                std::string safe_key = session_key;
                std::replace(safe_key.begin(), safe_key.end(), ':', '_');
                stream_log_path = log_cwd + "/.hunterclaw/acp-spawn-logs/" + safe_key + ".jsonl";
            }
        } catch (...) {}
    }

    // Aligned with openclaw: deliveryContextFromSession(parentEntry)
    // Routes system events to the correct thread/topic of the parent session.
    std::optional<json> parent_delivery_ctx;
    if (effective_stream_to_parent && !parent_session_key.empty()) {
        // Aligned with openclaw: deliveryContextFromSession — build delivery context from
        // the parent session's last known channel/to/accountId/threadId.
        // In HunterClaw, this is available via the SpawnAcpContext (ctx) which carries
        // the agent's current delivery context.
        if (ctx.agent_channel.has_value() && !ctx.agent_channel->empty() &&
            ctx.agent_to.has_value() && !ctx.agent_to->empty()) {
            json dctx;
            dctx["channel"] = *ctx.agent_channel;
            dctx["to"] = *ctx.agent_to;
            if (ctx.agent_account_id.has_value() && !ctx.agent_account_id->empty()) {
                dctx["accountId"] = *ctx.agent_account_id;
            }
            if (ctx.agent_thread_id.has_value() && !ctx.agent_thread_id->empty()) {
                dctx["threadId"] = *ctx.agent_thread_id;
            }
            parent_delivery_ctx = dctx;
        }
    }

    AcpSpawnParentRelayHandle parent_relay;
    if (effective_stream_to_parent && !parent_session_key.empty()) {
        parent_relay = start_acp_spawn_parent_stream_relay(
            child_idem,
            parent_session_key,
            session_key,
            target_agent_id,
            stream_log_path,   // resolved log path
            2500,              // stream_flush_ms
            60000,             // no_output_notice_ms
            15000,             // no_output_poll_ms
            6 * 60 * 60 * 1000, // max_relay_lifetime_ms
            false,             // emit_start_notice (will call notifyStarted after dispatch)
            parent_delivery_ctx, // deliveryContext (json)
            true,              // surfaceUpdates
            // Aligned with openclaw: cfg — progressCommentary / acp.projection.tagVisibility
            cfg.raw_config.is_null() ? std::nullopt : std::optional<json>(cfg.raw_config)
        );
    }

    // ── Step 9: Dispatch agent turn ───────────────────────────────────────
    // Aligned with openclaw: callGateway("agent", { message, sessionKey, idempotencyKey, deliver, label })
    try {
        json agent_params = {
            {"message",        params.task},
            {"sessionKey",     session_key},
            {"idempotencyKey", child_idem},
            {"deliver",        false}
        };
        if (params.label.has_value() && !params.label->empty()) {
            agent_params["label"] = *params.label;
        }
        // Aligned with openclaw: useInlineDelivery = hasDeliveryTarget && spawnMode=="session" && !effectiveStreamToParent
        // For thread-bound sessions, inject delivery context
        if (spawn_mode == "session" && !effective_stream_to_parent &&
            ctx.agent_channel.has_value() && !ctx.agent_channel->empty() &&
            ctx.agent_to.has_value() && !ctx.agent_to->empty()) {
            agent_params["deliver"] = true;
            agent_params["channel"] = *ctx.agent_channel;
            agent_params["to"] = *ctx.agent_to;
            if (ctx.agent_account_id.has_value()) agent_params["accountId"] = *ctx.agent_account_id;
            if (ctx.agent_thread_id.has_value()) agent_params["threadId"] = *ctx.agent_thread_id;
        }

        // 🎯 对齐 OpenClaw: callGateway("agent", params) — 通过 effective_gw 发到目标 Gateway
        // 如果 effective_gw 是远端 WebSocket 客户端，这里就是真正的跨进程 ACP 调用
        auto response = effective_gw->request<json>("agent", agent_params);
        if (!response.is_ok()) {
            if (parent_relay.dispose) parent_relay.dispose();
            // Aligned with openclaw: cleanupFailedAcpSpawn on dispatch failure
            cleanup_failed_acp_spawn(
                session_key, /*should_delete_session=*/true, /*delete_transcript=*/true,
                effective_gw, initialized_runtime_handle ? &*initialized_runtime_handle : nullptr
            );
            if (remote_gw) remote_gw->stop();
            return make_failure("error", "dispatch_failed",
                                response.error, session_key, spawn_mode);
        }
        const auto& resp = response.get();
        if (resp.contains("run_id") && resp["run_id"].is_string()) {
            std::string resp_run_id = resp["run_id"].get<std::string>();
            // trim
            size_t s = resp_run_id.find_first_not_of(" \t");
            if (s != std::string::npos) resp_run_id = resp_run_id.substr(s);
            if (!resp_run_id.empty()) child_run_id = resp_run_id;
        }
        // Also check camelCase runId
        if (child_run_id == child_idem && resp.contains("runId") && resp["runId"].is_string()) {
            std::string resp_run_id = resp["runId"].get<std::string>();
            size_t s = resp_run_id.find_first_not_of(" \t");
            if (s != std::string::npos) resp_run_id = resp_run_id.substr(s);
            if (!resp_run_id.empty()) child_run_id = resp_run_id;
        }
    } catch (const std::exception& e) {
        if (parent_relay.dispose) parent_relay.dispose();
        // Aligned with openclaw: cleanupFailedAcpSpawn on dispatch exception
        cleanup_failed_acp_spawn(
            session_key, /*should_delete_session=*/true, /*delete_transcript=*/true,
            effective_gw, initialized_runtime_handle ? &*initialized_runtime_handle : nullptr
        );
        if (remote_gw) remote_gw->stop();
        return make_failure("error", "dispatch_failed",
                            std::string(e.what()), session_key, spawn_mode);
    }

    // ── Step 10: Finalize relay ───────────────────────────────────────────
    // Aligned with openclaw: if childRunId !== childIdem, restart relay with correct runId
    if (effective_stream_to_parent && !parent_session_key.empty()) {
        if (child_run_id != child_idem) {
            // Restart relay with correct runId
            if (parent_relay.dispose) parent_relay.dispose();
            parent_relay = start_acp_spawn_parent_stream_relay(
                child_run_id,
                parent_session_key,
                session_key,
                target_agent_id,
                stream_log_path,
                2500, 60000, 15000, 6 * 60 * 60 * 1000, false,
                parent_delivery_ctx, true,  // delivery_context (json), surface_updates
                // Aligned with openclaw: cfg 与首次创建保持一致
                cfg.raw_config.is_null() ? std::nullopt : std::optional<json>(cfg.raw_config)
            );
        }
        // Aligned with openclaw: parentRelay.notifyStarted()
        if (parent_relay.notify_started) parent_relay.notify_started();
    }

    // ── Step 11: Register background task run ────────────────────────────────
    // Aligned with openclaw: createRunningTaskRun({ runtime: "acp", sourceId, ownerKey, ... })
    // Used for sessions_list display and task lifecycle tracking.
    try {
        auto& task_exec = tasks::get_task_executor();
        tasks::TaskRecord task_params;
        task_params.runtime = tasks::TaskRuntime::ACP;
        task_params.run_id = child_run_id;
        task_params.source_id = child_idem;  // idempotency key as source
        task_params.requester_session_key = parent_session_key.empty() ? session_key : parent_session_key;
        task_params.owner_key = requester_internal_key;
        task_params.child_session_key = session_key;
        task_params.agent_id = target_agent_id;
        task_params.label = params.label;
        task_params.task = params.task.substr(0, 200);  // truncate for display
        task_params.status = tasks::TaskStatus::RUNNING;
        task_params.scope_kind = tasks::TaskScopeKind::SESSION;
        task_params.notify_policy = tasks::TaskNotifyPolicy::DONE_ONLY;
        task_params.created_at = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        task_params.started_at = task_params.created_at;
        task_exec.create_running_task_run(task_params);
    } catch (...) {
        // Best-effort: task tracking failure must not abort spawn
    }

    // ── Step 12: Return result ────────────────────────────────────────────
    // 🎯 对齐 OC: fire-and-forget — spawn 完成后 stop 远端 gw（连接不再需要）
    // 父子通信通过 ParentStreamRelay（进程内事件总线）或 subagent announce（跨进程消息）完成
    if (remote_gw) {
        // 异步 stop，避免阻塞 spawn 返回
        auto gw_to_stop = remote_gw;
        std::thread([gw_to_stop]() {
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
            gw_to_stop->stop();
        }).detach();
    }

    std::string note = (spawn_mode == "session")
        ? ACP_SPAWN_SESSION_ACCEPTED_NOTE
        : ACP_SPAWN_ACCEPTED_NOTE;

    SpawnAcpResult ok;
    ok.status = "accepted";
    ok.child_session_key = session_key;
    ok.run_id = child_run_id;
    ok.mode = spawn_mode;
    // Aligned with openclaw: streamLogPath in result when effectiveStreamToParent
    ok.stream_log_path = (effective_stream_to_parent && stream_log_path.has_value())
        ? stream_log_path : std::nullopt;
    ok.note = note;
    ok.error = std::nullopt;
    ok.error_code = std::nullopt;
    return ok;
}
} // namespace agents
} // namespace hunterclaw
