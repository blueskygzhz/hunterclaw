// ============================================================================
// loop_detection.cc
// ✅ 严格对齐 OpenClaw: src/agents/tool-loop-detection.ts
// ============================================================================

#include "hunterclaw/react/loop_detection.h"
#include <sstream>
#include <algorithm>
#include <cctype>
#include <map>
#include <set>
#include <regex>
#include <openssl/sha.h>
#include <iomanip>
#include <spdlog/spdlog.h>

namespace hunterclaw {
namespace react {

namespace {

auto log() {
    static auto logger = spdlog::get("loop-detection");
    if (!logger) {
        logger = spdlog::stdout_logger_mt("loop-detection");
    }
    return logger;
}

// ── SHA256 ────────────────────────────────────────────────────────────────────
// ✅ 对齐 OpenClaw: createHash("sha256").update(serialized).digest("hex")

std::string compute_sha256(const std::string& data) {
    unsigned char hash[SHA256_DIGEST_LENGTH];
    SHA256(reinterpret_cast<const unsigned char*>(data.c_str()), data.size(), hash);
    std::ostringstream oss;
    for (int i = 0; i < SHA256_DIGEST_LENGTH; i++) {
        oss << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(hash[i]);
    }
    return oss.str();
}

// ── stableStringify ───────────────────────────────────────────────────────────
// ✅ 对齐 OpenClaw: stableStringify — keys 按字母序排序，保证相同结构不同 key 顺序产生相同 hash

std::string stable_stringify(const nlohmann::json& j) {
    if (j.is_null())    return "null";
    if (j.is_boolean()) return j.get<bool>() ? "true" : "false";
    if (j.is_number())  return j.dump();
    if (j.is_string())  return j.dump();  // JSON quoted
    if (j.is_array()) {
        std::ostringstream oss;
        oss << "[";
        bool first = true;
        for (const auto& item : j) {
            if (!first) oss << ",";
            oss << stable_stringify(item);
            first = false;
        }
        oss << "]";
        return oss.str();
    }
    if (j.is_object()) {
        // ✅ keys 按字母序排序 (对齐 OpenClaw: Object.keys(obj).toSorted())
        std::vector<std::string> keys;
        for (auto it = j.begin(); it != j.end(); ++it) {
            keys.push_back(it.key());
        }
        std::sort(keys.begin(), keys.end());
        std::ostringstream oss;
        oss << "{";
        bool first = true;
        for (const auto& k : keys) {
            if (!first) oss << ",";
            // key 以 JSON 字符串形式输出
            oss << nlohmann::json(k).dump() << ":" << stable_stringify(j[k]);
            first = false;
        }
        oss << "}";
        return oss.str();
    }
    return "null";
}

// ✅ 对齐 OpenClaw: stableStringifyFallback — 序列化失败时的降级处理
std::string stable_stringify_fallback(const core::JsonValue& params) {
    try {
        return stable_stringify(params.to_json());
    } catch (...) {
        // 降级: 直接 dump（不保证 key 顺序，但总比崩溃好）
        try {
            return params.to_json().dump();
        } catch (...) {
            return "null";
        }
    }
}

// ✅ 对齐 OpenClaw: hashToolCall = "${toolName}:${digestStable(params)}"
std::string hash_tool_call(const std::string& tool_name, const core::JsonValue& params) {
    std::string serialized = stable_stringify_fallback(params);
    return tool_name + ":" + compute_sha256(serialized);
}

// ── hashToolOutcome ───────────────────────────────────────────────────────────
// ✅ 对齐 OpenClaw: hashToolOutcome
// 对 error、已知轮询工具(process.poll/process.log)、通用结果分别处理

std::string extract_text_content(const tools::ToolResult& result) {
    // ✅ 对齐 OpenClaw: extractTextContent — 提取 content[].text 拼接
    std::ostringstream oss;
    bool first = true;
    for (const auto& content : result.content) {
        if (content.type == tools::ToolResultContent::Type::Text && !content.text.empty()) {
            if (!first) oss << "\n";
            oss << content.text;
            first = false;
        }
    }
    std::string text = oss.str();
    // trim trailing whitespace
    while (!text.empty() && (text.back() == ' ' || text.back() == '\n' || text.back() == '\r')) {
        text.pop_back();
    }
    return text;
}

// ✅ 对齐 OpenClaw: hashToolOutcome(toolName, params, result, error)
//   返回 { resultHash, noProgress? }。error → noProgress=true；
//   write unchanged / poll unchanged 等无进展结果 → noProgress=true。
struct ToolOutcomeHash {
    std::string result_hash;
    bool no_progress = false;
};

// error 有值 → "error:" + hash(error消息)
// process.poll/process.log → 只取关键字段 hash（过滤流式输出变化）
// 其他 → hash(details + text)
ToolOutcomeHash hash_tool_outcome(
    const std::string& tool_name,
    const core::JsonValue& params,
    const tools::ToolResult& result,
    const std::string& error
) {
    // ① error 路径: hash(error 字符串)，标记 noProgress（对齐 OC:291-298）
    if (!error.empty()) {
        return ToolOutcomeHash{"error:" + compute_sha256(error), true};
    }

    // ② 通用路径
    std::string text = extract_text_content(result);
    nlohmann::json details_j;
    // 尝试从 result.details 获取
    try {
        if (!result.details.is_null()) {
            details_j = result.details.to_json();
        }
    } catch (...) {}

    // ✅ 对齐 OpenClaw: write unchanged → noProgress（对齐 OC:329-331）
    if (tool_name == "write" && details_j.is_object()) {
        std::string status = details_j.value("status", "");
        bool unchanged = (status == "unchanged") ||
                         (details_j.value("bytesWritten", -1) == 0) ||
                         details_j.value("noChange", false);
        if (unchanged) {
            return ToolOutcomeHash{compute_sha256(stable_stringify(
                nlohmann::json{{"status", "unchanged"}})), true};
        }
    }

    // ✅ 对齐 OpenClaw: process.poll / process.log 特殊处理
    // 只取状态字段，过滤掉每次不同的流式日志输出
    bool is_process_tool = (tool_name == "process");
    if (is_process_tool) {
        std::string action;
        try {
            nlohmann::json params_j = params.to_json();
            if (params_j.contains("action") && params_j["action"].is_string()) {
                action = params_j["action"].get<std::string>();
            }
        } catch (...) {}

        if (action == "poll") {
            nlohmann::json h;
            h["action"] = "poll";
            h["status"]     = details_j.contains("status")     ? details_j["status"]     : nlohmann::json(nullptr);
            h["exitCode"]   = details_j.contains("exitCode")   ? details_j["exitCode"]   : nlohmann::json(nullptr);
            h["exitSignal"] = details_j.contains("exitSignal") ? details_j["exitSignal"] : nlohmann::json(nullptr);
            h["aggregated"] = details_j.contains("aggregated") ? details_j["aggregated"] : nlohmann::json(nullptr);
            h["text"]       = text;
            // poll 仍在 running（无 exitCode）视为无进展。
            bool still_running = h["exitCode"].is_null() && h["exitSignal"].is_null();
            return ToolOutcomeHash{compute_sha256(stable_stringify(h)), still_running};
        }
        if (action == "log") {
            nlohmann::json h;
            h["action"]     = "log";
            h["status"]     = details_j.contains("status")     ? details_j["status"]     : nlohmann::json(nullptr);
            h["totalLines"] = details_j.contains("totalLines") ? details_j["totalLines"] : nlohmann::json(nullptr);
            h["totalChars"] = details_j.contains("totalChars") ? details_j["totalChars"] : nlohmann::json(nullptr);
            h["truncated"]  = details_j.contains("truncated")  ? details_j["truncated"]  : nlohmann::json(nullptr);
            h["exitCode"]   = details_j.contains("exitCode")   ? details_j["exitCode"]   : nlohmann::json(nullptr);
            h["exitSignal"] = details_j.contains("exitSignal") ? details_j["exitSignal"] : nlohmann::json(nullptr);
            h["text"]       = text;
            return ToolOutcomeHash{compute_sha256(stable_stringify(h)), false};
        }
    }

    // ③ 通用结果: hash(details + text)
    nlohmann::json generic;
    generic["details"] = details_j;
    generic["text"]    = text;
    return ToolOutcomeHash{compute_sha256(stable_stringify(generic)), false};
}

// ── getNoProgressStreak ───────────────────────────────────────────────────────
// ✅ 对齐 OpenClaw: getNoProgressStreak
// 倒序遍历 history，找到 tool_name+args_hash 匹配且有 result_hash 的条目
// 统计连续相同 result_hash 的次数（中途变化则 break）
struct NoProgressResult {
    int count = 0;
    std::string latest_result_hash;
};

NoProgressResult get_no_progress_streak(
    const std::vector<ToolCallRecord>& history,
    const std::string& tool_name,
    const std::string& args_hash
) {
    NoProgressResult r;
    for (int i = static_cast<int>(history.size()) - 1; i >= 0; i--) {
        const auto& record = history[i];
        if (record.tool_name != tool_name || record.args_hash != args_hash) {
            continue;  // ✅ 对齐 OpenClaw: continue (不是 break)，跳过不匹配的
        }
        if (record.result_hash.empty()) {
            continue;  // 还没有结果
        }
        if (r.latest_result_hash.empty()) {
            r.latest_result_hash = record.result_hash;
            r.count = 1;
            continue;
        }
        if (record.result_hash != r.latest_result_hash) {
            break;  // 结果变化了，中断
        }
        r.count++;
    }
    return r;
}

// ── getPingPongStreak ─────────────────────────────────────────────────────────
// ✅ 严格对齐 OpenClaw: getPingPongStreak
// 基于 argsHash（工具名+参数）而非工具名检测交替模式
// 只有双边结果都稳定（noProgressEvidence）才触发 Critical

struct PingPongResult {
    int count = 0;
    std::string paired_tool_name;
    std::string paired_signature;
    bool no_progress_evidence = false;
};

PingPongResult get_ping_pong_streak(
    const std::vector<ToolCallRecord>& history,
    const std::string& current_signature  // argsHash of current call
) {
    PingPongResult r;
    if (history.empty()) return r;

    const auto& last = history.back();

    // 找到上一个不同 argsHash 的条目（otherSignature）
    std::string other_signature;
    std::string other_tool_name;
    for (int i = static_cast<int>(history.size()) - 2; i >= 0; i--) {
        const auto& call = history[i];
        if (call.args_hash != last.args_hash) {
            other_signature = call.args_hash;
            other_tool_name = call.tool_name;
            break;
        }
    }

    if (other_signature.empty()) return r;

    // 统计尾部交替次数
    int alternating_tail_count = 0;
    for (int i = static_cast<int>(history.size()) - 1; i >= 0; i--) {
        const auto& call = history[i];
        // ✅ 对齐 OpenClaw: expected 取决于 alternatingTailCount % 2
        const std::string& expected = (alternating_tail_count % 2 == 0)
            ? last.args_hash : other_signature;
        if (call.args_hash != expected) break;
        alternating_tail_count++;
    }

    if (alternating_tail_count < 2) return r;

    // ✅ 对齐 OpenClaw: currentSignature 必须等于 expectedCurrentSignature (otherSignature)
    if (current_signature != other_signature) return r;

    // ── noProgressEvidence 检查 ──
    // ✅ 对齐 OpenClaw: 双边（A 侧和 B 侧）result_hash 都必须稳定才视为无进展
    int tail_start = std::max(0, static_cast<int>(history.size()) - alternating_tail_count);
    std::string first_hash_a;
    std::string first_hash_b;
    bool no_progress = true;

    for (int i = tail_start; i < static_cast<int>(history.size()); i++) {
        const auto& call = history[i];
        if (call.result_hash.empty()) {
            no_progress = false;
            break;
        }
        if (call.args_hash == last.args_hash) {
            if (first_hash_a.empty()) {
                first_hash_a = call.result_hash;
            } else if (first_hash_a != call.result_hash) {
                no_progress = false;
                break;
            }
            continue;
        }
        if (call.args_hash == other_signature) {
            if (first_hash_b.empty()) {
                first_hash_b = call.result_hash;
            } else if (first_hash_b != call.result_hash) {
                no_progress = false;
                break;
            }
            continue;
        }
        no_progress = false;
        break;
    }

    // ✅ 对齐 OpenClaw: 两侧都需要有稳定 hash 才视为 no-progress
    if (first_hash_a.empty() || first_hash_b.empty()) {
        no_progress = false;
    }

    r.count = alternating_tail_count + 1;  // ✅ 对齐 OpenClaw: +1
    r.paired_tool_name = last.tool_name;
    r.paired_signature = last.args_hash;
    r.no_progress_evidence = no_progress;
    return r;
}

// ── canonicalPairKey ──────────────────────────────────────────────────────────
// ✅ 对齐 OpenClaw: canonicalPairKey — 排序后用 | 连接，防止 A:B vs B:A 不同
std::string canonical_pair_key(const std::string& a, const std::string& b) {
    if (a <= b) return a + "|" + b;
    return b + "|" + a;
}

// ── extractUnknownToolName ────────────────────────────────────────────────────
// 🎯 对齐 OpenClaw extractUnknownToolName (tool-loop-detection.ts:145-155):
//   从error 文本解析 "unknown tool: X" / "tool X not found|is not available"，
//   命中则返回小写工具名，否则空。
std::string extract_unknown_tool_name(const std::string& error) {
    if (error.empty()) return "";
    // 两个正则（大小写不敏感），对齐 OpenClaw。
    static const std::regex re1(
        R"(unknown tool[:\s]+["']?([a-z0-9_.\-]+)["']?)",
        std::regex::icase);
    static const std::regex re2(
        R"(tool\s+["']?([a-z0-9_.\-]+)["']?\s+(?:not found|is not available))",
        std::regex::icase);
    std::smatch m;
    std::string name;
    if (std::regex_search(error, m, re1) && m.size() > 1) {
        name = m[1].str();
    } else if (std::regex_search(error, m, re2) && m.size() > 1) {
        name = m[1].str();
    }
    // to lower
    std::transform(name.begin(), name.end(), name.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    return name;
}

