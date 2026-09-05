#pragma once

// ============================================================================
// runtime_errors.h
// Aligned with openclaw: src/acp/runtime/errors.ts
//
// 7 error codes + AcpRuntimeError class + error boundary utility
// ============================================================================

#include <string>
#include <stdexcept>
#include <functional>
#include <optional>

namespace hunterclaw {
namespace acp {

// ── Error codes ──────────────────────────────────────────────────────────────
// Aligned with openclaw: ACP_ERROR_CODES
enum class AcpErrorCode {
    ACP_BACKEND_MISSING,
    ACP_BACKEND_UNAVAILABLE,
    ACP_BACKEND_UNSUPPORTED_CONTROL,
    ACP_DISPATCH_DISABLED,
    ACP_INVALID_RUNTIME_OPTION,
    ACP_SESSION_INIT_FAILED,
    ACP_TURN_FAILED,
};

inline std::string error_code_to_string(AcpErrorCode code) {
    switch (code) {
        case AcpErrorCode::ACP_BACKEND_MISSING:             return "ACP_BACKEND_MISSING";
        case AcpErrorCode::ACP_BACKEND_UNAVAILABLE:         return "ACP_BACKEND_UNAVAILABLE";
        case AcpErrorCode::ACP_BACKEND_UNSUPPORTED_CONTROL: return "ACP_BACKEND_UNSUPPORTED_CONTROL";
        case AcpErrorCode::ACP_DISPATCH_DISABLED:           return "ACP_DISPATCH_DISABLED";
        case AcpErrorCode::ACP_INVALID_RUNTIME_OPTION:      return "ACP_INVALID_RUNTIME_OPTION";
        case AcpErrorCode::ACP_SESSION_INIT_FAILED:         return "ACP_SESSION_INIT_FAILED";
        case AcpErrorCode::ACP_TURN_FAILED:                 return "ACP_TURN_FAILED";
    }
    return "ACP_TURN_FAILED";
}

inline AcpErrorCode error_code_from_string(const std::string& s) {
    if (s == "ACP_BACKEND_MISSING")             return AcpErrorCode::ACP_BACKEND_MISSING;
    if (s == "ACP_BACKEND_UNAVAILABLE")         return AcpErrorCode::ACP_BACKEND_UNAVAILABLE;
    if (s == "ACP_BACKEND_UNSUPPORTED_CONTROL") return AcpErrorCode::ACP_BACKEND_UNSUPPORTED_CONTROL;
    if (s == "ACP_DISPATCH_DISABLED")           return AcpErrorCode::ACP_DISPATCH_DISABLED;
    if (s == "ACP_INVALID_RUNTIME_OPTION")      return AcpErrorCode::ACP_INVALID_RUNTIME_OPTION;
    if (s == "ACP_SESSION_INIT_FAILED")         return AcpErrorCode::ACP_SESSION_INIT_FAILED;
    return AcpErrorCode::ACP_TURN_FAILED;
}

// ── AcpRuntimeError ──────────────────────────────────────────────────────────
// Aligned with openclaw: class AcpRuntimeError extends Error
class AcpRuntimeError : public std::runtime_error {
public:
    AcpErrorCode code;
    // Backend-specific structured failure code (e.g. acpx "SESSION_RESUME_REQUIRED",
    // "TURN_TIMEOUT"), preserved so recovery decisions key on the failure kind rather
    // than parsing the human-readable message.
    // Aligned with openclaw: AcpRuntimeError.detailCode
    std::optional<std::string> detail_code;

    AcpRuntimeError(AcpErrorCode code, const std::string& message)
        : std::runtime_error(message), code(code) {}

    AcpRuntimeError(AcpErrorCode code, const std::string& message,
                    const std::optional<std::string>& detail_code)
        : std::runtime_error(message), code(code), detail_code(detail_code) {}

    std::string code_string() const { return error_code_to_string(code); }
};

// ── Detail codes ─────────────────────────────────────────────────────────────
// Aligned with openclaw: ACP_TURN_TIMEOUT_DETAIL_CODE (manager.turn-timeout.ts:13)
inline constexpr const char* ACP_TURN_TIMEOUT_DETAIL_CODE = "TURN_TIMEOUT";

// ── toAcpRuntimeError ────────────────────────────────────────────────────────
// Aligned with openclaw: toAcpRuntimeError
inline AcpRuntimeError to_acp_runtime_error(
    const std::exception& e,
    AcpErrorCode fallback_code,
    const std::string& fallback_message
) {
    if (auto* acp_err = dynamic_cast<const AcpRuntimeError*>(&e)) {
        return *acp_err;
    }
    return AcpRuntimeError(fallback_code, e.what());
}

// ── withAcpRuntimeErrorBoundary ──────────────────────────────────────────────
// Aligned with openclaw: withAcpRuntimeErrorBoundary
template <typename Fn>
auto with_acp_runtime_error_boundary(
    Fn&& fn,
    AcpErrorCode fallback_code,
    const std::string& fallback_message
) -> decltype(fn()) {
    try {
        return fn();
    } catch (const AcpRuntimeError&) {
        throw;
    } catch (const std::exception& e) {
        throw AcpRuntimeError(fallback_code, e.what());
    } catch (...) {
        throw AcpRuntimeError(fallback_code, fallback_message);
    }
}

// ── formatAcpRuntimeErrorText ────────────────────────────────────────────────
// Aligned with openclaw: formatAcpRuntimeErrorText (error-text.ts)
inline std::string format_acp_runtime_error_text(const AcpRuntimeError& error) {
    std::string next;
    switch (error.code) {
        case AcpErrorCode::ACP_BACKEND_MISSING:
        case AcpErrorCode::ACP_BACKEND_UNAVAILABLE:
            next = "Run `/acp doctor`, install/enable the backend plugin, then retry.";
            break;
        case AcpErrorCode::ACP_DISPATCH_DISABLED:
            next = "Enable `acp.dispatch.enabled=true` to allow thread-message ACP turns.";
            break;
        case AcpErrorCode::ACP_SESSION_INIT_FAILED:
            next = "If this session is stale, recreate it with `/acp spawn` and rebind the thread.";
            break;
        case AcpErrorCode::ACP_INVALID_RUNTIME_OPTION:
            next = "Use `/acp status` to inspect options and pass valid values.";
            break;
        case AcpErrorCode::ACP_BACKEND_UNSUPPORTED_CONTROL:
            next = "This backend does not support that control; use a supported command.";
            break;
        case AcpErrorCode::ACP_TURN_FAILED:
            next = "Retry, or use `/acp cancel` and send the message again.";
            break;
    }
    std::string result = "ACP error (" + error.code_string() + "): " + error.what();
    if (!next.empty()) {
        result += "\nnext: " + next;
    }
    return result;
}

} // namespace acp
} // namespace hunterclaw
