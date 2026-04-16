// 严格对齐 OpenClaw: src/cron/normalize.ts (621行) +
// normalize-job-identity.ts (21行) + service/normalize.ts (70行)

#include "hunterclaw/cron/normalize.h"
#include "hunterclaw/cron/schedule.h"

#include <algorithm>
#include <cctype>
#include <regex>
#include <sstream>

namespace hunterclaw {
namespace cron {

using json = nlohmann::json;

namespace {

// ── String helpers ──────────────────────────────────────────────────────────

std::string trim(const std::string& s) {
  auto start = s.find_first_not_of(" \t\r\n");
  if (start == std::string::npos) return "";
  auto end = s.find_last_not_of(" \t\r\n");
  return s.substr(start, end - start + 1);
}

std::string to_lower(const std::string& s) {
  std::string r = s;
  std::transform(r.begin(), r.end(), r.begin(), ::tolower);
  return r;
}

std::optional<std::string> normalize_optional_string(const json& val) {
  if (val.is_null() || !val.is_string()) return std::nullopt;
  std::string s = trim(val.get<std::string>());
  return s.empty() ? std::nullopt : std::optional<std::string>(s);
}

std::optional<std::string> normalize_optional_lowercase(const json& val) {
  auto s = normalize_optional_string(val);
  if (!s) return std::nullopt;
  return to_lower(*s);
}

std::string normalize_lowercase_or_empty(const json& val) {
  auto s = normalize_optional_lowercase(val);
  return s.value_or("");
}

bool is_record(const json& val) {
  return val.is_object() && !val.is_array();
}

// ── truncateUtf16Safe ───────────────────────────────────────────────────────

std::string truncate_text(const std::string& input, int max_len) {
  if ((int)input.size() <= max_len) return input;
  std::string truncated = input.substr(0, std::max(0, max_len - 1));
  auto end = truncated.find_last_not_of(" \t\r\n");
  if (end != std::string::npos) truncated = truncated.substr(0, end + 1);
  return truncated + "\xe2\x80\xa6";  // UTF-8 ellipsis
}

// ── Coerce schedule (对齐 OpenClaw coerceSchedule) ──────────────────────────

json coerce_schedule(const json& schedule) {
  json next = schedule;

  std::string raw_kind = normalize_lowercase_or_empty(schedule.value("kind", json()));
  std::string kind;
  if (raw_kind == "at" || raw_kind == "every" || raw_kind == "cron") kind = raw_kind;

  auto expr_raw = normalize_optional_string(schedule.value("expr", json()));
  auto legacy_cron = normalize_optional_string(schedule.value("cron", json()));
  std::string normalized_expr = expr_raw.value_or(legacy_cron.value_or(""));

  // Infer kind from fields
  if (kind.empty()) {
    if (schedule.contains("atMs") || schedule.contains("at")) kind = "at";
    else if (schedule.contains("everyMs")) kind = "every";
    else if (!normalized_expr.empty()) kind = "cron";
  }

  if (!kind.empty()) next["kind"] = kind;

  // Normalize at/atMs
  if (schedule.contains("atMs") && schedule["atMs"].is_number()) {
    int64_t at_ms = schedule["atMs"].get<int64_t>();
    // Convert to ISO string representation
    next["at"] = std::to_string(at_ms);
    next.erase("atMs");
  } else if (schedule.contains("at") && schedule["at"].is_string()) {
    std::string at_str = trim(schedule["at"].get<std::string>());
    auto parsed = parse_absolute_time_ms(at_str);
    if (parsed.has_value()) {
      // Keep original string format
      next["at"] = at_str;
    }
  }
  if (next.contains("atMs")) next.erase("atMs");

  // Normalize expr
  if (!normalized_expr.empty()) {
    next["expr"] = normalized_expr;
  } else if (next.contains("expr")) {
    next.erase("expr");
  }
  if (next.contains("cron")) next.erase("cron");

  // Normalize staggerMs
  if (next.contains("staggerMs")) {
    if (next["staggerMs"].is_number()) {
      int64_t val = next["staggerMs"].get<int64_t>();
      next["staggerMs"] = std::max<int64_t>(0, val);
    } else {
      next.erase("staggerMs");
    }
  }

  // Cleanup mutually exclusive fields (对齐 OpenClaw)
  if (kind == "at") {
    next.erase("everyMs"); next.erase("anchorMs"); next.erase("expr");
    next.erase("tz"); next.erase("staggerMs");
  } else if (kind == "every") {
    next.erase("at"); next.erase("expr"); next.erase("tz"); next.erase("staggerMs");
  } else if (kind == "cron") {
    next.erase("at"); next.erase("everyMs"); next.erase("anchorMs");
  }

  return next;
}

// ── Coerce payload (对齐 OpenClaw coercePayload) ────────────────────────────

json coerce_payload(const json& payload) {
  json next = payload;

  std::string kind_raw = normalize_lowercase_or_empty(payload.value("kind", json()));
  if (kind_raw == "agentturn") next["kind"] = "agentTurn";
  else if (kind_raw == "systemevent") next["kind"] = "systemEvent";
  else if (!kind_raw.empty()) next["kind"] = kind_raw;

  // Infer kind from fields
  if (!next.contains("kind") || !next["kind"].is_string() || next["kind"].get<std::string>().empty()) {
    bool has_message = normalize_optional_string(next.value("message", json())).has_value();
    bool has_text = normalize_optional_string(next.value("text", json())).has_value();
    if (has_message) next["kind"] = "agentTurn";
    else if (has_text) next["kind"] = "systemEvent";
  }

  // Trim message/text
  if (next.contains("message") && next["message"].is_string()) {
    auto trimmed = normalize_optional_string(next["message"]);
    if (trimmed) next["message"] = *trimmed;
  }
  if (next.contains("text") && next["text"].is_string()) {
    auto trimmed = normalize_optional_string(next["text"]);
    if (trimmed) next["text"] = *trimmed;
  }

  // Cleanup model, thinking, timeoutSeconds
  auto cleanup_optional_str = [&](const std::string& field) {
    if (next.contains(field)) {
      auto val = normalize_optional_string(next[field]);
      if (val) next[field] = *val;
      else next.erase(field);
    }
  };
  cleanup_optional_str("model");
  cleanup_optional_str("thinking");

  if (next.contains("timeoutSeconds")) {
    if (next["timeoutSeconds"].is_number()) {
      next["timeoutSeconds"] = std::max(0, next["timeoutSeconds"].get<int>());
    } else {
      next.erase("timeoutSeconds");
    }
  }

  // Kind-specific cleanup (对齐 OpenClaw)
  std::string final_kind = next.value("kind", "");
  if (final_kind == "systemEvent") {
    for (auto& f : {"message", "model", "fallbacks", "thinking", "timeoutSeconds",
                     "lightContext", "allowUnsafeExternalContent", "toolsAllow"}) {
      next.erase(f);
    }
  } else if (final_kind == "agentTurn") {
    next.erase("text");
  }

  // Remove legacy delivery fields from payload (对齐 OpenClaw)
  for (auto& f : {"deliver", "channel", "to", "threadId", "bestEffortDeliver", "provider"}) {
    next.erase(f);
  }

  return next;
}

// ── Coerce delivery (对齐 OpenClaw coerceDelivery) ──────────────────────────

json coerce_delivery(const json& delivery) {
  json next = delivery;

  // Mode normalization
  if (next.contains("mode")) {
    auto mode = normalize_optional_lowercase(next["mode"]);
    if (mode.has_value()) {
      if (*mode == "deliver") next["mode"] = "announce";  // legacy compat
      else next["mode"] = *mode;
    } else {
      next.erase("mode");
    }
  }

  // Normalize string fields
  auto normalize_field = [&](const std::string& field) {
    if (next.contains(field)) {
      auto val = normalize_optional_string(next[field]);
      if (val) next[field] = *val;
      else next.erase(field);
    }
  };
  normalize_field("channel");
  normalize_field("to");
  normalize_field("accountId");

  return next;
}

// ── normalizeSessionTarget (对齐 OpenClaw) ──────────────────────────────────

std::optional<std::string> normalize_session_target(const json& raw) {
  if (!raw.is_string()) return std::nullopt;
  std::string trimmed = trim(raw.get<std::string>());
  std::string lower = to_lower(trimmed);

  if (lower == "main" || lower == "isolated" || lower == "current") return lower;

  // Support session:xxx prefix
  if (lower.rfind("session:", 0) == 0) {
    std::string session_id = trim(trimmed.substr(8));
    if (session_id.empty()) return std::nullopt;
    if (session_id.find('/') != std::string::npos ||
        session_id.find('\\') != std::string::npos ||
        session_id.find('\0') != std::string::npos) {
      return std::nullopt;
    }
    return "session:" + session_id;
  }

  return std::nullopt;
}

// ── normalizeWakeMode (对齐 OpenClaw) ───────────────────────────────────────

std::optional<std::string> normalize_wake_mode(const json& raw) {
  if (!raw.is_string()) return std::nullopt;
  auto trimmed = normalize_optional_lowercase(raw);
  if (!trimmed) return std::nullopt;
  if (*trimmed == "now" || *trimmed == "next-heartbeat") return *trimmed;
  return std::nullopt;
}

// ── inferTopLevelPayload (对齐 OpenClaw) ────────────────────────────────────

std::optional<json> infer_top_level_payload(const json& next) {
  auto message = normalize_optional_string(next.value("message", json()));
  if (message) return json{{"kind", "agentTurn"}, {"message", *message}};

  auto text = normalize_optional_string(next.value("text", json()));
  if (text) return json{{"kind", "systemEvent"}, {"text", *text}};

  return std::nullopt;
}

// ── stripLegacyTopLevelFields (对齐 OpenClaw) ──────────────────────────────

void strip_legacy_top_level_fields(json& next) {
  for (auto& f : {"model", "thinking", "timeoutSeconds", "fallbacks", "lightContext",
                   "toolsAllow", "allowUnsafeExternalContent", "message", "text",
                   "deliver", "channel", "to", "threadId", "bestEffortDeliver", "provider"}) {
    next.erase(f);
  }
}

// ── unwrapJob (对齐 OpenClaw) ───────────────────────────────────────────────

json unwrap_job(const json& raw) {
  if (raw.contains("data") && is_record(raw["data"])) return raw["data"];
  if (raw.contains("job") && is_record(raw["job"])) return raw["job"];
  return raw;
}

}  // namespace

// ============================================================================
// normalizeCronJobInput (对齐 OpenClaw normalizeCronJobInput)
// ============================================================================

std::optional<json> normalize_cron_job_input(const json& raw,
                                              const NormalizeOptions& options) {
  if (!is_record(raw)) return std::nullopt;

  json base = unwrap_job(raw);
  json next = base;

  // agentId normalization
  if (base.contains("agentId")) {
    if (base["agentId"].is_null()) {
      next["agentId"] = nullptr;
    } else if (base["agentId"].is_string()) {
      auto trimmed = trim(base["agentId"].get<std::string>());
      if (!trimmed.empty()) {
        next["agentId"] = to_lower(trimmed);
      } else {
        next.erase("agentId");
      }
    }
  }

  // sessionKey normalization
  if (base.contains("sessionKey")) {
    if (base["sessionKey"].is_null()) {
      next["sessionKey"] = nullptr;
    } else if (base["sessionKey"].is_string()) {
      auto trimmed = trim(base["sessionKey"].get<std::string>());
      if (!trimmed.empty()) next["sessionKey"] = trimmed;
      else next.erase("sessionKey");
    }
  }

  // enabled normalization
  if (base.contains("enabled")) {
    if (base["enabled"].is_boolean()) {
      next["enabled"] = base["enabled"];
    } else if (base["enabled"].is_string()) {
      auto lower = to_lower(trim(base["enabled"].get<std::string>()));
      if (lower == "true") next["enabled"] = true;
      else if (lower == "false") next["enabled"] = false;
    }
  }

  // sessionTarget normalization
  if (base.contains("sessionTarget")) {
    auto normalized = normalize_session_target(base["sessionTarget"]);
    if (normalized) next["sessionTarget"] = *normalized;
    else next.erase("sessionTarget");
  }

  // wakeMode normalization
  if (base.contains("wakeMode")) {
    auto normalized = normalize_wake_mode(base["wakeMode"]);
    if (normalized) next["wakeMode"] = *normalized;
    else next.erase("wakeMode");
  }

  // schedule coercion
  if (base.contains("schedule") && is_record(base["schedule"])) {
    next["schedule"] = coerce_schedule(base["schedule"]);
  }

  // payload inference
  if (!next.contains("payload") || !is_record(next["payload"])) {
    auto inferred = infer_top_level_payload(next);
    if (inferred) next["payload"] = *inferred;
  }

  // payload coercion
  if (base.contains("payload") && is_record(base["payload"])) {
    next["payload"] = coerce_payload(base["payload"]);
  }

  // delivery coercion
  if (base.contains("delivery") && is_record(base["delivery"])) {
    next["delivery"] = coerce_delivery(base["delivery"]);
  }

  // Remove isolation field (legacy)
  next.erase("isolation");

  // Copy top-level agentTurn fields to payload (对齐 OpenClaw copyTopLevelAgentTurnFields)
  if (next.contains("payload") && is_record(next["payload"]) &&
      next["payload"].value("kind", "") == "agentTurn") {
    auto& payload = next["payload"];
    auto copy_str = [&](const std::string& field) {
      if (!normalize_optional_string(payload.value(field, json())).has_value()) {
        auto val = normalize_optional_string(next.value(field, json()));
        if (val) payload[field] = *val;
      }
    };
    copy_str("model");
    copy_str("thinking");

    if ((!payload.contains("timeoutSeconds") || !payload["timeoutSeconds"].is_number()) &&
        next.contains("timeoutSeconds") && next["timeoutSeconds"].is_number()) {
      payload["timeoutSeconds"] = next["timeoutSeconds"];
    }
  }

  strip_legacy_top_level_fields(next);

  // Apply defaults (对齐 OpenClaw options.applyDefaults)
  if (options.apply_defaults) {
    if (!next.contains("wakeMode") || next["wakeMode"].is_null()) {
      next["wakeMode"] = "now";
    }
    if (!next.contains("enabled") || !next["enabled"].is_boolean()) {
      next["enabled"] = true;
    }

    // Name inference
    if ((!next.contains("name") || !next["name"].is_string() ||
         trim(next["name"].get<std::string>()).empty()) &&
        next.contains("schedule") && next.contains("payload")) {
      next["name"] = infer_legacy_name(next);
    } else if (next.contains("name") && next["name"].is_string()) {
      auto trimmed = trim(next["name"].get<std::string>());
      if (!trimmed.empty()) next["name"] = trimmed;
    }

    // sessionTarget defaults
    if (!next.contains("sessionTarget") && next.contains("payload") && is_record(next["payload"])) {
      std::string kind = next["payload"].value("kind", "");
      if (kind == "systemEvent") next["sessionTarget"] = "main";
      else if (kind == "agentTurn") next["sessionTarget"] = "isolated";
    }

    // Resolve "current" sessionTarget
    if (next.value("sessionTarget", "") == "current") {
      if (options.session_context && options.session_context->session_key) {
        auto sk = trim(*options.session_context->session_key);
        if (!sk.empty()) {
          next["sessionTarget"] = "session:" + sk;
        }
      }
      if (next.value("sessionTarget", "") == "current") {
        next["sessionTarget"] = "isolated";
      }
    }

    // deleteAfterRun default for at schedule
    if (next.contains("schedule") && is_record(next["schedule"]) &&
        next["schedule"].value("kind", "") == "at" &&
        !next.contains("deleteAfterRun")) {
      next["deleteAfterRun"] = true;
    }

    // Stagger defaults for cron schedule
    if (next.contains("schedule") && is_record(next["schedule"]) &&
        next["schedule"].value("kind", "") == "cron") {
      auto& sched = next["schedule"];
      if (!sched.contains("staggerMs") || !sched["staggerMs"].is_number()) {
        std::string expr = sched.value("expr", "");
        auto default_stagger = get_default_stagger_ms(expr);
        if (default_stagger) sched["staggerMs"] = *default_stagger;
      }
    }

    // Default delivery for isolated agentTurn
    std::string session_target = next.value("sessionTarget", "");
    std::string payload_kind = "";
    if (next.contains("payload") && is_record(next["payload"])) {
      payload_kind = next["payload"].value("kind", "");
    }
    bool is_isolated_like = (session_target == "isolated" || session_target == "current" ||
                             session_target.rfind("session:", 0) == 0);
    bool has_delivery = next.contains("delivery") && !next["delivery"].is_null();
    if (!has_delivery && is_isolated_like && payload_kind == "agentTurn") {
      next["delivery"] = json{{"mode", "announce"}};
    }
  }

  return next;
}

// ============================================================================
// normalizeCronJobCreate (对齐 OpenClaw normalizeCronJobCreate)
// ============================================================================

std::optional<json> normalize_cron_job_create(
    const json& raw,
    const std::optional<NormalizeOptions::SessionContext>& session_context) {
  NormalizeOptions opts;
  opts.apply_defaults = true;
  opts.session_context = session_context;
  return normalize_cron_job_input(raw, opts);
}

// ============================================================================
// normalizeCronJobPatch (对齐 OpenClaw normalizeCronJobPatch)
// ============================================================================

std::optional<json> normalize_cron_job_patch(const json& raw,
                                              const NormalizeOptions& options) {
  NormalizeOptions opts = options;
  opts.apply_defaults = false;
  return normalize_cron_job_input(raw, opts);
}

// ============================================================================
// normalizeCronJobIdentityFields (对齐 OpenClaw normalizeCronJobIdentityFields)
// ============================================================================

NormalizeIdentityResult normalize_cron_job_identity_fields(json& raw) {
  auto raw_id = normalize_optional_string(raw.value("id", json()));
  auto legacy_job_id = normalize_optional_string(raw.value("jobId", json()));
  bool had_job_id_key = raw.contains("jobId");

  std::string normalized_id = raw_id.value_or(legacy_job_id.value_or(""));
  bool id_changed = !normalized_id.empty() && raw.value("id", "") != normalized_id;

  if (id_changed) raw["id"] = normalized_id;
  if (had_job_id_key) raw.erase("jobId");

  return {id_changed || had_job_id_key, had_job_id_key};
}

// ============================================================================
// inferLegacyName (对齐 OpenClaw service/normalize.ts inferLegacyName)
// ============================================================================

std::string infer_legacy_name(const json& job) {
  std::string text;
  if (job.contains("payload") && is_record(job["payload"])) {
    auto& payload = job["payload"];
    std::string kind = payload.value("kind", "");
    if (kind == "systemEvent" && payload.contains("text") && payload["text"].is_string()) {
      text = payload["text"].get<std::string>();
    } else if (kind == "agentTurn" && payload.contains("message") && payload["message"].is_string()) {
      text = payload["message"].get<std::string>();
    }
  }

  // First line
  std::istringstream iss(text);
  std::string line;
  std::string first_line;
  while (std::getline(iss, line)) {
    auto trimmed = trim(line);
    if (!trimmed.empty()) { first_line = trimmed; break; }
  }
  if (!first_line.empty()) return truncate_text(first_line, 60);

  // Fallback to schedule info
  if (job.contains("schedule") && is_record(job["schedule"])) {
    auto& sched = job["schedule"];
    std::string kind = sched.value("kind", "");
    if (kind == "cron" && sched.contains("expr") && sched["expr"].is_string()) {
      return "Cron: " + truncate_text(sched["expr"].get<std::string>(), 52);
    }
    if (kind == "every" && sched.contains("everyMs") && sched["everyMs"].is_number()) {
      return "Every: " + std::to_string(sched["everyMs"].get<int64_t>()) + "ms";
    }
    if (kind == "at") return "One-shot";
  }

  return "Cron job";
}

// ============================================================================
// normalizeRequiredName (对齐 OpenClaw normalizeRequiredName)
// ============================================================================

std::string normalize_required_name(const json& raw) {
  if (!raw.is_string()) throw std::runtime_error("cron job name is required");
  std::string name = trim(raw.get<std::string>());
  if (name.empty()) throw std::runtime_error("cron job name is required");
  return name;
}

// ============================================================================
// normalizeOptionalAgentId (对齐 OpenClaw normalizeOptionalAgentId)
// ============================================================================

std::optional<std::string> normalize_optional_agent_id(const json& raw) {
  auto trimmed = normalize_optional_string(raw);
  if (!trimmed) return std::nullopt;
  return to_lower(*trimmed);
}

}  // namespace cron
}  // namespace hunterclaw