// ── getUnknownToolRepeatStreak ────────────────────────────────────────────────
// 🎯 对齐 OpenClaw getUnknownToolRepeatStreak (tool-loop-detection.ts:374-398):
//   从尾部倒序，统计连续同名 unknown_tool_name 的次数（遇到非匹配即break）。
struct UnknownToolStreak {
    int count = 0;
    std::string unknown_tool_name;
};

UnknownToolStreak get_unknown_tool_repeat_streak(
    const std::vector<ToolCallRecord>& history,
    const std::string& tool_name
) {
    UnknownToolStreak r;
    for (int i = static_cast<int>(history.size()) - 1; i >= 0; i--) {
        const auto& record = history[i];
        // 对齐 OC: toolName 不匹配 或 无 unknownToolName → break（非continue）。
        if (record.tool_name != tool_name || record.unknown_tool_name.empty()) {
            break;
        }
        if (r.unknown_tool_name.empty()) {
            r.unknown_tool_name = record.unknown_tool_name;
            r.count = 1;
            continue;
        }
        if (record.unknown_tool_name != r.unknown_tool_name) {
            break;
        }
        r.count++;
    }
    return r;
}

// ── getArgumentChurnNoProgressStreak ──────────────────────────────────────────
// 🎯 严格对齐 OpenClaw tool-loop-argument-churn.ts:
//   在tail（连续同 toolName 且 noProgress）中，统计各 argsHash 变体的出现次数；
//   仅当存在 >1 个"稳定变体"(count>=3)、且所有 tail 调用都属于稳定变体、
//   且所有稳定变体共享同一 resultHash、且当前 argsHash 也是稳定变体时，
//   判定为 argument churn（warning-only）。否则返回 count=0。
struct ArgumentChurnResult {
    int count = 0;
    int variant_count = 0;
};

