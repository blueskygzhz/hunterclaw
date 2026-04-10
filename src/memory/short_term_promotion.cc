#include "hunterclaw/memory/short_term_promotion.h"
#include "hunterclaw/memory/concept_vocabulary.h"
#include <filesystem>
#include <fstream>
#include <sstream>
#include <chrono>
#include <cmath>
#include <algorithm>
#include <regex>
#include <openssl/sha.h>
#include <iomanip>
#include <nlohmann/json.hpp>

// 严格对齐 OpenClaw short-term-promotion.ts (1879 行)

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace hunterclaw {
namespace memory {

const PromotionWeights DEFAULT_PROMOTION_WEIGHTS = {
    0.24, 0.30, 0.15, 0.15, 0.10, 0.06
};

static const std::regex SHORT_TERM_PATH_RE(
    R"((?:^|\/)memory\/(\d{4})-(\d{2})-(\d{2})\.md$)");
static const std::regex SHORT_TERM_BASENAME_RE(
    R"(^(\d{4})-(\d{2})-(\d{2})\.md$)");
static const std::regex SHORT_TERM_SESSION_CORPUS_RE(
    R"((?:^|\/)memory\/\.dreams\/session-corpus\/(\d{4})-(\d{2})-(\d{2})\.(?:md|txt)$)");
static const int64_t DAY_MS = 24LL * 60 * 60 * 1000;

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
                entry.snippet = normalize_snippet(val.value("snippet", ""));
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
                    entry.concept_tags = derive_concept_tags(entry.path, entry.snippet);
                if (!entry.path.empty() && entry.source == "memory")
                    store.entries[key] = entry;
            }
        }
    } catch (...) {}
    return store;
}

static void write_store(const std::string& wd, const ShortTermRecallStore& store) {
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
    fs::rename(tmp, path);
}

// ============================================================================
// isShortTermMemoryPath (対齐 short-term-promotion.ts:798-807)
// ============================================================================

bool is_short_term_memory_path(const std::string& file_path) {
    std::string normalized = normalize_memory_path(file_path);
    if (std::regex_search(normalized, SHORT_TERM_PATH_RE)) return true;
    if (std::regex_search(normalized, SHORT_TERM_SESSION_CORPUS_RE)) return true;
    return std::regex_match(normalized, SHORT_TERM_BASENAME_RE);
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

        // Merge query hashes
        if (!qhash.empty() &&
            std::find(entry.query_hashes.begin(), entry.query_hashes.end(), qhash)
                == entry.query_hashes.end()) {
            entry.query_hashes.push_back(qhash);
            if (static_cast<int>(entry.query_hashes.size()) > MAX_QUERY_HASHES)
                entry.query_hashes.erase(entry.query_hashes.begin());
        }
        // Merge recall days
        if (!today.empty() &&
            std::find(entry.recall_days.begin(), entry.recall_days.end(), today)
                == entry.recall_days.end()) {
            entry.recall_days.push_back(today);
            if (static_cast<int>(entry.recall_days.size()) > MAX_RECALL_DAYS)
                entry.recall_days.erase(entry.recall_days.begin());
        }
        // Concept tags
        auto tags = derive_concept_tags(path, snippet);
        if (!tags.empty()) entry.concept_tags = tags;
    }

    store.updated_at = now_iso;
    write_store(workspace_dir, store);
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
    std::vector<PromotionCandidate> candidates;

    for (const auto& [key, entry] : store.entries) {
        if (entry.source != "memory" || !is_short_term_memory_path(entry.path)) continue;
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

        double score =
            weights.frequency * frequency +
            weights.relevance * avg_score +
            weights.diversity * diversity +
            weights.recency * recency +
            weights.consolidation * consolidation +
            weights.conceptual * conceptual;

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

    // Read existing MEMORY.md
    std::string existing;
    {
        std::ifstream f(memory_path);
        if (f.is_open()) existing = std::string(
            (std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    }

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
                << " recalls=" << cand.recall_count
                << " avg=" << std::fixed << std::setprecision(3) << cand.avg_score
                << " source=" << src << "]\n";
    }
    section << "\n";

    // Write to MEMORY.md
    std::string header = existing.empty() ? "# Long-Term Memory\n\n" : "";
    std::string trailing = existing.empty() ? "" :
        (existing.back() == '\n' ? "" : "\n");
    std::ofstream out(memory_path);
    out << header << existing << trailing << section.str();
    out.close();

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

    // Normalize store
    auto store = read_store(workspace_dir, now_iso);
    int original_count = static_cast<int>(store.entries.size());

    // Re-derive concept tags for all entries
    for (auto& [key, entry] : store.entries) {
        auto tags = derive_concept_tags(entry.path, entry.snippet);
        if (!tags.empty()) entry.concept_tags = tags;
    }

    store.updated_at = now_iso;
    write_store(workspace_dir, store);

    int normalized_count = static_cast<int>(store.entries.size());
    result.removed_invalid_entries = std::max(0, original_count - normalized_count);
    result.rewrote_store = true;
    result.changed = true;

    return result;
}

}  // namespace memory
}  // namespace hunterclaw
