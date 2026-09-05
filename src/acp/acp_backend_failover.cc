// 严格对齐 OpenClaw: src/acp/control-plane/manager.backend-failover.ts

#include "hunterclaw/acp/acp_backend_failover.h"

#include <algorithm>
#include <regex>
#include <set>

namespace hunterclaw {
namespace acp {

namespace {

/// 对齐 OC: normalizeText —— trim 后空串视为"未提供"
std::string normalize_text(const std::string& value) {
    size_t begin = 0;
    while (begin < value.size() && static_cast<unsigned char>(value[begin]) <= 0x20) {
        ++begin;
    }
    size_t end = value.size();
    while (end > begin && static_cast<unsigned char>(value[end - 1]) <= 0x20) {
        --end;
    }
    if (end <= begin) {
        return std::string();
    }
    return value.substr(begin, end - begin);
}

} // namespace

BackendCandidatePlan resolve_backend_candidate_plan(
    const std::string& configured_primary_backend,
    const std::string& resolved_primary_backend,
    const std::vector<std::string>& fallback_backends)
{
    const std::string configured = normalize_text(configured_primary_backend);
    const std::string resolved = normalize_text(resolved_primary_backend);

    BackendCandidatePlan plan;

    // 对齐 OC: new Set([configured ?? resolved ?? "", ...fallbacks])
    // 首项即使是空串也必须保留 —— 空串让 registry 走默认 backend。
    const std::string primary = !configured.empty() ? configured : resolved;

    std::set<std::string> seen;
    plan.candidate_backends.push_back(primary);
    seen.insert(primary);

    for (const auto& raw : fallback_backends) {
        const std::string normalized = normalize_text(raw);
        // 对齐 OC: .filter((b): b is string => b != null) —— 空值被过滤
        if (normalized.empty()) continue;
        if (seen.insert(normalized).second) {
            plan.candidate_backends.push_back(normalized);
        }
    }

    // 对齐 OC: describeBackendCandidate = (b) => b || resolved || configured || "<auto>"
    plan.describe_backend_candidate = [resolved, configured](const std::string& backend) {
        if (!backend.empty()) return backend;
        if (!resolved.empty()) return resolved;
        if (!configured.empty()) return configured;
        return std::string("<auto>");
    };

    return plan;
}

bool is_failover_worthy_backend_error(const BackendAttempt& attempt) {
    // 对齐 OC: !attempt.sawOutput —— 已经产出可见输出就不能换 backend，
    // 否则用户会看到同一问题被回答两遍。
    if (attempt.saw_output) {
        return false;
    }

    if (attempt.code != AcpErrorCode::ACP_TURN_FAILED &&
        attempt.code != AcpErrorCode::ACP_SESSION_INIT_FAILED &&
        attempt.code != AcpErrorCode::ACP_BACKEND_UNAVAILABLE) {
        return false;
    }

    // 对齐 OC 正则（逐词一致）：
    // /\b(?:unavailable|rate[-\s]?limit(?:ed|ing)?|quota|exhausted|temporar(?:y|ily)|overloaded)\b/i
    static const std::regex transient_re(
        "\\b(?:unavailable|rate[-[:space:]]?limit(?:ed|ing)?|quota|exhausted|"
        "temporar(?:y|ily)|overloaded)\\b",
        std::regex::icase | std::regex::optimize);
    return std::regex_search(attempt.error, transient_re);
}

bool should_attempt_backend_failover(size_t backend_index,
                                     const std::vector<std::string>& candidate_backends)
{
    // 对齐 OC: backendIndex < candidateBackends.length - 1
    // 注意 size_t 无符号：candidate 为空时 size()-1 会下溢，必须先判空。
    if (candidate_backends.empty()) {
        return false;
    }
    return backend_index + 1 < candidate_backends.size();
}

} // namespace acp
} // namespace hunterclaw