ArgumentChurnResult get_argument_churn_no_progress_streak(
    const std::vector<ToolCallRecord>& history,
    const std::string& tool_name,
    const std::string& current_args_hash
) {
    static const int kMinStableCallsPerVariant = 3;
    // argsHash -> { result_hash, count }（保序用 vector + index map）
    struct Outcome { std::string result_hash; int count = 0; };
    std::map<std::string, Outcome> outcomes;

    for (int i = static_cast<int>(history.size()) - 1; i >= 0; i--) {
        const auto& record = history[i];
        // 对齐 OC: toolName 不匹配 → break
        if (record.tool_name != tool_name) break;
        // 对齐 OC:无 resultHash → continue
        if (record.result_hash.empty()) continue;
        // 对齐 OC: noProgress !== true → break
        if (!record.no_progress) break;
        auto it = outcomes.find(record.args_hash);
        if (it != outcomes.end() && it->second.result_hash != record.result_hash) {
            // 同一 argsHash 出现不同 resultHash → break（有进展）
            break;
        }
        if (it == outcomes.end()) {
            outcomes[record.args_hash] = Outcome{record.result_hash, 1};
        } else {
            it->second.count += 1;
        }
    }

    // 统计
    int total = 0;
    std::vector<Outcome> stable;
    for (auto& kv : outcomes) {
        total += kv.second.count;
        if (kv.second.count >= kMinStableCallsPerVariant) stable.push_back(kv.second);
    }
    // hasSharedStableOutcome: 所有稳定变体的 result_hash 只有一种
    std::set<std::string> stable_result_hashes;
    int stable_total = 0;
    for (auto& o : stable) { stable_result_hashes.insert(o.result_hash); stable_total += o.count; }
    bool has_shared_stable_outcome = (stable_result_hashes.size() == 1);
    bool has_only_stable_variants = (stable_total == total);
    int current_count = 0;
    if (auto it = outcomes.find(current_args_hash); it != outcomes.end())
        current_count = it->second.count;

    bool has_stable_churn =
        stable.size() > 1 &&
        has_only_stable_variants &&
        has_shared_stable_outcome &&
        current_count >= kMinStableCallsPerVariant;

    if (has_stable_churn) {
        return ArgumentChurnResult{total, static_cast<int>(stable.size())};
    }
    return ArgumentChurnResult{0, 0};
}

} // namespace

