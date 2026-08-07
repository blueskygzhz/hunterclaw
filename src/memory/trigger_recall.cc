#include "hunterclaw/memory/trigger_recall.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <regex>
#include <set>
#include <unordered_map>
#include <future>
#include <mutex>
#include <memory>

// 🎯 严格对齐 OpenClaw extensions/active-memory/trigger-recall.ts

namespace hunterclaw {
namespace memory {

namespace {

// 统计 UTF-8 字符串的Unicode codepoint 数（对齐 JS .length 的近似：以 codepoint 计）。
size_t codepoint_count(const std::string& s) {
    size_t n = 0;
    const unsigned char* p = reinterpret_cast<const unsigned char*>(s.data());
    const unsigned char* end = p + s.size();
    while (p < end) {
        unsigned char c = *p;
        int len = 1;
        if (c < 0x80) len = 1;
        else if ((c & 0xE0) == 0xC0) len = 2;
        else if ((c & 0xF0) == 0xE0) len = 3;
        else if ((c & 0xF8) == 0xF0) len = 4;
        p += len;
        ++n;
    }
    return n;
}

// lowercase 仅对 ASCII（对齐 JS toLowerCase 的常见场景；多字节保持原样）。
std::string ascii_lower(const std::string& s) {
    std::string out = s;
    for (char& c : out) {
        unsigned char uc = static_cast<unsigned char>(c);
        if (uc < 0x80) c = static_cast<char>(std::tolower(uc));
    }
    return out;
}

// trim 首尾空白。
std::string trim(const std::string& s) {
    size_t b = s.find_first_not_of(" \t\r\n");
    if (b == std::string::npos) return "";
    size_t e = s.find_last_not_of(" \t\r\n");
    return s.substr(b, e - b + 1);
}

// 归一化路径并大写（对齐 OC: replaceAll("\\","/").replace(/^\.\//,"").toUpperCase()）。
std::string normalize_path_upper(const std::string& path) {
    std::string p = path;
    std::replace(p.begin(), p.end(), '\\', '/');
    if (p.rfind("./", 0) == 0) p = p.substr(2);
    for (char& c : p) {
        unsigned char uc = static_cast<unsigned char>(c);
        if (uc < 0x80) c = static_cast<char>(std::toupper(uc));
    }
    return p;
}

} // namespace

// 对齐 normalizeWords: /[\p{L}\p{N}_]+/gu → lowercase → 过滤 length>1。
std::vector<std::string> normalize_words(const std::string& value) {
    std::string lowered = ascii_lower(value);
    std::vector<std::string> words;
    std::string current;
    const unsigned char* p = reinterpret_cast<const unsigned char*>(lowered.data());
    const unsigned char* end = p + lowered.size();
    while (p < end) {
        unsigned char c = *p;
        if (c < 0x80) {
            if (std::isalnum(c) || c == '_') {
                current.push_back(static_cast<char>(c));
            } else {
                if (!current.empty()) { words.push_back(current); current.clear(); }
            }
            ++p;
        } else {
            // 多字节 UTF-8：视为词字符
            int len = 1;
            if ((c & 0xE0) == 0xC0) len = 2;
            else if ((c & 0xF0) == 0xE0) len = 3;
            else if ((c & 0xF8) == 0xF0) len = 4;
            for (int i = 0; i < len && p < end; ++i, ++p)
                current.push_back(static_cast<char>(*p));
        }
    }
    if (!current.empty()) words.push_back(current);
    // 对齐 OC: .filter(word => word.length > 1)（以 codepoint 计）
    std::vector<std::string> out;
    for (auto& w : words) {
        if (codepoint_count(w) > 1) out.push_back(std::move(w));
    }
    return out;
}

// 对齐 splitTriggerPhrases: split(/[\n;|]+/) → trim → filter(Boolean)。
std::vector<std::string> split_trigger_phrases(const std::string& value) {
    std::vector<std::string> phrases;
    std::string current;
    for (char c : value) {
        if (c == '\n' || c == ';' || c == '|') {
            std::string t = trim(current);
            if (!t.empty()) phrases.push_back(t);
            current.clear();
        } else {
            current.push_back(c);
        }
    }
    std::string t = trim(current);
    if (!t.empty()) phrases.push_back(t);
    return phrases;
}

// 对齐 scoreTriggerPhrase。
double score_trigger_phrase(const std::string& message, const std::string& phrase) {
    std::vector<std::string> message_words = normalize_words(message);
    // triggerWords = [...new Set(normalizeWords(phrase))]（保持出现顺序去重）
    std::vector<std::string> trigger_words;
    {
        std::set<std::string> seen;
        for (auto& w : normalize_words(phrase)) {
            if (seen.insert(w).second) trigger_words.push_back(w);
        }
    }
    if (trigger_words.empty()) return 0.0;

    if (trigger_words.size() == 1) {
        const std::string& t = trigger_words[0];
        return (std::find(message_words.begin(), message_words.end(), t) != message_words.end())
                   ? 0.85 : 0.0;
    }

    // hasExactSequence: 存在起点 start 使 triggerWords 连续匹配 messageWords。
    bool has_exact = false;
    if (message_words.size() >= trigger_words.size()) {
        for (size_t start = 0; start + trigger_words.size() <= message_words.size(); ++start) {
            bool ok = true;
            for (size_t off = 0; off < trigger_words.size(); ++off) {
                if (message_words[start + off] != trigger_words[off]) { ok = false; break; }
            }
            if (ok) { has_exact = true; break; }
        }
    }
    if (has_exact) return 1.0;

    std::set<std::string> message_word_set(message_words.begin(), message_words.end());
    int overlap = 0;
    for (const auto& w : trigger_words) {
        if (message_word_set.count(w)) ++overlap;
    }
    if (overlap == 0) return 0.0;

    double coverage = static_cast<double>(overlap) / static_cast<double>(trigger_words.size());
    return coverage * 0.8 + std::min(1.0, overlap / 2.0) * 0.2;
}

// 对齐 scoreTriggerMatch。
double score_trigger_match(const std::string& message, const MemorySearchResult& entry) {
    if (!entry.triggers.has_value() || entry.triggers->empty()) return 0.0;
    double trigger_score = 0.0;
    for (const auto& phrase : split_trigger_phrases(*entry.triggers)) {
        trigger_score = std::max(trigger_score, score_trigger_phrase(message, phrase));
    }
    double relevance = std::max(0.0, std::min(1.0, entry.score));
    return trigger_score * 0.8 + relevance * 0.2;
}

// 对齐 isPromotedTrustedMemoryEntry。
bool is_promoted_trusted_memory_entry(
    const MemorySearchResult& entry,
    const std::vector<std::string>& active_project_keys) {
    if (entry.project_key.has_value() && !entry.project_key->empty()) {
        // storedProjectKeys = [...new Set(split(";").trim().filter(Boolean))]
        std::vector<std::string> stored;
        std::set<std::string> seen;
        {
            std::string current;
            for (char c : *entry.project_key) {
                if (c == ';') {
                    std::string t = trim(current);
                    if (!t.empty() && seen.insert(t).second) stored.push_back(t);
                    current.clear();
                } else {
                    current.push_back(c);
                }
            }
            std::string t = trim(current);
            if (!t.empty() && seen.insert(t).second) stored.push_back(t);
        }
        // 要求所有 stored key 均 active，且非空。
        if (stored.empty()) return false;
        for (const auto& k : stored) {
            if (std::find(active_project_keys.begin(), active_project_keys.end(), k) ==
                active_project_keys.end()) {
                return false;
            }
        }
    }

    if (entry.origin_class.has_value() &&
        (*entry.origin_class == "owner" || *entry.origin_class == "agent")) {
        return true;
    }

    if (entry.source != MemorySource::Memory) {
        return false;
    }

    std::string normalized = normalize_path_upper(entry.path);
    return normalized == "MEMORY.MD" || normalized == "USER.MD";
}

// 对齐 selectStrongTriggerMatches。
std::vector<TriggerRecallMatch> select_strong_trigger_matches(
    const std::string& message,
    const std::vector<MemorySearchResult>& entries,
    const std::vector<std::string>& active_project_keys) {
    std::vector<TriggerRecallMatch> matches;
    for (const auto& e : entries) {
        if (!is_promoted_trusted_memory_entry(e, active_project_keys)) continue;
        double s = score_trigger_match(message, e);
        if (s < STRONG_TRIGGER_MATCH_SCORE) continue;
        matches.push_back(TriggerRecallMatch{e, s});
    }
    // 排序：matchScore desc → path asc → startLine asc
    std::sort(matches.begin(), matches.end(),
              [](const TriggerRecallMatch& a, const TriggerRecallMatch& b) {
                  if (a.match_score != b.match_score) return a.match_score > b.match_score;
                  if (a.entry.path != b.entry.path) return a.entry.path < b.entry.path;
                  return a.entry.start_line < b.entry.start_line;
              });
    if (static_cast<int>(matches.size()) > TRIGGER_INJECTION_LIMIT) {
        matches.resize(TRIGGER_INJECTION_LIMIT);
    }
    return matches;
}

// 对齐 loadTriggerRecallCandidates 合并：Map 去重，trigger 优先，检索补充。
std::vector<MemorySearchResult> merge_trigger_candidates(
    const std::vector<MemorySearchResult>& trigger_candidates,
    const std::vector<MemorySearchResult>& retrieved) {
    std::vector<MemorySearchResult> ordered;
    std::unordered_map<std::string, size_t> index;  // key →ordered 中位置

    auto key_of = [](const MemorySearchResult& e) -> std::string {
        return to_string(e.source) + ":" + e.path + ":" +
               std::to_string(e.start_line) + ":" + std::to_string(e.end_line);
    };
    // 对齐 OC: [...triggerCandidates, ...retrieved]，同key 后者覆盖前者的value，
    //   但保持首次出现的插入顺序。
    auto upsert = [&](const MemorySearchResult& e) {
        std::string k = key_of(e);
        auto it = index.find(k);
        if (it == index.end()) {
            index[k] = ordered.size();
            ordered.push_back(e);
        } else {
            ordered[it->second] = e;  // 覆盖 value（对齐 Map.set 语义）
        }
    };
    for (const auto& e : trigger_candidates) upsert(e);
    for (const auto& e : retrieved) upsert(e);
    return ordered;
}

// 对齐 stripMemoryAnnotationCarriers。
std::string strip_memory_annotation_carriers(const std::string& text) {
    // MEMORY_ANNOTATION_CARRIER_RE = /<!--\s*(?:trigger|importance|project)\s*:[^\r\n]*?-->/giu
    static const std::regex carrier_re(
        R"(<!--\s*(?:trigger|importance|project)\s*:[^\r\n]*?-->)",
        std::regex::icase);
    if (!std::regex_search(text, carrier_re)) {
        return text;// 对齐 OC: 未剥离时原样返回
    }
    std::string without = std::regex_replace(text, carrier_re, "");
    // 清理行尾空白：/[ \t]+(?=\r?$)/gm
    static const std::regex trailing_ws_re(R"([ \t]+(?=\r?(\n|$)))");
    return std::regex_replace(without, trailing_ws_re, "");
}

namespace {
// UTF-8 安全截断到 max_chars 字节（对齐 truncateUtf16Safe 的边界安全语义）。
std::string truncate_utf8_safe(const std::string& s, size_t max_chars) {
    if (s.size() <= max_chars) return s;
    std::string result = s.substr(0, max_chars);
    while (!result.empty() && (static_cast<unsigned char>(result.back()) & 0xC0) == 0x80) {
        result.pop_back();
    }
    return result;
}
} // namespace

// 对齐 escapeXml (prompt.ts)。顺序需与 OC 一致：& 先行，避免二次转义。
std::string escape_xml(const std::string& str) {
    std::string out;
    out.reserve(str.size());
    for (char c : str) {
        switch (c) {
            case '&': out += "&amp;"; break;
            case '<': out += "&lt;"; break;
            case '>': out += "&gt;"; break;
            case '"': out += "&quot;"; break;
            case '\'': out += "&apos;"; break;
            default: out += c; break;
        }
    }
    return out;
}

// 对齐 buildMetadata + buildPromptPrefix (prompt.ts)。
std::string build_active_memory_prompt_prefix(const std::string& summary) {
    // 对齐 buildMetadata: summary 为空 → undefined
    if (summary.empty()) return "";
    static const std::string kPluginTag = "active_memory_plugin";
    static const std::string kContextHeader = "Context:";
    std::string metadata =
        "<" + kPluginTag + ">\n" + escape_xml(summary) + "\n</" + kPluginTag + ">";
    // 对齐 buildPromptPrefix: [ACTIVE_MEMORY_CONTEXT_HEADER, metadata].join("\n")
    return kContextHeader + "\n" + metadata;
}

// 对齐 buildTriggerRecallContext:摘要正文 → truncate → buildPromptPrefix 包裹。
std::string build_trigger_recall_context(const std::vector<TriggerRecallMatch>& matches) {
    if (matches.empty()) return "";
    std::string summary;
    for (size_t i = 0; i < matches.size(); ++i) {
        if (i > 0) summary += "\n";
        const auto& e = matches[i].entry;
        std::string snippet = trim(strip_memory_annotation_carriers(e.snippet));
        summary += "- " + snippet + " (Source: " + e.path + "#L" +
                   std::to_string(e.start_line) + ")";
    }
    std::string truncated = truncate_utf8_safe(summary, MAX_TRIGGER_CONTEXT_CHARS);
    return build_active_memory_prompt_prefix(truncated);
}

// 对齐 resolveTriggerRecall。
TriggerRecallResult resolve_trigger_recall(
    const std::string& message,
    const std::vector<MemorySearchResult>& trigger_candidates,
    const std::vector<MemorySearchResult>& retrieved,
    const std::vector<std::string>& active_project_keys) {
    // 对齐 OC: candidates = loadTriggerRecallCandidates(...)（合并去重）
    std::vector<MemorySearchResult> candidates =
        merge_trigger_candidates(trigger_candidates, retrieved);
    // 对齐 OC: matches = selectStrongTriggerMatches(message, candidates, activeProjectKeys)
    std::vector<TriggerRecallMatch> matches =
        select_strong_trigger_matches(message, candidates, active_project_keys);

    TriggerRecallResult result;
    result.context = build_trigger_recall_context(matches);
    result.has_strong_hit = !matches.empty();
    result.injected_count = static_cast<int>(matches.size());
    return result;
}

// ============================================================================
// Prewarm 缓存（对齐 triggerRecallPrewarms）
// ============================================================================
namespace {

using CandidateFuture = std::shared_future<std::vector<MemorySearchResult>>;

struct PrewarmEntry {
    std::string cfg_token;
    std::string agent_id;
    std::string query;
    std::vector<std::string> active_project_keys;
    CandidateFuture future;
};

std::mutex& prewarm_mutex() {
    static std::mutex m;
    return m;
}

std::unordered_map<std::string, PrewarmEntry>& prewarm_map() {
    static std::unordered_map<std::string, PrewarmEntry> m;
    return m;
}

// 对齐 OC existing 复用条件：cfg 引用相等 + agentId + query + activeProjectKeys 逐项相等。
bool same_params(const PrewarmEntry& e, const TriggerLookupKey& key) {
    if (e.cfg_token != key.cfg_token) return false;
    if (e.agent_id != key.agent_id) return false;
    if (e.query != key.query) return false;
    if (e.active_project_keys.size() != key.active_project_keys.size()) return false;
    for (size_t i = 0; i < e.active_project_keys.size(); ++i) {
        if (e.active_project_keys[i] != key.active_project_keys[i]) return false;
    }
    return true;
}

// 启动一次异步候选加载（loader 失败→返回空，对齐 OC .catch(()=>[])）。
CandidateFuture launch_loader(const CandidateLoader& loader) {
    // std::launch::async 保证真正后台执行（对齐 OC Promise 立即启动的语义）。
    return std::async(std::launch::async, [loader]() -> std::vector<MemorySearchResult> {
        try {
            return loader();
        } catch (...) {
            return {};
        }
    }).share();
}

} // namespace

// 对齐 resolveTriggerRecallCandidates。
std::vector<MemorySearchResult> resolve_trigger_recall_candidates(
    const TriggerLookupKey& key,
    const CandidateLoader& loader) {
    // 对齐 OC: const runId = params.runId?.trim(); if (!runId) return loadTriggerRecallCandidates(...)
    std::string run_id = key.run_id.value_or("");
    {
        size_t b = run_id.find_first_not_of(" \t\r\n");
        size_t e = run_id.find_last_not_of(" \t\r\n");
        run_id = (b == std::string::npos) ? "" : run_id.substr(b, e - b + 1);
    }
    if (run_id.empty()) {
        // 无 run_id：直接加载（不缓存）。同样吞掉异常，行为与缓存路径一致。
        try {
            return loader();
        } catch (...) {
            return {};
        }
    }

    CandidateFuture future;
    bool created = false;
    {
        std::lock_guard<std::mutex> lock(prewarm_mutex());
        auto& map = prewarm_map();
        auto it = map.find(run_id);
        if (it != map.end() && same_params(it->second, key)) {
            // 对齐 OC: 复用 existing.promise
            future = it->second.future;
        } else {
            // 对齐 OC: 新建 entry 并存入
            PrewarmEntry entry;
            entry.cfg_token = key.cfg_token;
            entry.agent_id = key.agent_id;
            entry.query = key.query;
            entry.active_project_keys = key.active_project_keys;
            entry.future = launch_loader(loader);
            future = entry.future;
            map[run_id] = std::move(entry);
            created = true;
        }
    }

    // 等待结果。对齐 OC: 失败清理由"创建该 entry 的调用"负责（对应 OC 在新建时
    //   挂载的 void entry.promise.catch(...)），复用调用不触发清理。
    std::vector<MemorySearchResult> candidates = future.get();
    if (created && candidates.empty()) {
        std::lock_guard<std::mutex> lock(prewarm_mutex());
        auto& map = prewarm_map();
        auto it = map.find(run_id);
        // 若缓存里仍是同参数的同一 entry，则移除（对齐 OC 的 get(runId)===entry 判定）。
        if (it != map.end() && same_params(it->second, key)) {
            map.erase(it);
        }
    }
    return candidates;
}

// 对齐 prewarmTriggerRecall。
void prewarm_trigger_recall(
    const TriggerLookupKey& key,
    const CandidateLoader& loader) {
    // 对齐 OC: await resolveTriggerRecallCandidates(params)（结果丢弃）。
    (void)resolve_trigger_recall_candidates(key, loader);
}

// 对齐 resolveTriggerRecall（含缓存）。
TriggerRecallResult resolve_trigger_recall_cached(
    const std::string& message,
    const TriggerLookupKey& key,
    const CandidateLoader& loader) {
    std::vector<MemorySearchResult> candidates =
        resolve_trigger_recall_candidates(key, loader);
    std::vector<TriggerRecallMatch> matches =
        select_strong_trigger_matches(message, candidates, key.active_project_keys);

    TriggerRecallResult result;
    result.context = build_trigger_recall_context(matches);
    result.has_strong_hit = !matches.empty();
    result.injected_count = static_cast<int>(matches.size());
    return result;
}

// 对齐 forgetTriggerRecallPrewarm。
void forget_trigger_recall_prewarm(const std::optional<std::string>& run_id) {
    if (!run_id.has_value() || run_id->empty()) return;
    std::lock_guard<std::mutex> lock(prewarm_mutex());
    prewarm_map().erase(*run_id);
}

// 对齐 resetTriggerRecallPrewarmsForTests。
void reset_trigger_recall_prewarms() {
    std::lock_guard<std::mutex> lock(prewarm_mutex());
    prewarm_map().clear();
}

} // namespace memory
} // namespace hunterclaw
