// 🎯 严格对齐 OpenClaw: src/config/redact-snapshot.ts
//                      + redact-snapshot.raw.ts
//                      + redact-snapshot.secret-ref.ts
// 配置快照双向脱敏系统 — 公共接口
#pragma once

#include <string>
#include <vector>
#include <nlohmann/json.hpp>

namespace hunterclaw {
namespace config {

using json = nlohmann::json;

// ============================================================================
// Constants — 对齐 OpenClaw
// ============================================================================

// 🎯 对齐 OpenClaw: REDACTED_SENTINEL = "__OPENCLAW_REDACTED__"
// 哨兵值：config.set / config.apply / config.patch 写入前检测此值并还原
static constexpr const char* REDACTED_SENTINEL = "__OPENCLAW_REDACTED__";

// ============================================================================
// ConfigSnapshot — 对齐 OpenClaw: ConfigFileSnapshot
// ============================================================================

struct ConfigSnapshot {
    bool exists = false;
    bool valid = false;
    std::string path;
    std::string hash;
    json config = json::object();
    std::string raw;
    json parsed = json(nullptr);
    json resolved = json::object();
};

// ============================================================================
// RedactionResult — 对齐 OpenClaw: RedactionResult
// ============================================================================

struct RedactionResult {
    bool ok = false;
    json result;
    std::string error;
};

// ============================================================================
// Public API
// ============================================================================

// 🎯 对齐 OpenClaw: redactConfigObject<T>(value, uiHints?)
// 脱敏普通配置对象（路径名启发式）
json redact_config_object(const json& config);

// 🎯 对齐 OpenClaw: redactConfigSnapshot(snapshot, uiHints?)
// 脱敏完整配置快照（config + raw + parsed + resolved）
ConfigSnapshot redact_config_snapshot(const ConfigSnapshot& snapshot);

// 🎯 对齐 OpenClaw: restoreRedactedValues(incoming, original, hints?)
// 将 REDACTED_SENTINEL 还原为原始凭据
RedactionResult restore_redacted_values(const json& incoming, const json& original);

}  // namespace config
}  // namespace hunterclaw