// ============================================================================
// LoopDetectionState 实现
// ============================================================================

std::shared_ptr<LoopDetectionState> LoopDetectionState::create(
    const LoopDetectionConfig& config
) {
    return std::shared_ptr<LoopDetectionState>(new LoopDetectionState(config));
}

LoopDetectionState::LoopDetectionState(const LoopDetectionConfig& config)
    : config_(config) {
}

// ── detect_tool_call_loop ─────────────────────────────────────────────────────
// ✅ 严格对齐 OpenClaw: detectToolCallLoop (tool-loop-detection.ts:372-494)
//
// 检测顺序 (与 OpenClaw 完全一致):
//   1. global_circuit_breaker      (noProgressStreak >= globalBreakerThreshold)
//   2. known_poll_no_progress      critical  (knownPollTool && noProgressStreak >= criticalThreshold)
//   3. known_poll_no_progress      warning   (knownPollTool && noProgressStreak >= warningThreshold)
//   4. ping_pong                   critical  (count >= criticalThreshold && noProgressEvidence)
//   5. ping_pong                   warning   (count >= warningThreshold)
//   6. generic_repeat              warning   (!knownPollTool && recentCount >= warningThreshold)
//
// 关键差异（相较旧实现）:
//   • generic_repeat 只有 Warning，无 Critical（对齐 OpenClaw）
//   • global_circuit_breaker 基于 noProgressStreak，不是 history_.size()
//   • ping_pong 基于 argsHash（非工具名），且需要 noProgressEvidence
//   • params hash 使用 stableStringify（key 字母序排序）

