#pragma once

// ============================================================================
// runtime_types.h
// Aligned with openclaw: src/acp/runtime/types.ts
//
// Defines the AcpRuntime interface contract that all ACP backends must implement.
// ============================================================================

#include <string>
#include <vector>
#include <optional>
#include <functional>
#include <memory>
#include <map>
#include <variant>

namespace hunterclaw {
namespace acp {

// ── Prompt mode ──────────────────────────────────────────────────────────────
// Aligned with openclaw: AcpRuntimePromptMode = "prompt" | "steer"
enum class AcpRuntimePromptMode {
    Prompt,
    Steer,
};

// ── Session mode ─────────────────────────────────────────────────────────────
// Aligned with openclaw: AcpRuntimeSessionMode = "persistent" | "oneshot"
enum class AcpRuntimeSessionMode {
    Persistent,
    Oneshot,
};

inline std::string session_mode_to_string(AcpRuntimeSessionMode mode) {
    return mode == AcpRuntimeSessionMode::Persistent ? "persistent" : "oneshot";
}

inline AcpRuntimeSessionMode session_mode_from_string(const std::string& s) {
    return s == "oneshot" ? AcpRuntimeSessionMode::Oneshot : AcpRuntimeSessionMode::Persistent;
}

// ── Runtime control ──────────────────────────────────────────────────────────
// Aligned with openclaw: AcpRuntimeControl
enum class AcpRuntimeControl {
    SessionSetMode,
    SessionSetConfigOption,
    SessionStatus,
};

// ── Runtime handle ───────────────────────────────────────────────────────────
// Aligned with openclaw: AcpRuntimeHandle
struct AcpRuntimeHandle {
    std::string session_key;
    std::string backend;
    std::string runtime_session_name;
    std::optional<std::string> cwd;
    std::optional<std::string> acpx_record_id;
    std::optional<std::string> backend_session_id;
    std::optional<std::string> agent_session_id;
};

// ── Ensure input ─────────────────────────────────────────────────────────────
// Aligned with openclaw: AcpRuntimeEnsureInput
struct AcpRuntimeEnsureInput {
    std::string session_key;
    std::string agent;
    AcpRuntimeSessionMode mode = AcpRuntimeSessionMode::Persistent;
    std::optional<std::string> resume_session_id;
    std::optional<std::string> cwd;
    std::map<std::string, std::string> env;
};

// ── Turn attachment ──────────────────────────────────────────────────────────
// Aligned with openclaw: AcpRuntimeTurnAttachment
struct AcpRuntimeTurnAttachment {
    std::string media_type;
    std::string data;
};

// ── Turn input ───────────────────────────────────────────────────────────────
// Aligned with openclaw: AcpRuntimeTurnInput
struct AcpRuntimeTurnInput {
    AcpRuntimeHandle handle;
    std::string text;
    std::vector<AcpRuntimeTurnAttachment> attachments;
    AcpRuntimePromptMode mode = AcpRuntimePromptMode::Prompt;
    std::string request_id;
    // signal: handled via cancelled_ atomic in C++
};

// ── Capabilities ─────────────────────────────────────────────────────────────
// Aligned with openclaw: AcpRuntimeCapabilities
struct AcpRuntimeCapabilities {
    std::vector<AcpRuntimeControl> controls;
    std::vector<std::string> config_option_keys;
};

// ── Runtime status ───────────────────────────────────────────────────────────
// Aligned with openclaw: AcpRuntimeStatus
struct AcpRuntimeStatus {
    std::optional<std::string> summary;
    std::optional<std::string> acpx_record_id;
    std::optional<std::string> backend_session_id;
    std::optional<std::string> agent_session_id;
    std::map<std::string, std::string> details;
};

// ── Doctor report ────────────────────────────────────────────────────────────
// Aligned with openclaw: AcpRuntimeDoctorReport
struct AcpRuntimeDoctorReport {
    bool ok = false;
    std::optional<std::string> code;
    std::string message;
    std::optional<std::string> install_command;
    std::vector<std::string> details;
};

// ── Session update tag ───────────────────────────────────────────────────────
// Aligned with openclaw: AcpSessionUpdateTag
using AcpSessionUpdateTag = std::string;

// ── Runtime event ────────────────────────────────────────────────────────────
// Aligned with openclaw: AcpRuntimeEvent (union type)
struct AcpRuntimeEvent {
    enum class Type {
        TextDelta,
        Status,
        ToolCall,
        Done,
        Error,
    };

    Type type;
    std::string text;
    std::optional<std::string> stream;     // "output" | "thought"
    std::optional<AcpSessionUpdateTag> tag;
    std::optional<std::string> tool_call_id;
    std::optional<std::string> status;     // tool status
    std::optional<std::string> title;      // tool title
    std::optional<std::string> stop_reason;
    std::optional<std::string> message;    // error message
    std::optional<std::string> code;       // error code
    // Backend-specific structured failure code on error events.
    // Aligned with openclaw: AcpRuntimeEvent["error"].detailCode
    // 超时判定（TURN_TIMEOUT）与 resume 重试（SESSION_RESUME_REQUIRED）都靠它，
    // 不能只靠解析 message 文本。
    std::optional<std::string> detail_code;
    std::optional<bool> retryable;
    std::optional<int> used;               // usage
    std::optional<int> size;               // usage
};

// ── Event callback ───────────────────────────────────────────────────────────
using AcpRuntimeEventCallback = std::function<void(const AcpRuntimeEvent&)>;

// ── AcpRuntime interface ─────────────────────────────────────────────────────
// Aligned with openclaw: interface AcpRuntime (runtime/types.ts)
class AcpRuntime {
public:
    virtual ~AcpRuntime() = default;

    // Required methods
    virtual AcpRuntimeHandle ensure_session(const AcpRuntimeEnsureInput& input) = 0;

    // runTurn streams events via callback (C++ equivalent of AsyncIterable)
    virtual void run_turn(const AcpRuntimeTurnInput& input,
                          AcpRuntimeEventCallback on_event) = 0;

    virtual void cancel(const AcpRuntimeHandle& handle,
                        const std::string& reason = "") = 0;

    virtual void close(const AcpRuntimeHandle& handle,
                       const std::string& reason) = 0;

    // Optional methods (return false/empty if not supported)
    virtual bool has_get_capabilities() const { return false; }
    virtual AcpRuntimeCapabilities get_capabilities(const AcpRuntimeHandle& handle) {
        return {};
    }

    virtual bool has_get_status() const { return false; }
    virtual AcpRuntimeStatus get_status(const AcpRuntimeHandle& handle) {
        return {};
    }

    virtual bool has_set_mode() const { return false; }
    virtual void set_mode(const AcpRuntimeHandle& handle, const std::string& mode) {}

    virtual bool has_set_config_option() const { return false; }
    virtual void set_config_option(const AcpRuntimeHandle& handle,
                                   const std::string& key,
                                   const std::string& value) {}

    virtual bool has_doctor() const { return false; }
    virtual AcpRuntimeDoctorReport doctor() { return {}; }
};

} // namespace acp
} // namespace hunterclaw
