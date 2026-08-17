#include "hunterclaw/memory/short_term_promotion.h"
#include "hunterclaw/memory/concept_vocabulary.h"
#include <filesystem>
#include <fstream>
#include <sstream>
#include <chrono>
#include <cmath>
#include <algorithm>
#include <regex>
#include <set>
#include <cstdint>
#include <cstdlib>
#include <cerrno>
#include <thread>
#include <stdexcept>
#include <csignal>
#include <unistd.h>
#include <fcntl.h>
#include <openssl/sha.h>
#include <iomanip>
#include <nlohmann/json.hpp>

// 严格对齐 OpenClaw short-term-promotion.ts (1879 行)

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace hunterclaw {
namespace memory {

// Forward declarations（供 read_store / write_store 等前置调用）
static std::string truncate_short_term_snippet(const std::string& snippet);
static int64_t parse_iso_ms(const std::string& iso);

const PromotionWeights DEFAULT_PROMOTION_WEIGHTS = {
    0.24, 0.30, 0.15, 0.15, 0.10, 0.06
};

// 対齐 short-term-promotion-utils.ts:13-17 —— 支持子目录与日期后缀
static const std::regex SHORT_TERM_PATH_RE(
    R"((?:^|\/)memory\/(?:[^/]+\/)*(\d{4})-(\d{2})-(\d{2})(?:-[^/]+)?\.md$)");
static const std::regex DREAMING_MEMORY_PATH_RE(
    R"((?:^|\/)memory\/dreaming\/)");
static const std::regex SHORT_TERM_BASENAME_RE(
    R"(^(\d{4})-(\d{2})-(\d{2})(?:-[^/]+)?\.md$)");
static const std::regex SHORT_TERM_SESSION_CORPUS_RE(
    R"((?:^|\/)memory\/\.dreams\/session-corpus\/(\d{4})-(\d{2})-(\d{2})\.(?:md|txt)$)");
static const int64_t DAY_MS = 24LL * 60 * 60 * 1000;

// ============================================================================
// isContaminatedDreamingSnippet 正则集 (対齐 short-term-promotion-utils.ts:22-32)
// ============================================================================
static const std::regex DREAMING_TRANSCRIPT_PROMPT_LINE_RE(
    R"(\[[^\]]*dreaming-narrative[^\]]*]\s*(?:User|Assistant):\s*Write a dream diary entry from these memory fragments:?)",
    std::regex::icase);
static const std::regex RAW_SESSION_METADATA_RE(
    R"(\bSession Key\b.{0,260}\bSession ID\b|\bSession ID\b.{0,260}\bSession Key\b)",
    std::regex::icase);
static const std::regex RAW_CONVERSATION_SUMMARY_RE(
    R"(^(?:[-*+]\s*)?Conversation Summary:)", std::regex::icase);
static const std::regex RAW_TRANSCRIPT_TURN_RE(
    R"(^(?:[-*+]\s*)?(?:user|assistant):\s)", std::regex::icase);
static const std::regex MEMORY_FLUSH_PROMPT_RE(
    R"(Save important context from this session to the daily memory file\.\s*STRICT RULES:)",
    std::regex::icase);
static const std::regex PROMOTION_SCORE_METADATA_RE(
    R"(\[\s*score=\d+(?:\.\d+)?\s+(?:signals=\d+\s+)?recalls=\d+\s+avg=\d+(?:\.\d+)?\s+source=memory\/)",
    std::regex::icase);
static const std::regex OPENCLAW_PROMOTION_MARKER_RE(
    R"(<!--\s*openclaw-memory-promotion:)", std::regex::icase);
static const std::regex DREAMING_DIFF_PREFIX_RE(
    R"(@@\s*-\d+(?:,\d+)?\s+[-*+]\s+)", std::regex::icase);

// ============================================================================
// Helper Functions (対齐 short-term-promotion.ts:209-326)
// ============================================================================

static double clamp_score(double value) {
    if (!std::isfinite(value)) return 0.0;
    return std::max(0.0, std::min(1.0, value));
}

static std::string sha1_hex(const std::string& input) {
    unsigned char digest[20];
    SHA1(reinterpret_cast<const unsigned char*>(input.data()), input.size(), digest);
    std::ostringstream oss;
    for (int i = 0; i < 12; ++i)  // slice(0, 12) → 12 hex chars = 6 bytes
        oss << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(digest[i]);
    return oss.str().substr(0, 12);
}

static std::string normalize_snippet(const std::string& raw) {
    std::string trimmed = raw;
    auto s = trimmed.find_first_not_of(" \t\n\r");
    if (s == std::string::npos) return "";
    auto e = trimmed.find_last_not_of(" \t\n\r");
    trimmed = trimmed.substr(s, e - s + 1);
    // Collapse whitespace
    return std::regex_replace(trimmed, std::regex("\\s+"), " ");
}

static std::string normalize_memory_path(const std::string& raw) {
    std::string result = raw;
    std::replace(result.begin(), result.end(), '\\', '/');
    // Remove leading ./
    if (result.size() >= 2 && result[0] == '.' && result[1] == '/') {
        result = result.substr(2);
    }
    return result;
}

static std::string build_claim_hash(const std::string& snippet) {
    return sha1_hex(normalize_snippet(snippet));
}

static std::string build_entry_key(
    const std::string& source,
    const std::string& path,
    int start_line, int end_line,
    const std::string& claim_hash = ""
) {
    std::string base = source + ":" + normalize_memory_path(path) + ":"
        + std::to_string(start_line) + ":" + std::to_string(end_line);
    return claim_hash.empty() ? base : (base + ":" + claim_hash);
}

static std::string hash_query(const std::string& query) {
    return sha1_hex(normalize_lowercase(query));
}

static std::string normalize_iso_day(const std::string& iso_like) {
    std::regex re(R"(^(\d{4}-\d{2}-\d{2}))");
    std::smatch m;
    if (std::regex_search(iso_like, m, re)) return m[1].str();
    return "";
}

static int total_signal_count(int recall, int daily, int grounded) {
    return std::max(0, recall) + std::max(0, daily) + std::max(0, grounded);
}

// ============================================================================
// Date helpers
// ============================================================================

std::string format_memory_dreaming_day(int64_t epoch_ms, const std::string& /*timezone*/) {
    if (epoch_ms <= 0) {
        epoch_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
    }
    time_t t = epoch_ms / 1000;
    struct tm tm;
    gmtime_r(&t, &tm);
    char buf[16];
    strftime(buf, sizeof(buf), "%Y-%m-%d", &tm);
    return std::string(buf);
}

// ============================================================================
// Path helpers (対齐 short-term-promotion.ts:550-564)
// ============================================================================

static const std::string SHORT_TERM_STORE_REL = "memory/.dreams/short-term-recall.json";
static const std::string SHORT_TERM_PHASE_SIGNAL_REL = "memory/.dreams/phase-signals.json";
static const std::string SHORT_TERM_LOCK_REL = "memory/.dreams/short-term-promotion.lock";

std::string resolve_short_term_recall_store_path(const std::string& wd) {
    return (fs::path(wd) / SHORT_TERM_STORE_REL).string();
}
std::string resolve_short_term_phase_signal_store_path(const std::string& wd) {
    return (fs::path(wd) / SHORT_TERM_PHASE_SIGNAL_REL).string();
}
std::string resolve_short_term_recall_lock_path(const std::string& wd) {
    return (fs::path(wd) / SHORT_TERM_LOCK_REL).string();
}

static void ensure_artifacts_dir(const std::string& wd) {
    auto dir = fs::path(wd) / "memory" / ".dreams";
    fs::create_directories(dir);
}

// ============================================================================
// File lock (対齐 short-term-promotion-store.ts:79-125 withShortTermLock)
//   基于 O_EXCL 锁文件 + "pid:acquiredMs" owner + 60s stale 抢占 + 超时。
// ============================================================================

static const int64_t SHORT_TERM_LOCK_STALE_MS = 60000;
static const int64_t SHORT_TERM_LOCK_WAIT_TIMEOUT_MS = 10000;
static const int SHORT_TERM_LOCK_RETRY_DELAY_MS = 40;

static int64_t now_epoch_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

// 対齐 store.ts:49-59 parseLockOwnerPid
static int parse_lock_owner_pid(const std::string& raw) {
    std::smatch m;
    static const std::regex PID_RE(R"(^(\d+):)");
    std::string trimmed = raw;
    auto s = trimmed.find_first_not_of(" \t\n\r");
    if (s != std::string::npos) trimmed = trimmed.substr(s);
    if (std::regex_search(trimmed, m, PID_RE)) {
        int pid = std::atoi(m[1].str().c_str());
        return pid > 0 ? pid : -1;
    }
    return -1;
}

// 対齐 store.ts:61-73 isProcessLikelyAlive（EPERM/未知视为存活，避免误抢活锁）
static bool is_process_likely_alive(int pid) {
    if (pid <= 0) return false;
    if (::kill(pid, 0) == 0) return true;
    return errno != ESRCH;  // ESRCH=不存在; EPERM 等视为存活
}

template <typename Fn>
static auto with_short_term_lock(const std::string& wd, Fn&& task)
    -> decltype(task()) {
    ensure_artifacts_dir(wd);
    std::string lock_path = resolve_short_term_recall_lock_path(wd);
    int64_t started_at = now_epoch_ms();
    while (true) {
        std::string owner = std::to_string(::getpid()) + ":" + std::to_string(now_epoch_ms());
        int fd = ::open(lock_path.c_str(), O_CREAT | O_EXCL | O_WRONLY, 0600);
        if (fd >= 0) {
            (void)!::write(fd, owner.data(), owner.size());
            ::close(fd);
            struct LockGuard {
                std::string p; std::string o;
                ~LockGuard() {
                    // 仅当锁仍属本 owner 时删除（対齐 store.ts:102-105）
                    std::ifstream f(p);
                    if (f.is_open()) {
                        std::string cur((std::istreambuf_iterator<char>(f)),
                                        std::istreambuf_iterator<char>());
                        f.close();
                        if (cur == o) { std::error_code ec; fs::remove(p, ec); }
                    } else {
                        std::error_code ec; fs::remove(p, ec);
                    }
                }
            } guard{lock_path, owner};
            return task();
        }
        // 锁存在：读 owner，判断 stale 可抢占（対齐 store.ts:109-116）
        std::ifstream f(lock_path);
        std::string existing;
        if (f.is_open()) {
            existing.assign((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
            f.close();
        }
        int64_t acquired_ms = 0;
        {
            auto pos = existing.find(':');
            if (pos != std::string::npos) acquired_ms = std::atoll(existing.c_str() + pos + 1);
        }
        if (!existing.empty() && now_epoch_ms() - acquired_ms > SHORT_TERM_LOCK_STALE_MS) {
            int owner_pid = parse_lock_owner_pid(existing);
            if (owner_pid <= 0 || !is_process_likely_alive(owner_pid)) {
                std::error_code ec; fs::remove(lock_path, ec);
                continue;
            }
        }
        if (now_epoch_ms() - started_at >= SHORT_TERM_LOCK_WAIT_TIMEOUT_MS) {
            throw std::runtime_error("Timed out waiting for short-term promotion lock at " + lock_path);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(SHORT_TERM_LOCK_RETRY_DELAY_MS));
    }
}

// ============================================================================
// Atomic write helpers (対齐 short-term-promotion-memory-write.ts)
// ============================================================================

static std::string sha256_hex(const std::string& input) {
    unsigned char digest[SHA256_DIGEST_LENGTH];
    SHA256(reinterpret_cast<const unsigned char*>(input.data()), input.size(), digest);
    std::ostringstream oss;
    for (int i = 0; i < SHA256_DIGEST_LENGTH; ++i)
        oss << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(digest[i]);
    return oss.str();
}

// 対齐 memory-write.ts:20-53 resolveMemoryWritePath —— 解析符号链接的真实写入目标。
static std::string resolve_memory_write_path(const std::string& file_path) {
    std::error_code ec;
    auto real = fs::weakly_canonical(fs::path(file_path), ec);
    if (!ec && !real.empty()) return real.string();
    return file_path;
}

// 対齐 memory-write.ts:97-144 —— 原子写：tmp(0600)+fsync+rename；带 expected-hash 冲突检测。
// 返回 false 表示检测到并发冲突（MEMORY.md 在读取后被外部改动）。
static bool write_memory_content_atomic(
    const std::string& memory_write_path,
    const std::string& content,
    const std::string& expected_hash) {
    // rename 前重新读取并校验 hash（対齐:108-113 trackedRename 冲突检测）
    if (!expected_hash.empty()) {
        std::ifstream cur(memory_write_path);
        std::string current;
        if (cur.is_open())
            current.assign((std::istreambuf_iterator<char>(cur)), std::istreambuf_iterator<char>());
        if (sha256_hex(current) != expected_hash) return false;  // MemoryWriteConflictError
    }
    std::string tmp = memory_write_path + ".promotion.tmp";
    int fd = ::open(tmp.c_str(), O_CREAT | O_TRUNC | O_WRONLY, 0600);
    if (fd < 0) throw std::runtime_error("cannot open temp for MEMORY.md write: " + tmp);
    ssize_t off = 0, n = static_cast<ssize_t>(content.size());
    while (off < n) {
        ssize_t w = ::write(fd, content.data() + off, n - off);
        if (w < 0) { ::close(fd); std::error_code ec; fs::remove(tmp, ec); throw std::runtime_error("write failed"); }
        off += w;
    }
    ::fsync(fd);
    ::close(fd);
    std::error_code ec;
    fs::rename(tmp, memory_write_path, ec);
    if (ec) { fs::remove(tmp, ec); throw std::runtime_error("atomic rename failed for MEMORY.md"); }
    return true;
}

// ============================================================================
// Store I/O (対齐 short-term-promotion.ts:693-796)
// ============================================================================

struct ShortTermRecallStore {
    int version = 1;
    std::string updated_at;
    std::map<std::string, ShortTermRecallEntry> entries;
};

static ShortTermRecallStore read_store(const std::string& wd, const std::string& now_iso) {
    ShortTermRecallStore store;
    store.version = 1;
    store.updated_at = now_iso;

    auto path = resolve_short_term_recall_store_path(wd);
    std::ifstream f(path);
    if (!f.is_open()) return store;

    try {
        json j = json::parse(f);
        if (j.contains("updatedAt") && j["updatedAt"].is_string())
            store.updated_at = j["updatedAt"];
        if (j.contains("entries") && j["entries"].is_object()) {
            for (auto& [key, val] : j["entries"].items()) {
                if (!val.is_object()) continue;
                ShortTermRecallEntry entry;
                entry.key = key;
                entry.path = normalize_memory_path(val.value("path", ""));
                entry.start_line = val.value("startLine", 0);
                entry.end_line = val.value("endLine", 0);
                entry.source = val.value("source", "memory");
                std::string full_snippet = normalize_snippet(val.value("snippet", ""));
                // 対齐 utils.ts:347-354 —— 污染 snippet 直接丢弃该条目
                if (!full_snippet.empty() &&
                    is_contaminated_dreaming_snippet(
                        full_snippet, is_short_term_session_corpus_path(entry.path))) {
                    continue;
                }
                entry.snippet = truncate_short_term_snippet(full_snippet);  // 対齐:355 800 cap
                entry.recall_count = std::max(0, val.value("recallCount", 0));
                entry.daily_count = std::max(0, val.value("dailyCount", 0));
                entry.grounded_count = std::max(0, val.value("groundedCount", 0));
                entry.total_score = std::max(0.0, val.value("totalScore", 0.0));
                entry.max_score = clamp_score(val.value("maxScore", 0.0));
                entry.first_recalled_at = val.value("firstRecalledAt", now_iso);
                entry.last_recalled_at = val.value("lastRecalledAt", now_iso);
                if (val.contains("promotedAt") && val["promotedAt"].is_string())
                    entry.promoted_at = val["promotedAt"];
                if (val.contains("claimHash") && val["claimHash"].is_string())
                    entry.claim_hash = val["claimHash"];
                if (val.contains("queryHashes") && val["queryHashes"].is_array())
                    for (auto& qh : val["queryHashes"])
                        if (qh.is_string()) entry.query_hashes.push_back(qh);
                if (val.contains("recallDays") && val["recallDays"].is_array())
                    for (auto& d : val["recallDays"])
                        if (d.is_string()) {
                            auto nd = normalize_iso_day(d);
                            if (!nd.empty()) entry.recall_days.push_back(nd);
                        }
                if (val.contains("conceptTags") && val["conceptTags"].is_array())
                    for (auto& t : val["conceptTags"])
                        if (t.is_string()) entry.concept_tags.push_back(normalize_lowercase(t));
                if (entry.concept_tags.empty())
                    entry.concept_tags = derive_concept_tags(entry.path, full_snippet);
                if (!entry.path.empty() && entry.source == "memory")
                    store.entries[key] = entry;
            }
        }
    } catch (...) {}
    return store;
}

// 対齐 utils.ts:443-497 —— retention 淘汰（超 512 条时按 lastRecalledAt/signal/score 保留）
static int64_t parse_store_ts_ms(const std::string& v) {
    if (v.empty()) return INT64_MIN;
    int64_t ms = parse_iso_ms(v);
    return ms > 0 ? ms : INT64_MIN;
}

static void enforce_store_retention(ShortTermRecallStore& store) {
    if (static_cast<int>(store.entries.size()) <= SHORT_TERM_RECALL_MAX_ENTRIES) return;
    std::vector<std::pair<std::string, ShortTermRecallEntry>> items(
        store.entries.begin(), store.entries.end());
    std::sort(items.begin(), items.end(),
              [](const auto& la, const auto& lb) {
        const auto& a = la.second; const auto& b = lb.second;
        int64_t al = parse_store_ts_ms(a.last_recalled_at), bl = parse_store_ts_ms(b.last_recalled_at);
        if (al != bl) return al > bl;
        int as = total_signal_count(a.recall_count, a.daily_count, a.grounded_count);
        int bs = total_signal_count(b.recall_count, b.daily_count, b.grounded_count);
        if (as != bs) return as > bs;
        if (a.total_score != b.total_score) return a.total_score > b.total_score;
        if (a.max_score != b.max_score) return a.max_score > b.max_score;
        int64_t ap = parse_store_ts_ms(a.promoted_at.value_or("")),
                bp = parse_store_ts_ms(b.promoted_at.value_or(""));
        if (ap != bp) return ap > bp;
        return la.first < lb.first;
    });
    items.resize(SHORT_TERM_RECALL_MAX_ENTRIES);
    store.entries.clear();
    for (auto& [k, v] : items) store.entries[k] = std::move(v);
}

static void write_store(const std::string& wd, ShortTermRecallStore& store) {
    enforce_store_retention(store);  // 対齐 store.ts:263
    ensure_artifacts_dir(wd);
    auto path = resolve_short_term_recall_store_path(wd);
    json j;
    j["version"] = 1;
    j["updatedAt"] = store.updated_at;
    j["entries"] = json::object();
    for (const auto& [key, e] : store.entries) {
        json entry;
        entry["key"] = e.key;
        entry["path"] = e.path;
        entry["startLine"] = e.start_line;
        entry["endLine"] = e.end_line;
        entry["source"] = e.source;
        entry["snippet"] = e.snippet;
        entry["recallCount"] = e.recall_count;
        entry["dailyCount"] = e.daily_count;
        entry["groundedCount"] = e.grounded_count;
        entry["totalScore"] = e.total_score;
        entry["maxScore"] = e.max_score;
        entry["firstRecalledAt"] = e.first_recalled_at;
        entry["lastRecalledAt"] = e.last_recalled_at;
        entry["queryHashes"] = e.query_hashes;
        entry["recallDays"] = e.recall_days;
        entry["conceptTags"] = e.concept_tags;
        if (e.claim_hash) entry["claimHash"] = *e.claim_hash;
        if (e.promoted_at) entry["promotedAt"] = *e.promoted_at;
        j["entries"][key] = entry;
    }
    auto tmp = path + ".tmp";
    std::ofstream out(tmp);
    out << j.dump(2) << "\n";
    out.close();
    std::error_code ec;
    fs::permissions(tmp, fs::perms::owner_read | fs::perms::owner_write,
                    fs::perm_options::replace, ec);  // 対齐 mode 0o600
    fs::rename(tmp, path);
}

// ============================================================================
// isShortTermSessionCorpusPath (対齐 short-term-promotion-utils.ts:569-571)
// ============================================================================

bool is_short_term_session_corpus_path(const std::string& file_path) {
    return std::regex_search(normalize_memory_path(file_path), SHORT_TERM_SESSION_CORPUS_RE);
}

// ============================================================================
// isShortTermMemoryPath (対齐 short-term-promotion-utils.ts:555-567)
// ============================================================================

bool is_short_term_memory_path(const std::string& file_path) {
    std::string normalized = normalize_memory_path(file_path);
    // 対齐:557-559 —— 先排除 memory/dreaming/ 托管产物，避免误判为短期记忆。
    if (std::regex_search(normalized, DREAMING_MEMORY_PATH_RE)) return false;
    if (std::regex_search(normalized, SHORT_TERM_PATH_RE)) return true;
    if (std::regex_search(normalized, SHORT_TERM_SESSION_CORPUS_RE)) return true;
    return std::regex_match(normalized, SHORT_TERM_BASENAME_RE);
}

// ============================================================================
// snippet cap (対齐 short-term-promotion-utils.ts:101-106)
// ============================================================================

static std::string truncate_short_term_snippet(const std::string& snippet) {
    // 注：OpenClaw 按 UTF-16 code unit 计长；此处以字符/字节近似，保持 <=800 上限语义。
    if (static_cast<int>(snippet.size()) <= SHORT_TERM_RECALL_MAX_SNIPPET_CHARS) return snippet;
    std::string truncated = snippet.substr(0, SHORT_TERM_RECALL_MAX_SNIPPET_CHARS);
    // trimEnd
    auto e = truncated.find_last_not_of(" \t\n\r");
    if (e == std::string::npos) return "";
    return truncated.substr(0, e + 1);
}

// ============================================================================
// isContaminatedDreamingSnippet (対齐 short-term-promotion-utils.ts:114-186)
// ============================================================================

static std::string consume_dreaming_lead_prefix(const std::string& snippet) {
    size_t index = 0;
    while (index < snippet.size()) {
        // diff prefix (@@ -N,N +/-/* )
        std::smatch m;
        std::string rest = snippet.substr(index);
        if (std::regex_search(rest, m, DREAMING_DIFF_PREFIX_RE,
                              std::regex_constants::match_continuous)) {
            index += m.length(0);
            continue;
        }
        char c = snippet[index];
        if (c == '[' || c == '(') {
            index += 1;
            while (index < snippet.size() && snippet[index] == ' ') index += 1;
            continue;
        }
        if ((c == '-' || c == '*' || c == '+' || c == '>') &&
            index + 1 < snippet.size() && snippet[index + 1] == ' ') {
            index += 2;
            continue;
        }
        break;
    }
    return snippet.substr(index);
}

static bool has_dreaming_narrative_lead(const std::string& snippet) {
    std::string without_prefix = consume_dreaming_lead_prefix(snippet);
    static const std::regex START_LEAD_RE(R"(^(?:Candidate|Reflections?):)", std::regex::icase);
    if (std::regex_search(without_prefix, START_LEAD_RE)) return true;
    // 対齐:154-155 —— 首 200 字符内任意位置出现 Candidate:/Reflections:
    std::string head = without_prefix.substr(0, std::min<size_t>(without_prefix.size(), 200));
    static const std::regex HEAD_LEAD_RE(R"(\b(?:Candidate|Reflections?):)", std::regex::icase);
    return std::regex_search(head, HEAD_LEAD_RE);
}

bool is_contaminated_dreaming_snippet(const std::string& raw, bool allow_transcript_turn_snippet) {
    std::string snippet = normalize_snippet(raw);
    if (snippet.empty()) return false;
    if (std::regex_search(snippet, OPENCLAW_PROMOTION_MARKER_RE) ||
        std::regex_search(snippet, DREAMING_TRANSCRIPT_PROMPT_LINE_RE) ||
        std::regex_search(snippet, RAW_SESSION_METADATA_RE) ||
        std::regex_search(snippet, RAW_CONVERSATION_SUMMARY_RE) ||
        (!allow_transcript_turn_snippet && std::regex_search(snippet, RAW_TRANSCRIPT_TURN_RE)) ||
        std::regex_search(snippet, MEMORY_FLUSH_PROMPT_RE) ||
        std::regex_search(snippet, PROMOTION_SCORE_METADATA_RE)) {
        return true;
    }
    // 复合检测 (対齐:178-185)
    bool has_narrative_lead = has_dreaming_narrative_lead(snippet);
    static const std::regex CONFIDENCE_RE(R"(\bconfidence:\s*\d)", std::regex::icase);
    static const std::regex EVIDENCE_RE(
        R"(\bevidence:\s*(?:memory\/\.dreams\/session-corpus\/|memory\/))", std::regex::icase);
    static const std::regex STATUS_RE(R"(\bstatus:\s*staged\b)", std::regex::icase);
    static const std::regex RECALLS_RE(R"(\brecalls:\s*\d+\b)", std::regex::icase);
    bool has_confidence = std::regex_search(snippet, CONFIDENCE_RE);
    bool has_evidence = std::regex_search(snippet, EVIDENCE_RE);
    bool has_status = std::regex_search(snippet, STATUS_RE);
    bool has_recalls = std::regex_search(snippet, RECALLS_RE);
    return has_narrative_lead && has_confidence && has_evidence && has_status && has_recalls;
}

// ============================================================================
// Scoring components (対齐 short-term-promotion.ts:340-362, 503-512)
// ============================================================================

static double calculate_consolidation_component(const std::vector<std::string>& recall_days) {
    if (recall_days.empty()) return 0.0;
    if (recall_days.size() == 1) return 0.2;
    // Parse and sort days
    std::vector<int64_t> parsed;
    for (const auto& d : recall_days) {
        struct tm tm = {};
        if (sscanf(d.c_str(), "%d-%d-%d", &tm.tm_year, &tm.tm_mon, &tm.tm_mday) == 3) {
            tm.tm_year -= 1900;
            tm.tm_mon -= 1;
            time_t t = timegm(&tm);
            if (t >= 0) parsed.push_back(t * 1000LL);
        }
    }
    if (parsed.size() <= 1) return 0.2;
    std::sort(parsed.begin(), parsed.end());
    double span_days = std::max(0.0, static_cast<double>(parsed.back() - parsed.front()) / DAY_MS);
    double spacing = clamp_score(std::log1p(parsed.size() - 1) / std::log1p(4));
    double span = clamp_score(span_days / 7.0);
    return clamp_score(0.55 * spacing + 0.45 * span);
}

static double calculate_conceptual_component(const std::vector<std::string>& tags) {
    return clamp_score(static_cast<double>(tags.size()) / 6.0);
}

static double calculate_recency_component(double age_days, double half_life_days) {
    if (!std::isfinite(age_days) || age_days < 0) return 1.0;
    if (!std::isfinite(half_life_days) || half_life_days <= 0) return 1.0;
    double lambda = M_LN2 / half_life_days;
    return std::exp(-lambda * age_days);
}

// ============================================================================
// Phase-signal boost (対齐 short-term-promotion.ts:57-91)
// ============================================================================

struct PhaseSignalEntry {
    std::string key;
    int light_hits = 0;
    int rem_hits = 0;
    std::string last_light_at;
    std::string last_rem_at;
};

static int64_t parse_iso_ms(const std::string& iso) {
    struct tm tm = {};
    if (sscanf(iso.c_str(), "%d-%d-%dT%d:%d:%d",
               &tm.tm_year, &tm.tm_mon, &tm.tm_mday,
               &tm.tm_hour, &tm.tm_min, &tm.tm_sec) >= 3) {
        tm.tm_year -= 1900;
        tm.tm_mon -= 1;
        time_t t = timegm(&tm);
        if (t > 0) return t * 1000LL;
    }
    return -1;
}

// 対齐:57-66 calculatePhaseSignalAgeDays（返回 <0 表示 null）
static double calculate_phase_signal_age_days(const std::string& last_seen_at, int64_t now_ms) {
    if (last_seen_at.empty()) return -1.0;
    int64_t parsed = parse_iso_ms(last_seen_at);
    if (parsed <= 0) return -1.0;
    return std::max(0.0, static_cast<double>(now_ms - parsed) / DAY_MS);
}

// 対齐:68-91 calculatePhaseSignalBoost
static double calculate_phase_signal_boost(const PhaseSignalEntry* entry, int64_t now_ms) {
    if (!entry) return 0.0;
    double light_strength = clamp_score(
        std::log1p(std::max(0, entry->light_hits)) / std::log1p(6));
    double rem_strength = clamp_score(
        std::log1p(std::max(0, entry->rem_hits)) / std::log1p(6));
    double light_age = calculate_phase_signal_age_days(entry->last_light_at, now_ms);
    double rem_age = calculate_phase_signal_age_days(entry->last_rem_at, now_ms);
    double light_recency = light_age < 0 ? 0.0
        : clamp_score(calculate_recency_component(light_age, PHASE_SIGNAL_HALF_LIFE_DAYS));
    double rem_recency = rem_age < 0 ? 0.0
        : clamp_score(calculate_recency_component(rem_age, PHASE_SIGNAL_HALF_LIFE_DAYS));
    return clamp_score(
        PHASE_SIGNAL_LIGHT_BOOST_MAX * light_strength * light_recency +
        PHASE_SIGNAL_REM_BOOST_MAX * rem_strength * rem_recency);
}

// 対齐 short-term-promotion-store.ts:162-240 readPhaseSignalStore/normalize
static std::map<std::string, PhaseSignalEntry> read_phase_signal_store(const std::string& wd) {
    std::map<std::string, PhaseSignalEntry> entries;
    auto path = resolve_short_term_phase_signal_store_path(wd);
    std::ifstream f(path);
    if (!f.is_open()) return entries;
    try {
        json j = json::parse(f);
        if (j.contains("entries") && j["entries"].is_object()) {
            for (auto& [map_key, val] : j["entries"].items()) {
                if (!val.is_object()) continue;
                PhaseSignalEntry e;
                std::string ekey = (val.contains("key") && val["key"].is_string())
                    ? val["key"].get<std::string>() : std::string();
                e.key = !ekey.empty() ? ekey : map_key;
                e.light_hits = std::max(0, val.value("lightHits", 0));
                e.rem_hits = std::max(0, val.value("remHits", 0));
                if (e.light_hits == 0 && e.rem_hits == 0) continue;  // 対齐:183
                if (val.contains("lastLightAt") && val["lastLightAt"].is_string())
                    e.last_light_at = val["lastLightAt"];
                if (val.contains("lastRemAt") && val["lastRemAt"].is_string())
                    e.last_rem_at = val["lastRemAt"];
                entries[e.key] = e;
            }
        }
    } catch (...) {}
    return entries;
}

// ============================================================================
// recordShortTermRecalls (対齐 short-term-promotion.ts:809-917)
// ============================================================================

void record_short_term_recalls(
    const std::string& workspace_dir,
    const std::string& query,
    const std::vector<MemorySearchResult>& results,
    const std::string& signal_type,
    bool dedupe_by_query_per_day,
    const std::string& day_bucket,
    int64_t now_ms,
    const std::string& timezone
) {
    if (workspace_dir.empty() || query.empty()) return;

    // Filter to short-term memory paths
    std::vector<const MemorySearchResult*> relevant;
    for (const auto& r : results) {
        if (r.source == MemorySource::Memory && is_short_term_memory_path(r.path))
            relevant.push_back(&r);
    }
    if (relevant.empty()) return;

    if (now_ms <= 0)
        now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
    std::string now_iso = std::to_string(now_ms);  // simplified ISO
    {
        time_t t = now_ms / 1000;
        struct tm tm;
        gmtime_r(&t, &tm);
        char buf[32];
        strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%S.000Z", &tm);
        now_iso = buf;
    }
    std::string qhash = hash_query(query);
    std::string today = day_bucket.empty()
        ? format_memory_dreaming_day(now_ms, timezone)
        : normalize_iso_day(day_bucket);
    if (today.empty()) today = format_memory_dreaming_day(now_ms, timezone);

    // 対齐 store.ts —— 读-改-写临界区加文件锁，避免并发写破坏 recall store。
    with_short_term_lock(workspace_dir, [&]() {
    auto store = read_store(workspace_dir, now_iso);

    for (const auto* rp : relevant) {
        const auto& r = *rp;
        std::string path = normalize_memory_path(r.path);
        std::string snippet = normalize_snippet(r.snippet);
        std::string claim = snippet.empty() ? "" : build_claim_hash(snippet);
        std::string key = build_entry_key(
            "memory", path,
            std::max(1, r.start_line), std::max(1, r.end_line),
            claim);
        auto it = store.entries.find(key);
        bool existing = (it != store.entries.end());
        auto& entry = store.entries[key];
        double score = clamp_score(r.score);

        bool dedupe = dedupe_by_query_per_day && existing &&
            std::find(entry.query_hashes.begin(), entry.query_hashes.end(), qhash) != entry.query_hashes.end() &&
            std::find(entry.recall_days.begin(), entry.recall_days.end(), today) != entry.recall_days.end();

        if (signal_type == "recall")
            entry.recall_count = std::max(0, entry.recall_count + (dedupe ? 0 : 1));
        else if (signal_type == "daily")
            entry.daily_count = std::max(0, entry.daily_count + (dedupe ? 0 : 1));

        entry.total_score = std::max(0.0, entry.total_score + (dedupe ? 0.0 : score));
        entry.max_score = std::max(entry.max_score, dedupe ? 0.0 : score);
        entry.key = key;
        entry.path = path;
        entry.start_line = std::max(1, r.start_line);
        entry.end_line = std::max(1, r.end_line);
        entry.source = "memory";
        if (entry.snippet.empty()) entry.snippet = snippet;
        if (entry.first_recalled_at.empty()) entry.first_recalled_at = now_iso;
        entry.last_recalled_at = now_iso;

        // Merge query hashes (対齐 utils.ts:218-237 mergeQueryHashes：去重+过滤空+保留后 N)
        if (!qhash.empty()) {
            std::vector<std::string> next;
            std::set<std::string> seen;
            for (const auto& v : entry.query_hashes) {
                if (v.empty() || seen.count(v)) continue;
                seen.insert(v); next.push_back(v);
            }
            if (!seen.count(qhash)) next.push_back(qhash);
            if (static_cast<int>(next.size()) > MAX_QUERY_HASHES)
                next.erase(next.begin(), next.begin() + (next.size() - MAX_QUERY_HASHES));
            entry.query_hashes = std::move(next);
        }
        // Merge recall days (対齐 utils.ts:239-259 mergeRecentDistinct)
        if (!today.empty()) {
            std::vector<std::string> next;
            std::set<std::string> seen;
            for (const auto& v : entry.recall_days) {
                if (v.empty() || seen.count(v)) continue;
                seen.insert(v); next.push_back(v);
            }
            if (std::find(next.begin(), next.end(), today) == next.end())
                next.push_back(today);
            if (static_cast<int>(next.size()) > MAX_RECALL_DAYS)
                next.erase(next.begin(), next.begin() + (next.size() - MAX_RECALL_DAYS));
            entry.recall_days = std::move(next);
        }
        // Concept tags
        auto tags = derive_concept_tags(path, snippet);
        if (!tags.empty()) entry.concept_tags = tags;
    }

    store.updated_at = now_iso;
    write_store(workspace_dir, store);
    });  // with_short_term_lock
}

// ============================================================================
// recordDreamingPhaseSignals (対齐 short-term-promotion.ts:1042-1094)
// ============================================================================

void record_dreaming_phase_signals(
    const std::string& workspace_dir,
    const std::string& phase,
    const std::vector<std::string>& keys,
    int64_t now_ms
) {
    if (workspace_dir.empty() || keys.empty()) return;
    if (now_ms <= 0)
        now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
    std::string now_iso;
    {
        time_t t = now_ms / 1000;
        struct tm tm;
        gmtime_r(&t, &tm);
        char buf[32];
        strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%S.000Z", &tm);
        now_iso = buf;
    }

    // Read phase signal store
    auto signal_path = resolve_short_term_phase_signal_store_path(workspace_dir);
    json signals;
    signals["version"] = 1;
    signals["updatedAt"] = now_iso;
    signals["entries"] = json::object();
    try {
        std::ifstream f(signal_path);
        if (f.is_open()) signals = json::parse(f);
    } catch (...) {}

    auto store = read_store(workspace_dir, now_iso);
    std::set<std::string> known_keys;
    for (const auto& [k, _] : store.entries) known_keys.insert(k);

    for (const auto& key : keys) {
        if (key.empty() || !known_keys.count(key)) continue;
        auto& entry = signals["entries"][key];
        if (!entry.is_object()) {
            entry = json::object();
            entry["key"] = key;
            entry["lightHits"] = 0;
            entry["remHits"] = 0;
        }
        if (phase == "light") {
            entry["lightHits"] = std::min(9999, entry.value("lightHits", 0) + 1);
            entry["lastLightAt"] = now_iso;
        } else {
            entry["remHits"] = std::min(9999, entry.value("remHits", 0) + 1);
            entry["lastRemAt"] = now_iso;
        }
    }

    signals["updatedAt"] = now_iso;
    ensure_artifacts_dir(workspace_dir);
    auto tmp = signal_path + ".tmp";
    std::ofstream out(tmp);
    out << signals.dump(2) << "\n";
    out.close();
    fs::rename(tmp, signal_path);
}

// ============================================================================
// rankShortTermPromotionCandidates (対齐 short-term-promotion.ts:1096-1232)
// ============================================================================

std::vector<PromotionCandidate> rank_short_term_promotion_candidates(
    const RankShortTermPromotionOptions& options
) {
    if (options.workspace_dir.empty()) return {};

    int64_t now_ms = options.now_ms;
    if (now_ms <= 0)
        now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
    std::string now_iso;
    {
        time_t t = now_ms / 1000;
        struct tm tm;
        gmtime_r(&t, &tm);
        char buf[32];
        strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%S.000Z", &tm);
        now_iso = buf;
    }

    double min_score = options.min_score.value_or(DEFAULT_PROMOTION_MIN_SCORE);
    int min_recall = options.min_recall_count.value_or(DEFAULT_PROMOTION_MIN_RECALL_COUNT);
    int min_queries = options.min_unique_queries.value_or(DEFAULT_PROMOTION_MIN_UNIQUE_QUERIES);
    int max_age = options.max_age_days.value_or(-1);
    double half_life = options.recency_half_life_days.value_or(DEFAULT_RECENCY_HALF_LIFE_DAYS);

    PromotionWeights weights = options.weights.value_or(DEFAULT_PROMOTION_WEIGHTS);
    // Normalize weights
    double sum = weights.frequency + weights.relevance + weights.diversity +
                 weights.recency + weights.consolidation + weights.conceptual;
    if (sum > 0) {
        weights.frequency /= sum;
        weights.relevance /= sum;
        weights.diversity /= sum;
        weights.recency /= sum;
        weights.consolidation /= sum;
        weights.conceptual /= sum;
    }

    auto store = read_store(options.workspace_dir, now_iso);
    auto phase_signals = read_phase_signal_store(options.workspace_dir);
    std::vector<PromotionCandidate> candidates;

    for (const auto& [key, entry] : store.entries) {
        if (entry.source != "memory" || !is_short_term_memory_path(entry.path)) continue;
        // 対齐:129-135 —— 过滤梦境/转录污染 snippet（session-corpus 允许 transcript turn）。
        if (is_contaminated_dreaming_snippet(
                entry.snippet, is_short_term_session_corpus_path(entry.path))) continue;
        if (!options.include_promoted && entry.promoted_at.has_value()) continue;

        int sig_count = total_signal_count(entry.recall_count, entry.daily_count, entry.grounded_count);
        if (sig_count <= 0 || sig_count < min_recall) continue;

        int unique_queries = static_cast<int>(entry.query_hashes.size());
        int context_diversity = std::max(unique_queries, static_cast<int>(entry.recall_days.size()));
        if (context_diversity < min_queries) continue;

        double avg_score = clamp_score(entry.total_score / std::max(1, sig_count));
        double frequency = clamp_score(std::log1p(sig_count) / std::log1p(10));
        double diversity = clamp_score(static_cast<double>(context_diversity) / 5.0);

        // Age
        int64_t last_ms = 0;
        // Parse ISO timestamp
        struct tm tm = {};
        if (sscanf(entry.last_recalled_at.c_str(), "%d-%d-%dT%d:%d:%d",
                    &tm.tm_year, &tm.tm_mon, &tm.tm_mday,
                    &tm.tm_hour, &tm.tm_min, &tm.tm_sec) >= 3) {
            tm.tm_year -= 1900;
            tm.tm_mon -= 1;
            last_ms = timegm(&tm) * 1000LL;
        }
        double age_days = std::isfinite(static_cast<double>(last_ms)) && last_ms > 0
            ? std::max(0.0, static_cast<double>(now_ms - last_ms) / DAY_MS)
            : 0.0;
        if (max_age >= 0 && age_days > max_age) continue;

        double recency = clamp_score(calculate_recency_component(age_days, half_life));
        double consolidation = std::max(
            calculate_consolidation_component(entry.recall_days),
            clamp_score(static_cast<double>(entry.grounded_count) / 3.0));
        double conceptual = calculate_conceptual_component(entry.concept_tags);

        // phase-signal boost (対齐:174)
        auto ps_it = phase_signals.find(entry.key);
        double phase_boost = calculate_phase_signal_boost(
            ps_it != phase_signals.end() ? &ps_it->second : nullptr, now_ms);

        double score =
            weights.frequency * frequency +
            weights.relevance * avg_score +
            weights.diversity * diversity +
            weights.recency * recency +
            weights.consolidation * consolidation +
            weights.conceptual * conceptual +
            phase_boost;

        if (score < min_score) continue;

        PromotionCandidate cand;
        cand.key = key;
        cand.path = entry.path;
        cand.start_line = entry.start_line;
        cand.end_line = entry.end_line;
        cand.source = "memory";
        cand.snippet = entry.snippet;
        cand.recall_count = entry.recall_count;
        cand.daily_count = entry.daily_count;
        cand.grounded_count = entry.grounded_count;
        cand.signal_count = sig_count;
        cand.avg_score = avg_score;
        cand.max_score = clamp_score(entry.max_score);
        cand.unique_queries = unique_queries;
        cand.claim_hash = entry.claim_hash;
        cand.promoted_at = entry.promoted_at;
        cand.first_recalled_at = entry.first_recalled_at;
        cand.last_recalled_at = entry.last_recalled_at;
        cand.age_days = age_days;
        cand.score = clamp_score(score);
        cand.recall_days = entry.recall_days;
        cand.concept_tags = entry.concept_tags;
        cand.components = {frequency, avg_score, diversity, recency, consolidation, conceptual};
        candidates.push_back(std::move(cand));
    }

    // Sort by score desc, then recall_count desc, then path asc
    std::sort(candidates.begin(), candidates.end(), [](const PromotionCandidate& a, const PromotionCandidate& b) {
        if (a.score != b.score) return a.score > b.score;
        if (a.recall_count != b.recall_count) return a.recall_count > b.recall_count;
        return a.path < b.path;
    });

    int limit = options.limit.value_or(static_cast<int>(candidates.size()));
    if (limit >= 0 && static_cast<int>(candidates.size()) > limit)
        candidates.resize(limit);

    return candidates;
}

// ============================================================================
// readShortTermRecallEntries (対齐 short-term-promotion.ts:1234-1249)
// ============================================================================

std::vector<ShortTermRecallEntry> read_short_term_recall_entries(
    const std::string& workspace_dir, int64_t now_ms
) {
    if (workspace_dir.empty()) return {};
    if (now_ms <= 0)
        now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
    std::string now_iso;
    {
        time_t t = now_ms / 1000;
        struct tm tm;
        gmtime_r(&t, &tm);
        char buf[32];
        strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%S.000Z", &tm);
        now_iso = buf;
    }
    auto store = read_store(workspace_dir, now_iso);
    std::vector<ShortTermRecallEntry> entries;
    for (auto& [key, entry] : store.entries) {
        if (entry.source == "memory" && is_short_term_memory_path(entry.path))
            entries.push_back(entry);
    }
    return entries;
}

// ============================================================================
// applyShortTermPromotions (対齐 short-term-promotion.ts:1443-1575)
// ============================================================================

ApplyShortTermPromotionsResult apply_short_term_promotions(
    const ApplyShortTermPromotionsOptions& options
) {
    std::string wd = options.workspace_dir;
    int64_t now_ms = options.now_ms > 0 ? options.now_ms
        : std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
    std::string now_iso;
    {
        time_t t = now_ms / 1000;
        struct tm tm;
        gmtime_r(&t, &tm);
        char buf[32];
        strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%S.000Z", &tm);
        now_iso = buf;
    }

    std::string memory_path = (fs::path(wd) / "MEMORY.md").string();
    ApplyShortTermPromotionsResult result;
    result.memory_path = memory_path;

    double min_score = options.min_score.value_or(DEFAULT_PROMOTION_MIN_SCORE);
    int min_recall = options.min_recall_count.value_or(DEFAULT_PROMOTION_MIN_RECALL_COUNT);
    int min_queries = options.min_unique_queries.value_or(DEFAULT_PROMOTION_MIN_UNIQUE_QUERIES);
    int limit = options.limit.value_or(static_cast<int>(options.candidates.size()));

    auto store = read_store(wd, now_iso);

    // Filter candidates
    std::vector<PromotionCandidate> selected;
    for (const auto& cand : options.candidates) {
        if (cand.promoted_at.has_value()) continue;
        if (cand.score < min_score) continue;
        int sig = total_signal_count(cand.recall_count, cand.daily_count, cand.grounded_count);
        if (sig < min_recall) continue;
        if (std::max(cand.unique_queries, static_cast<int>(cand.recall_days.size())) < min_queries) continue;
        auto it = store.entries.find(cand.key);
        if (it != store.entries.end() && it->second.promoted_at.has_value()) continue;
        selected.push_back(cand);
        if (static_cast<int>(selected.size()) >= limit) break;
    }

    if (selected.empty()) return result;

    // 解析符号链接后的真实写入路径（対齐 memory-write.ts:20-53 resolveMemoryWritePath）
    std::string write_path = resolve_memory_write_path(memory_path);

    // Read existing MEMORY.md（基于真实写入路径）
    std::string existing;
    {
        std::ifstream f(write_path);
        if (f.is_open()) existing = std::string(
            (std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    }
    std::string expected_hash = sha256_hex(existing);  // 对齐 hashMemoryContent（rename 前校验）

    // Build promotion section (対齐:1403-1422)
    std::string date_str = format_memory_dreaming_day(now_ms, options.timezone.value_or(""));
    std::ostringstream section;
    section << "\n## Promoted From Short-Term Memory (" << date_str << ")\n\n";
    for (const auto& cand : selected) {
        std::string src = cand.path + ":" + std::to_string(cand.start_line)
            + "-" + std::to_string(cand.end_line);
        std::string snip = cand.snippet.empty() ? "(no snippet captured)" : cand.snippet;
        section << "<!-- openclaw-memory-promotion:" << cand.key << " -->\n";
        section << "- " << snip
                << " [score=" << std::fixed << std::setprecision(3) << cand.score
                << " signals=" << cand.signal_count
                << " recalls=" << cand.recall_count
                << " avg=" << std::fixed << std::setprecision(3) << cand.avg_score
                << " source=" << src << "]\n";
    }
    section << "\n";

    // Write to MEMORY.md（原子写 + hash 冲突检测；対齐 memory-write.ts:97-144）
    std::string header = existing.empty() ? "# Long-Term Memory\n\n" : "";
    std::string trailing = existing.empty() ? "" :
        (existing.back() == '\n' ? "" : "\n");
    std::string new_content = header + existing + trailing + section.str();
    if (!write_memory_content_atomic(write_path, new_content, expected_hash)) {
        // 检测到并发冲突（MEMORY.md 在读取后被外部改动）：放弃本次提升，不标记 promoted。
        return result;
    }

    // Mark as promoted in store
    for (const auto& cand : selected) {
        auto it = store.entries.find(cand.key);
        if (it != store.entries.end()) {
            it->second.promoted_at = now_iso;
            it->second.start_line = cand.start_line;
            it->second.end_line = cand.end_line;
            it->second.snippet = cand.snippet;
        }
    }
    store.updated_at = now_iso;
    write_store(wd, store);

    result.applied = static_cast<int>(selected.size());
    result.appended = static_cast<int>(selected.size());
    result.applied_candidates = selected;
    return result;
}

// ============================================================================
// repairShortTermPromotionArtifacts (対齐 short-term-promotion.ts:1743-1823)
// ============================================================================

RepairResult repair_short_term_promotion_artifacts(const std::string& workspace_dir) {
    RepairResult result;
    if (workspace_dir.empty()) return result;

    std::string now_iso;
    {
        auto now = std::chrono::system_clock::now();
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();
        time_t t = ms / 1000;
        struct tm tm;
        gmtime_r(&t, &tm);
        char buf[32];
        strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%S.000Z", &tm);
        now_iso = buf;
    }

    // Remove stale lock
    auto lock_path = resolve_short_term_recall_lock_path(workspace_dir);
    try {
        if (fs::exists(lock_path)) {
            auto mtime = fs::last_write_time(lock_path);
            auto age = std::chrono::duration_cast<std::chrono::milliseconds>(
                fs::file_time_type::clock::now() - mtime).count();
            if (age > 60000) {  // 60s stale threshold
                fs::remove(lock_path);
                result.removed_stale_lock = true;
                result.changed = true;
            }
        }
    } catch (...) {}

    // Normalize store —— 读-改-写临界区加锁（対齐 store.ts withShortTermLock）
    int original_count = 0;
    int normalized_count = 0;
    with_short_term_lock(workspace_dir, [&]() {
        auto store = read_store(workspace_dir, now_iso);
        original_count = static_cast<int>(store.entries.size());

        // Re-derive concept tags for all entries
        for (auto& [key, entry] : store.entries) {
            auto tags = derive_concept_tags(entry.path, entry.snippet);
            if (!tags.empty()) entry.concept_tags = tags;
        }

        store.updated_at = now_iso;
        write_store(workspace_dir, store);
        normalized_count = static_cast<int>(store.entries.size());
    });
    result.removed_invalid_entries = std::max(0, original_count - normalized_count);
    result.rewrote_store = true;
    result.changed = true;

    return result;
}

}  // namespace memory
}  // namespace hunterclaw
