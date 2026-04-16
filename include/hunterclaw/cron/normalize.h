#pragma once

// 严格对齐 OpenClaw: src/cron/normalize.ts (621行)
// 核心输入标准化模块: 将原始 cron job 输入归一化为规范格式

#include "hunterclaw/cron/service.h"
#include <nlohmann/json.hpp>
#include <optional>
#include <string>

namespace hunterclaw {
namespace cron {

using json = nlohmann::json;

/**
 * NormalizeOptions (对齐 OpenClaw NormalizeOptions)
 */
struct NormalizeOptions {
  bool apply_defaults = false;
  struct SessionContext {
    std::optional<std::string> session_key;
  };
  std::optional<SessionContext> session_context;
};

/**
 * 标准化 cron job 输入
 * 对齐 OpenClaw: normalizeCronJobInput(raw, options)
 *
 * Handles:
 * - Unwrap data/job envelope
 * - Coerce schedule (at/every/cron kind inference, atMs→at, legacy cron field, stagger)
 * - Coerce payload (kind inference, field cleanup, agentTurn/systemEvent split)
 * - Coerce delivery (mode normalization, field cleanup)
 * - sessionTarget normalization (main/isolated/current/session:xxx)
 * - wakeMode normalization (now/next-heartbeat)
 * - Top-level field hoisting (model/thinking/timeoutSeconds → payload)
 * - Default application (enabled, name inference, sessionTarget, delivery, stagger)
 *
 * @param raw  原始 JSON 输入
 * @param options  标准化选项
 * @return 标准化后的 JSON，失败返回 nullopt
 */
std::optional<json> normalize_cron_job_input(const json& raw,
                                              const NormalizeOptions& options = {});

/**
 * 标准化 cron job 创建输入 (apply_defaults=true)
 * 对齐 OpenClaw: normalizeCronJobCreate(raw, options)
 */
std::optional<json> normalize_cron_job_create(
    const json& raw,
    const std::optional<NormalizeOptions::SessionContext>& session_context = std::nullopt);

/**
 * 标准化 cron job 补丁输入 (apply_defaults=false)
 * 对齐 OpenClaw: normalizeCronJobPatch(raw, options)
 */
std::optional<json> normalize_cron_job_patch(const json& raw,
                                              const NormalizeOptions& options = {});

/**
 * 标准化 job identity 字段 (legacy jobId → id)
 * 对齐 OpenClaw: normalizeCronJobIdentityFields(raw)
 */
struct NormalizeIdentityResult {
  bool mutated = false;
  bool legacy_job_id_issue = false;
};
NormalizeIdentityResult normalize_cron_job_identity_fields(json& raw);

/**
 * 推断 legacy job 名称
 * 对齐 OpenClaw: inferLegacyName(job)
 */
std::string infer_legacy_name(const json& job);

/**
 * 标准化必填名称
 * 对齐 OpenClaw: normalizeRequiredName(raw)
 */
std::string normalize_required_name(const json& raw);

/**
 * 标准化可选 agent ID
 * 对齐 OpenClaw: normalizeOptionalAgentId(raw)
 */
std::optional<std::string> normalize_optional_agent_id(const json& raw);

}  // namespace cron
}  // namespace hunterclaw
