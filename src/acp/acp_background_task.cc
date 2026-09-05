// 严格对齐 OpenClaw: src/acp/control-plane/manager.background-task.ts (221行)

#include "hunterclaw/acp/acp_background_task.h"

#include "hunterclaw/tasks/task_completion_contract.h"
#include "hunterclaw/tasks/task_executor.h"

#include <chrono>
#include <regex>

#include <spdlog/spdlog.h>

namespace hunterclaw {
namespace acp {

namespace {

/// 对齐 OC: logVerbose —— 本模块所有失败都只记日志、不抛，
/// 因为 background task 只是父侧可观测性镜像，写失败不应让子会话 turn 失败。
void log_background_task_warn(const std::string& message) {
    auto logger = spdlog::get("acp_manager");
    if (!logger) logger = spdlog::get("");
    if (logger) {
        logger->warn("acp-manager: {}", message);
    }
}

/// 对齐 OC: normalizeText —— trim 后空串归为 nullopt
std::optional<std::string> normalize_text(const std::optional<std::string>& value) {
    if (!value) {
        return std::nullopt;
    }
    size_t begin = 0;
    while (begin < value->size() && static_cast<unsigned char>((*value)[begin]) <= 0x20) {
        ++begin;
    }
    size_t end = value->size();
    while (end > begin && static_cast<unsigned char>((*value)[end - 1]) <= 0x20) {
        --end;
    }
    if (end <= begin) {
        return std::nullopt;
    }
    return value->substr(begin, end - begin);
}

/// 折叠所有空白为单空格（对齐 OC: replace(/\s+/g, " ")）
std::string collapse_whitespace(const std::string& value) {
    std::string out;
    out.reserve(value.size());
    bool in_space = false;
    for (char c : value) {
        const bool is_space = static_cast<unsigned char>(c) <= 0x20;
        if (is_space) {
            if (!in_space) {
                out.push_back(' ');
                in_space = true;
            }
        } else {
            out.push_back(c);
            in_space = false;
        }
    }
    return out;
}

std::string ltrim_copy(const std::string& s) {
    size_t begin = 0;
    while (begin < s.size() && static_cast<unsigned char>(s[begin]) <= 0x20) {
        ++begin;
    }
    return s.substr(begin);
}

std::string trim_copy(const std::string& s) {
    const std::string left = ltrim_copy(s);
    size_t end = left.size();
    while (end > 0 && static_cast<unsigned char>(left[end - 1]) <= 0x20) {
        --end;
    }
    return left.substr(0, end);
}

int64_t now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

} // namespace

std::string summarize_background_task_text(const std::string& text) {
    // 对齐 OC: normalizeText(text) ?? "ACP background task"
    const auto normalized = normalize_text(std::optional<std::string>(text));
    if (!normalized) {
        return "ACP background task";
    }
    if (normalized->size() <= ACP_BACKGROUND_TASK_TEXT_MAX_LENGTH) {
        return *normalized;
    }
    // 对齐 OC: `${truncateUtf16Safe(normalized, MAX - 1)}…`
    return tasks::truncate_utf8_safe(*normalized, ACP_BACKGROUND_TASK_TEXT_MAX_LENGTH - 1) +
           "\xE2\x80\xA6"; // U+2026 HORIZONTAL ELLIPSIS
}

std::string append_background_task_progress_summary(const std::string& current,
                                                   const std::string& chunk) {
    // 对齐 OC: chunk.replace(/\s+/g, " ")
    const std::string normalized_chunk = collapse_whitespace(chunk);
    if (normalized_chunk.empty()) {
        return current;
    }
    // 对齐 OC: current ? normalizedChunk : normalizedChunk.trimStart()
    // 首段去掉前导空白，续接段保留（保住词边界空格）
    const std::string chunk_to_append =
        current.empty() ? ltrim_copy(normalized_chunk) : normalized_chunk;
    if (chunk_to_append.empty()) {
        return current;
    }
    const std::string combined = collapse_whitespace(current + chunk_to_append);
    if (combined.size() <= ACP_BACKGROUND_TASK_PROGRESS_MAX_LENGTH) {
        return combined;
    }
    return tasks::truncate_utf8_safe(combined, ACP_BACKGROUND_TASK_PROGRESS_MAX_LENGTH - 1) +
           "\xE2\x80\xA6";
}

tasks::TaskStatus resolve_background_task_failure_status(const AcpRuntimeError& error) {
    // 对齐 OC: error.detailCode === ACP_TURN_TIMEOUT_DETAIL_CODE ? "timed_out" : "failed"
    if (error.detail_code && *error.detail_code == ACP_TURN_TIMEOUT_DETAIL_CODE) {
        return tasks::TaskStatus::TIMED_OUT;
    }
    return tasks::TaskStatus::FAILED;
}

AcpBackgroundTaskTerminalResult resolve_background_task_terminal_result(
    const std::string& progress_summary) {
    // 对齐 OC: 先走 required-completion 契约
    const auto required = tasks::resolve_required_completion_terminal_result(progress_summary);
    if (required.terminal_outcome) {
        return AcpBackgroundTaskTerminalResult{required.terminal_outcome,
                                               required.terminal_summary};
    }

    const std::string normalized = trim_copy(collapse_whitespace(progress_summary));
    if (normalized.empty()) {
        return AcpBackgroundTaskTerminalResult{};
    }

    // 对齐 OC: /\b(?:write failed:\s*)?permission denied(?: for (?<path>\S+))?\.?/i
    static const std::regex permission_denied_re(
        "\\b(?:write failed:[[:space:]]*)?permission denied"
        "(?: for ([^[:space:]]+))?\\.?",
        std::regex::icase | std::regex::optimize);
    std::smatch m;
    if (std::regex_search(normalized, m, permission_denied_re)) {
        std::optional<std::string> path;
        if (m[1].matched) {
            // 对齐 OC: path?.replace(/[.,;:!?]+$/, "")
            std::string raw = m[1].str();
            size_t end = raw.size();
            while (end > 0 && std::string(".,;:!?").find(raw[end - 1]) != std::string::npos) {
                --end;
            }
            raw = raw.substr(0, end);
            path = normalize_text(std::optional<std::string>(raw));
        }
        return AcpBackgroundTaskTerminalResult{
            tasks::TaskTerminalOutcome::BLOCKED,
            path ? ("Permission denied for " + *path + ".") : std::string("Permission denied.")};
    }

    // 对齐 OC: 三条 writable-session 判据
    static const std::regex writable_session_re("\\bneed a writable session\\b",
                                                std::regex::icase | std::regex::optimize);
    static const std::regex fs_auth_re("\\bfilesystem authorization\\b",
                                       std::regex::icase | std::regex::optimize);
    static const std::regex apply_patch_re("`?apply_patch`?",
                                           std::regex::icase | std::regex::optimize);
    if (std::regex_search(normalized, writable_session_re) ||
        std::regex_search(normalized, fs_auth_re) ||
        std::regex_search(normalized, apply_patch_re)) {
        return AcpBackgroundTaskTerminalResult{
            tasks::TaskTerminalOutcome::BLOCKED,
            "Writable session or apply_patch authorization required."};
    }

    return AcpBackgroundTaskTerminalResult{};
}

void create_background_task_record(const AcpBackgroundTaskContext& context, int64_t started_at) {
    // 对齐 OC: createBackgroundTaskRecord —— 失败只 logVerbose，绝不抛
    try {
        tasks::TaskRecord params;
        params.runtime = tasks::TaskRuntime::ACP;
        params.source_id = context.run_id;
        params.owner_key = context.requester_session_key;
        params.requester_session_key = context.requester_session_key;
        params.scope_kind = tasks::TaskScopeKind::SESSION;
        params.child_session_key = context.child_session_key;
        params.run_id = context.run_id;
        params.label = context.label;
        params.task = context.task;
        params.started_at = started_at;
        params.created_at = started_at;
        params.status = tasks::TaskStatus::RUNNING;

        auto created = tasks::get_task_executor().create_running_task_run(params);
        if (created.task_id.empty()) {
            log_background_task_warn("failed creating background task for " +
                                     context.run_id + ": persist_failed");
        }
    } catch (const std::exception& e) {
        log_background_task_warn("failed creating background task for " + context.run_id + ": " +
                                 e.what());
    } catch (...) {
        log_background_task_warn("failed creating background task for " + context.run_id +
                                 ": unknown error");
    }
}

void mark_background_task_running(const std::string& run_id,
                                  const std::optional<std::string>& progress_summary) {
    // 对齐 OC: markBackgroundTaskRunning —— startTaskRunByRunId + 进度落盘
    try {
        auto& executor = tasks::get_task_executor();
        executor.start_task_run_by_run_id(run_id);
        executor.record_task_progress_by_run_id(run_id, progress_summary);
    } catch (const std::exception& e) {
        log_background_task_warn("failed updating background task for " + run_id + ": " + e.what());
    } catch (...) {
        log_background_task_warn("failed updating background task for " + run_id +
                                 ": unknown error");
    }
}

void mark_background_task_terminal(const std::string& run_id,
                                   const MarkBackgroundTaskTerminalParams& params) {
    // 对齐 OC: markBackgroundTaskTerminal —— succeeded 走 complete，其余走 fail
    try {
        auto& executor = tasks::get_task_executor();
        const int64_t ended_at = params.ended_at != 0 ? params.ended_at : now_ms();

        // 进度摘要先落盘，避免终态写入覆盖掉最后一段进度
        if (params.progress_summary) {
            executor.record_task_progress_by_run_id(run_id, params.progress_summary);
        }

        if (params.status == tasks::TaskStatus::SUCCEEDED) {
            executor.complete_task_run_by_run_id(run_id, ended_at, params.terminal_summary,
                                                 params.terminal_outcome);
            return;
        }
        executor.fail_task_run_by_run_id(run_id, ended_at, params.error, params.status);
    } catch (const std::exception& e) {
        log_background_task_warn("failed updating background task for " + run_id + ": " + e.what());
    } catch (...) {
        log_background_task_warn("failed updating background task for " + run_id +
                                 ": unknown error");
    }
}

} // namespace acp
} // namespace hunterclaw