LoopDetectionResult LoopDetectionState::detect_tool_call_loop(
    const std::string& tool_name,
    const core::JsonValue& params
) {
    if (!config_.enabled) {
        return LoopDetectionResult{};
    }

    const std::string current_hash = hash_tool_call(tool_name, params);
    const auto no_progress = get_no_progress_streak(history_, tool_name, current_hash);
    const int no_progress_streak = no_progress.count;
    const bool known_poll_tool = loop_detection::is_known_polling_tool(tool_name, params);
    const auto ping_pong = get_ping_pong_streak(history_, current_hash);
    const auto unknown_tool = get_unknown_tool_repeat_streak(history_, tool_name);
    // 🎯 对齐 OpenClaw (tool-loop-detection.ts:526-532): argument_churn 检测 +
    //   活性信号（count >= warningThreshold 时置"argument_churn"，附加到
    //   poll/ping_pong warning）。
    const auto argument_churn = get_argument_churn_no_progress_streak(history_, tool_name, current_hash);
    const bool argument_churn_liveness =
        argument_churn.count >= config_.warning_threshold;

    // ── 0. unknown_tool_repeat ─────────────────────────────────────────────
    // 🎯 对齐 OpenClaw (tool-loop-detection.ts:534-543): 最先检查，
    //   连续调用不存在/不可用的工具达阈值 → critical 阻断。
    if (unknown_tool.count >= config_.unknown_tool_threshold) {
        const std::string& missing = unknown_tool.unknown_tool_name.empty()
            ? tool_name : unknown_tool.unknown_tool_name;
        log()->error("Unknown tool repeat: attempted unavailable tool {} {} times",
                     missing, unknown_tool.count);
        LoopDetectionResult r;
        r.stuck = true;
        r.level = LoopDetectionResult::Level::Critical;
        r.detector = LoopDetectorKind::UnknownToolRepeat;
        r.count = unknown_tool.count;
        r.message = "CRITICAL: attempted unavailable tool " + missing + " " +
                    std::to_string(unknown_tool.count) + " times. Stop retrying that "
                    "missing tool and answer without it.";
        r.warning_key = "unknown-tool:" + tool_name + ":" +
                        (unknown_tool.unknown_tool_name.empty() ? "unknown" : unknown_tool.unknown_tool_name);
        return r;
    }

    // ── 1. global circuit breaker ──────────────────────────────────────────
    // ✅ 对齐 OpenClaw: noProgressStreak >= globalCircuitBreakerThreshold
    if (no_progress_streak >= config_.global_circuit_breaker_threshold) {
        log()->error("Global circuit breaker triggered: {} repeated {} times with no progress",
                     tool_name, no_progress_streak);
        LoopDetectionResult r;
        r.stuck = true;
        r.level = LoopDetectionResult::Level::Critical;
        r.detector = LoopDetectorKind::GlobalCircuitBreaker;
        r.count = no_progress_streak;
        r.message = "CRITICAL: " + tool_name + " has repeated identical no-progress outcomes " +
                    std::to_string(no_progress_streak) + " times. Session execution blocked by "
                    "global circuit breaker to prevent runaway loops.";
        r.warning_key = "global:" + tool_name + ":" + current_hash + ":" +
                        (no_progress.latest_result_hash.empty() ? "none" : no_progress.latest_result_hash);
        return r;
    }

    // ── 2. known_poll_no_progress critical ────────────────────────────────
    // ✅ 对齐 OpenClaw: knownPollTool && detectors.knownPollNoProgress && noProgressStreak >= criticalThreshold
    if (known_poll_tool && config_.detectors.known_poll_no_progress &&
        no_progress_streak >= config_.critical_threshold) {
        log()->error("Critical polling loop detected: {} repeated {} times", tool_name, no_progress_streak);
        LoopDetectionResult r;
        r.stuck = true;
        r.level = LoopDetectionResult::Level::Critical;
        r.detector = LoopDetectorKind::KnownPollNoProgress;
        r.count = no_progress_streak;
        r.message = "CRITICAL: Called " + tool_name + " with identical arguments and no progress " +
                    std::to_string(no_progress_streak) + " times. This appears to be a stuck "
                    "polling loop. Session execution blocked to prevent resource waste.";
        r.warning_key = "poll:" + tool_name + ":" + current_hash + ":" +
                        (no_progress.latest_result_hash.empty() ? "none" : no_progress.latest_result_hash);
        return r;
    }

    // ── 3. known_poll_no_progress warning ────────────────────────────────
    if (known_poll_tool && config_.detectors.known_poll_no_progress &&
        no_progress_streak >= config_.warning_threshold) {
        log()->warn("Polling loop warning: {} repeated {} times", tool_name, no_progress_streak);
        LoopDetectionResult r;
        r.stuck = true;
        r.level = LoopDetectionResult::Level::Warning;
        r.detector = LoopDetectorKind::KnownPollNoProgress;
        r.count = no_progress_streak;
        r.message = "WARNING: You have called " + tool_name + " " +
                    std::to_string(no_progress_streak) + " times with identical arguments and no "
                    "progress. Stop polling and either (1) increase wait time between checks, or "
                    "(2) report the task as failed if the process is stuck.";
        r.warning_key = "poll:" + tool_name + ":" + current_hash + ":" +
                        (no_progress.latest_result_hash.empty() ? "none" : no_progress.latest_result_hash);
        //🎯 对齐 OC:588: ...(argumentChurnLivenessSignal ? { livenessSignal } : {})
        if (argument_churn_liveness) r.liveness_signal = "argument_churn";
        return r;
    }

    // ── 4. ping_pong critical ─────────────────────────────────────────────
    // ✅ 对齐 OpenClaw: pingPong.count >= criticalThreshold && pingPong.noProgressEvidence
    const std::string ping_pong_warning_key = !ping_pong.paired_signature.empty()
        ? "pingpong:" + canonical_pair_key(current_hash, ping_pong.paired_signature)
        : "pingpong:" + tool_name + ":" + current_hash;

    if (config_.detectors.ping_pong &&
        ping_pong.count >= config_.critical_threshold &&
        ping_pong.no_progress_evidence) {
        log()->error("Critical ping-pong loop detected: alternating calls count={} currentTool={}",
                     ping_pong.count, tool_name);
        LoopDetectionResult r;
        r.stuck = true;
        r.level = LoopDetectionResult::Level::Critical;
        r.detector = LoopDetectorKind::PingPong;
        r.count = ping_pong.count;
        r.message = "CRITICAL: You are alternating between repeated tool-call patterns (" +
                    std::to_string(ping_pong.count) + " consecutive calls) with no progress. "
                    "This appears to be a stuck ping-pong loop. Session execution blocked to "
                    "prevent resource waste.";
        r.paired_tool_name = ping_pong.paired_tool_name;
        r.warning_key = ping_pong_warning_key;
        return r;
    }

    // ── 5. ping_pong warning ──────────────────────────────────────────────
    if (config_.detectors.ping_pong && ping_pong.count >= config_.warning_threshold) {
        log()->warn("Ping-pong loop warning: alternating calls count={} currentTool={}",
                    ping_pong.count, tool_name);
        LoopDetectionResult r;
        r.stuck = true;
        r.level = LoopDetectionResult::Level::Warning;
        r.detector = LoopDetectorKind::PingPong;
        r.count = ping_pong.count;
        r.message = "WARNING: You are alternating between repeated tool-call patterns (" +
                    std::to_string(ping_pong.count) + " consecutive calls). This looks like a "
                    "ping-pong loop; stop retrying and report the task as failed.";
        r.paired_tool_name = ping_pong.paired_tool_name;
        r.warning_key = ping_pong_warning_key;
        // 🎯 对齐 OC:627: ...(argumentChurnLivenessSignal ? { livenessSignal } : {})
        if (argument_churn_liveness) r.liveness_signal = "argument_churn";
        return r;
    }

    // ── 6. generic_repeat ─────────────────────────────────────────────────
    // 🎯 对齐 OpenClaw (tool-loop-detection.ts:636-671):
    //   先 critical（noProgressStreak >= critical → 阻断 runaway loop），
    //   再 warning（recentCount >= warning）。此前 HunterClaw 缺 critical。
    if (!known_poll_tool && config_.detectors.generic_repeat &&
        no_progress_streak >= config_.critical_threshold) {
        log()->error("Critical generic loop detected: {} repeated {} times with no progress",
                     tool_name, no_progress_streak);
        LoopDetectionResult r;
        r.stuck = true;
        r.level = LoopDetectionResult::Level::Critical;
        r.detector = LoopDetectorKind::GenericRepeat;
        r.count = no_progress_streak;
        r.message = "CRITICAL: Called " + tool_name + " with identical outcomes " +
                    std::to_string(no_progress_streak) + " times. Session execution "
                    "blocked to prevent runaway loops.";
        r.warning_key = "generic:" + tool_name + ":" + current_hash + ":" +
                        (no_progress.latest_result_hash.empty() ? "none" : no_progress.latest_result_hash);
        return r;
    }

    // 🎯 对齐 OpenClaw (tool-loop-detection.ts:652-655): argument_churn warning
    //   位于 generic critical 之后、generic warning 之前。warning-only，
    //   本工具调用仍被允许（不阻断），仅作为"停滞活性"信号。
    if (argument_churn.count >= config_.warning_threshold) {
        log()->warn("Argument churn warning: {} cycled through {} stable argument patterns",
                    tool_name, argument_churn.variant_count);
        LoopDetectionResult r;
        r.stuck = true;
        r.level = LoopDetectionResult::Level::Warning;
        r.detector = LoopDetectorKind::ArgumentChurn;
        r.count = argument_churn.count;
        r.message = "WARNING: " + tool_name + " has cycled through " +
                    std::to_string(argument_churn.variant_count) + " repeated argument patterns "
                    "with the same stable outcome " + std::to_string(argument_churn.count) +
                    " times. Continued churn is treated as stalled run activity, but this tool "
                    "call remains allowed.";
        r.warning_key = "argument-churn:" + tool_name;
        r.liveness_signal = "argument_churn";
        return r;
    }

    if (!known_poll_tool && config_.detectors.generic_repeat) {
        int recent_count = 0;
        for (const auto& record : history_) {
            if (record.tool_name == tool_name && record.args_hash == current_hash) {
                recent_count++;
            }
        }
        if (recent_count >= config_.warning_threshold) {
            log()->warn("Loop warning: {} called {} times with identical arguments",
                        tool_name, recent_count);
            LoopDetectionResult r;
            r.stuck = true;
            r.level = LoopDetectionResult::Level::Warning;
            r.detector = LoopDetectorKind::GenericRepeat;
            r.count = recent_count;
            r.message = "WARNING: You have called " + tool_name + " " +
                        std::to_string(recent_count) + " times with identical arguments. "
                        "If this is not making progress, stop retrying and report the task as failed.";
            r.warning_key = "generic:" + tool_name + ":" + current_hash;
            return r;
        }
    }

    return LoopDetectionResult{};
}

