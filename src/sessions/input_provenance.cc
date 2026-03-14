#include "hunterclaw/sessions/input_provenance.h"
#include <algorithm>
#include <spdlog/spdlog.h>

namespace hunterclaw {
namespace sessions {

namespace {
    auto log() {
        static auto logger = spdlog::get("input_provenance");
        if (!logger) {
            logger = spdlog::stdout_logger_mt("input_provenance");
        }
        return logger;
    }
    
    /// Trim string
    std::string trim(const std::string& str) {
        if (str.empty()) return str;
        
        auto start = str.begin();
        while (start != str.end() && std::isspace(static_cast<unsigned char>(*start))) {
            ++start;
        }
        
        auto end = str.end();
        do {
            --end;
        } while (end > start && std::isspace(static_cast<unsigned char>(*end)));
        
        return std::string(start, end + 1);
    }
    
    /// Normalize optional string (对齐 OpenClaw normalizeOptionalString)
    std::optional<std::string> normalize_optional_string(const nlohmann::json& value) {
        if (!value.is_string()) {
            return std::nullopt;
        }
        std::string trimmed = trim(value.get<std::string>());
        return trimmed.empty() ? std::nullopt : std::make_optional(trimmed);
    }
}

// ============================================================================
// InputProvenanceKind String Conversion
// ============================================================================

std::string to_string(InputProvenanceKind kind) {
    switch (kind) {
        case InputProvenanceKind::ExternalUser:   return "external_user";
        case InputProvenanceKind::InterSession:   return "inter_session";
        case InputProvenanceKind::InternalSystem: return "internal_system";
        default: return "unknown";
    }
}

std::optional<InputProvenanceKind> provenance_kind_from_string(const std::string& str) {
    std::string lower = str;
    std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
    
    if (lower == "external_user")   return InputProvenanceKind::ExternalUser;
    if (lower == "inter_session")   return InputProvenanceKind::InterSession;
    if (lower == "internal_system") return InputProvenanceKind::InternalSystem;
    
    return std::nullopt;
}

// ============================================================================
// InputProvenance JSON Conversion
// ============================================================================

nlohmann::json InputProvenance::to_json() const {
    nlohmann::json j;
    j["kind"] = to_string(kind);
    
    if (origin_session_id) j["originSessionId"] = *origin_session_id;
    if (source_session_key) j["sourceSessionKey"] = *source_session_key;
    if (source_channel) j["sourceChannel"] = *source_channel;
    if (source_tool) j["sourceTool"] = *source_tool;
    
    return j;
}

std::optional<InputProvenance> InputProvenance::from_json(const nlohmann::json& j) {
    return normalize_input_provenance(j);
}

// ============================================================================
// Input Provenance Utilities
// ============================================================================

std::optional<InputProvenance> normalize_input_provenance(const nlohmann::json& value) {
    // 1. 验证是否为对象 (对齐 OpenClaw)
    if (!value.is_object()) {
        return std::nullopt;
    }
    
    // 2. 验证 kind 字段 (对齐 OpenClaw isInputProvenanceKind)
    if (!value.contains("kind") || !value["kind"].is_string()) {
        return std::nullopt;
    }
    
    auto kind = provenance_kind_from_string(value["kind"].get<std::string>());
    if (!kind) {
        return std::nullopt;
    }
    
    // 3. 构建 InputProvenance (对齐 OpenClaw)
    InputProvenance result(*kind);
    result.origin_session_id = normalize_optional_string(value.value("originSessionId", nlohmann::json()));
    result.source_session_key = normalize_optional_string(value.value("sourceSessionKey", nlohmann::json()));
    result.source_channel = normalize_optional_string(value.value("sourceChannel", nlohmann::json()));
    result.source_tool = normalize_optional_string(value.value("sourceTool", nlohmann::json()));
    
    return result;
}

nlohmann::json apply_input_provenance_to_user_message(
    const nlohmann::json& message,
    const std::optional<InputProvenance>& provenance
) {
    // 1. 如果没有 provenance,直接返回 (对齐 OpenClaw early return)
    if (!provenance) {
        return message;
    }
    
    // 2. 检查是否为 user message (对齐 OpenClaw)
    if (!message.contains("role") || message["role"] != "user") {
        return message;
    }
    
    // 3. 检查是否已有 provenance (对齐 OpenClaw existing check)
    if (message.contains("provenance")) {
        auto existing = normalize_input_provenance(message["provenance"]);
        if (existing) {
            // 已有有效 provenance,不覆盖
            return message;
        }
    }
    
    // 4. 添加 provenance (对齐 OpenClaw spread operator)
    nlohmann::json result = message;
    result["provenance"] = provenance->to_json();
    
    log()->trace("[InputProvenance] Applied {} to user message", to_string(provenance->kind));
    
    return result;
}

bool is_inter_session_input_provenance(const nlohmann::json& value) {
    auto provenance = normalize_input_provenance(value);
    return provenance && provenance->kind == InputProvenanceKind::InterSession;
}

bool has_inter_session_user_provenance(const nlohmann::json& message) {
    // 1. 检查是否为 user message (对齐 OpenClaw)
    if (!message.contains("role") || message["role"] != "user") {
        return false;
    }
    
    // 2. 检查 provenance (对齐 OpenClaw)
    if (!message.contains("provenance")) {
        return false;
    }
    
    return is_inter_session_input_provenance(message["provenance"]);
}

} // namespace sessions
} // namespace hunterclaw