// ── record_tool_call ──────────────────────────────────────────────────────────
// ✅ 对齐 OpenClaw: recordToolCall (tool-loop-detection.ts:501-523)
// 维护滑动窗口，记录 argsHash（工具名+参数 hash 的组合 hash）

void LoopDetectionState::record_tool_call(
    const std::string& tool_name,
    const core::JsonValue& params,
    const std::string& tool_call_id
) {
    ToolCallRecord record;
    record.tool_name    = tool_name;
    record.args_hash    = hash_tool_call(tool_name, params);  // ✅ toolName:SHA256(stableStringify(params))
    record.tool_call_id = tool_call_id;
    record.timestamp_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()
    ).count();
    // result_hash 留空，由 record_tool_call_outcome 回填

    history_.push_back(record);

    // ✅ 对齐 OpenClaw: 维护滑动窗口 history_size
    if (static_cast<int>(history_.size()) > config_.history_size) {
        history_.erase(history_.begin());
    }
}

// ── record_tool_call_outcome ──────────────────────────────────────────────────
// ✅ 对齐 OpenClaw: recordToolCallOutcome (tool-loop-detection.ts:528-588)
// 回填 result_hash 到最近匹配的未回填记录
// 如果没找到匹配记录，追加一条新记录

void LoopDetectionState::record_tool_call_outcome(
    const std::string& tool_name,
    const tools::ToolResult& result,
    const std::string& error,
    const std::string& tool_call_id,
    const core::JsonValue& params
) {
    // ✅ 对齐 OpenClaw: hashToolOutcome(toolName, toolParams, result, error)
    ToolOutcomeHash outcome = hash_tool_outcome(tool_name, params, result, error);
    if (outcome.result_hash.empty()) return;

    // 🎯 对齐 OpenClaw: error 时解析 unknownToolName，供 unknown_tool_repeat 检测。
    std::string unknown_tool_name = error.empty() ? "" : extract_unknown_tool_name(error);

    const std::string args_hash = hash_tool_call(tool_name, params);
    bool matched = false;

    // ✅ 对齐 OpenClaw: 倒序查找匹配且未回填 result_hash 的记录
    for (int i = static_cast<int>(history_.size()) - 1; i >= 0; i--) {
        auto& call = history_[i];
        // ✅ 对齐 OpenClaw: toolCallId 优先匹配
        if (!tool_call_id.empty() && call.tool_call_id != tool_call_id) {
            continue;
        }
        if (call.tool_name != tool_name || call.args_hash != args_hash) {
            continue;
        }
        if (!call.result_hash.empty()) {
            continue;  // 已经有 result_hash，跳过
        }
        call.result_hash = outcome.result_hash;
        call.no_progress = outcome.no_progress;  // 🎯 对齐 OC:755-759
        call.unknown_tool_name = unknown_tool_name;
        matched = true;
        break;
    }

    // ✅ 对齐 OpenClaw: 没找到匹配记录时，追加一条新记录
    if (!matched) {
        ToolCallRecord record;
        record.tool_name    = tool_name;
        record.args_hash    = args_hash;
        record.tool_call_id = tool_call_id;
        record.result_hash  = outcome.result_hash;
        record.no_progress  = outcome.no_progress;  // 🎯 对齐 OC:774
        record.unknown_tool_name = unknown_tool_name;
        record.timestamp_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()
        ).count();
        history_.push_back(record);
    }

    // ✅ 对齐 OpenClaw: splice 维护滑动窗口（outcome 也受 history_size 限制）
    if (static_cast<int>(history_.size()) > config_.history_size) {
        int excess = static_cast<int>(history_.size()) - config_.history_size;
        history_.erase(history_.begin(), history_.begin() + excess);
    }
}

// ── 向后兼容的旧签名 ──────────────────────────────────────────────────────────
void LoopDetectionState::record_tool_call_outcome(
    const std::string& tool_name,
    const tools::ToolResult& result,
    const std::string& error
) {
    // 无 tool_call_id 和 params，用空参数调新签名
    record_tool_call_outcome(tool_name, result, error, "", core::JsonValue{});
}

void LoopDetectionState::reset() {
    history_.clear();
}

std::vector<std::string> LoopDetectionState::recent_tool_names(int count) const {
    std::vector<std::string> result;
    int start = std::max(0, static_cast<int>(history_.size()) - count);
    for (size_t i = start; i < history_.size(); i++) {
        result.push_back(history_[i].tool_name);
    }
    return result;
}

// ============================================================================
// 辅助函数
// ============================================================================

namespace loop_detection {

std::string create_loop_summary(
    const LoopDetectionResult& result,
    const std::vector<ToolCallRecord>& recent_calls
) {
    std::ostringstream oss;
    oss << "Loop detected (" << result.count << " occurrences):\n";
    oss << "Detector: ";
    switch (result.detector) {
        case LoopDetectorKind::GenericRepeat:       oss << "generic_repeat\n"; break;
        case LoopDetectorKind::KnownPollNoProgress: oss << "known_poll_no_progress\n"; break;
        case LoopDetectorKind::GlobalCircuitBreaker:oss << "global_circuit_breaker\n"; break;
        case LoopDetectorKind::UnknownToolRepeat:   oss << "unknown_tool_repeat\n"; break;
        case LoopDetectorKind::ArgumentChurn:       oss << "argument_churn\n"; break;
        case LoopDetectorKind::PingPong:
            oss << "ping_pong\n";
            if (!result.paired_tool_name.empty()) {
                oss << "Paired tool: " << result.paired_tool_name << "\n";
            }
            break;
    }
    oss << "\nRecent tool calls:\n";
    int n = std::min(10, static_cast<int>(recent_calls.size()));
    for (int i = static_cast<int>(recent_calls.size()) - n;
         i < static_cast<int>(recent_calls.size()); i++) {
        oss << "  " << (i + 1) << ". " << recent_calls[i].tool_name;
        if (!recent_calls[i].result_hash.empty()) {
            oss << " (result_hash=" << recent_calls[i].result_hash.substr(0, 8) << "...)";
        }
        oss << "\n";
    }
    return oss.str();
}

// ✅ 严格对齐 OpenClaw: isKnownPollToolCall(toolName, params)
// OpenClaw 只识别:
//   - "command_status" (任意参数)
//   - "process" + params.action == "poll" 或 "log"
// HunterClaw 额外支持 execute_command 等（作为扩展保留）
bool is_known_polling_tool(const std::string& tool_name, const core::JsonValue& params) {
    // ✅ 对齐 OpenClaw: command_status 总是轮询工具
    if (tool_name == "command_status") {
        return true;
    }
    // ✅ 对齐 OpenClaw: process.poll / process.log
    if (tool_name == "process") {
        try {
            nlohmann::json params_j = params.to_json();
            if (params_j.contains("action") && params_j["action"].is_string()) {
                std::string action = params_j["action"].get<std::string>();
                return action == "poll" || action == "log";
            }
        } catch (...) {}
        return false;
    }
    return false;
}

// 向后兼容的无参数版本（只用工具名判断，对 execute_command 等返回 false）
bool is_known_polling_tool(const std::string& tool_name) {
    // 只保留 OpenClaw 原有的两个，其余返回 false
    return tool_name == "command_status";
}

} // namespace loop_detection

} // namespace react
} // namespace hunterclaw
