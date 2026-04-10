#include "hunterclaw/memory/dreaming_phases.h"
#include "hunterclaw/memory/concept_vocabulary.h"
#include <filesystem>
#include <fstream>
#include <sstream>
#include <chrono>
#include <cmath>
#include <algorithm>
#include <set>
#include <regex>
#include <iomanip>
#include <nlohmann/json.hpp>
#include <openssl/sha.h>
#include <sys/stat.h>

// 严格对齐 OpenClaw dreaming-phases.ts (1701 行)

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace hunterclaw {
namespace memory {

// ============================================================================
// Regex constants (对齐 dreaming-phases.ts:56-89)
// ============================================================================

static const std::regex DAILY_MEMORY_FILENAME_RE(R"(^(\d{4}-\d{2}-\d{2})\.md$)");
static const std::regex GENERIC_DAY_HEADING_RE(
    R"(^(?:(?:mon|monday|tue|tues|tuesday|wed|wednesday|thu|thur|thurs|thursday|fri|friday|sat|saturday|sun|sunday)(?:,\s+)?)?(?:(?:jan|january|feb|february|mar|march|apr|april|may|jun|june|jul|july|aug|august|sep|sept|september|oct|october|nov|november|dec|december)\s+\d{1,2}(?:st|nd|rd|th)?(?:,\s*\d{4})?|\d{1,2}[/-]\d{1,2}(?:[/-]\d{2,4})?|\d{4}[/-]\d{2}[/-]\d{2})$)",
    std::regex::icase);
static const int64_t DAY_MS = 24LL * 60 * 60 * 1000;

// Managed block markers (对齐:78-89)
struct ManagedBlock {
    std::string heading;
    std::string start_marker;
    std::string end_marker;
};
static const ManagedBlock MANAGED_BLOCKS[] = {
    {"## Light Sleep", "<!-- openclaw:dreaming:light:start -->", "<!-- openclaw:dreaming:light:end -->"},
    {"## REM Sleep", "<!-- openclaw:dreaming:rem:start -->", "<!-- openclaw:dreaming:rem:end -->"},
};

// ============================================================================
// Helper functions
// ============================================================================

static int64_t calculate_lookback_cutoff_ms(int64_t now_ms, int lookback_days) {
    return now_ms - static_cast<int64_t>(std::max(0, lookback_days)) * DAY_MS;
}

static bool is_day_within_lookback(const std::string& day, int64_t cutoff_ms) {
    struct tm tm = {};
    if (sscanf(day.c_str(), "%d-%d-%d", &tm.tm_year, &tm.tm_mon, &tm.tm_mday) != 3) return false;
    tm.tm_year -= 1900;
    tm.tm_mon -= 1;
    tm.tm_hour = 23; tm.tm_min = 59; tm.tm_sec = 59;
    int64_t day_ms = timegm(&tm) * 1000LL + 999;
    return day_ms >= cutoff_ms;
}

// 对齐 dreaming-phases.ts:122-127 normalizeDailyListMarker
static std::string normalize_daily_list_marker(const std::string& line) {
    std::string result = std::regex_replace(line, std::regex(R"(^\d+\.\s+)"), "");
    result = std::regex_replace(result, std::regex(R"(^[-*+]\s+)"), "");
    auto s = result.find_first_not_of(" \t");
    auto e = result.find_last_not_of(" \t");
    return (s == std::string::npos) ? "" : result.substr(s, e - s + 1);
}

// 对齐 dreaming-phases.ts:142-155 isGenericDailyHeading
bool is_generic_daily_heading(const std::string& heading) {
    std::string normalized = heading;
    auto s = normalized.find_first_not_of(" \t");
    if (s == std::string::npos) return true;
    auto e = normalized.find_last_not_of(" \t");
    normalized = normalized.substr(s, e - s + 1);
    normalized = std::regex_replace(normalized, std::regex(R"(\s+)"), " ");
    if (normalized.empty()) return true;
    std::string lower = normalized;
    std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
    if (lower == "today" || lower == "yesterday" || lower == "tomorrow") return true;
    if (lower == "morning" || lower == "afternoon" || lower == "evening" || lower == "night") return true;
    return std::regex_match(normalized, GENERIC_DAY_HEADING_RE);
}

// 对齐 dreaming-phases.ts:129-140 normalizeDailyHeading
static std::optional<std::string> normalize_daily_heading(const std::string& line) {
    std::string trimmed = line;
    auto s = trimmed.find_first_not_of(" \t");
    if (s == std::string::npos) return std::nullopt;
    auto e = trimmed.find_last_not_of(" \t");
    trimmed = trimmed.substr(s, e - s + 1);
    std::smatch m;
    if (!std::regex_match(trimmed, m, std::regex(R"(^#{1,6}\s+(.+)$)"))) return std::nullopt;
    std::string heading = m[1].str();
    heading = normalize_daily_list_marker(heading);
    if (heading.empty()) return std::nullopt;
    if (std::regex_match(heading, DAILY_MEMORY_FILENAME_RE)) return std::nullopt;
    if (is_generic_daily_heading(heading)) return std::nullopt;
    if (heading.size() > static_cast<size_t>(DAILY_INGESTION_MAX_SNIPPET_CHARS))
        heading = heading.substr(0, DAILY_INGESTION_MAX_SNIPPET_CHARS);
    return std::regex_replace(heading, std::regex(R"(\s+)"), " ");
}

// 对齐 dreaming-phases.ts:157-167 normalizeDailySnippet
static std::optional<std::string> normalize_daily_snippet(const std::string& line) {
    std::string trimmed = line;
    auto s = trimmed.find_first_not_of(" \t");
    if (s == std::string::npos) return std::nullopt;
    auto e = trimmed.find_last_not_of(" \t");
    trimmed = trimmed.substr(s, e - s + 1);
    if (trimmed.empty() || trimmed[0] == '#' || trimmed.substr(0, 4) == "<!--") return std::nullopt;
    std::string without_marker = normalize_daily_list_marker(trimmed);
    if (static_cast<int>(without_marker.size()) < DAILY_INGESTION_MIN_SNIPPET_CHARS) return std::nullopt;
    std::string result = without_marker.substr(0, std::min(without_marker.size(), static_cast<size_t>(DAILY_INGESTION_MAX_SNIPPET_CHARS)));
    return std::regex_replace(result, std::regex(R"(\s+)"), " ");
}

// 对齐 dreaming-phases.ts:175-184 buildDailyChunkSnippet
static std::string build_daily_chunk_snippet(
    const std::optional<std::string>& heading,
    const std::vector<std::string>& chunk_lines,
    const std::string& chunk_kind  // "list" | "paragraph" | ""
) {
    std::string joiner = (chunk_kind == "list") ? "; " : " ";
    std::ostringstream body;
    for (size_t i = 0; i < chunk_lines.size(); ++i) {
        if (i > 0) body << joiner;
        body << chunk_lines[i];
    }
    std::string body_str = body.str();
    auto s = body_str.find_first_not_of(" \t");
    auto e = body_str.find_last_not_of(" \t");
    body_str = (s == std::string::npos) ? "" : body_str.substr(s, e - s + 1);
    std::string prefixed = heading ? (*heading + ": " + body_str) : body_str;
    if (prefixed.size() > static_cast<size_t>(DAILY_INGESTION_MAX_SNIPPET_CHARS))
        prefixed = prefixed.substr(0, DAILY_INGESTION_MAX_SNIPPET_CHARS);
    return std::regex_replace(prefixed, std::regex(R"(\s+)"), " ");
}

// ============================================================================
// buildDailySnippetChunks (对齐 dreaming-phases.ts:186-269)
// ============================================================================

std::vector<DailySnippetChunk> build_daily_snippet_chunks(
    const std::vector<std::string>& lines,
    int limit
) {
    std::vector<DailySnippetChunk> chunks;
    std::optional<std::string> active_heading;
    std::vector<std::string> chunk_lines;
    std::string chunk_kind;  // "list" | "paragraph" | ""
    int chunk_start_line = 0;
    int chunk_end_line = 0;

    auto flush_chunk = [&]() {
        if (chunk_lines.empty()) {
            chunk_kind.clear();
            chunk_start_line = 0;
            chunk_end_line = 0;
            return;
        }
        std::string snippet = build_daily_chunk_snippet(active_heading, chunk_lines, chunk_kind);
        auto s = snippet.find_first_not_of(" \t");
        auto e = snippet.find_last_not_of(" \t");
        snippet = (s == std::string::npos) ? "" : snippet.substr(s, e - s + 1);
        if (static_cast<int>(snippet.size()) >= DAILY_INGESTION_MIN_SNIPPET_CHARS) {
            chunks.push_back({chunk_start_line, chunk_end_line, snippet});
        }
        chunk_lines.clear();
        chunk_kind.clear();
        chunk_start_line = 0;
        chunk_end_line = 0;
    };

    for (int index = 0; index < static_cast<int>(lines.size()); ++index) {
        const auto& line = lines[index];

        auto heading = normalize_daily_heading(line);
        if (heading) {
            flush_chunk();
            active_heading = heading;
            continue;
        }

        std::string trimmed = line;
        auto ts = trimmed.find_first_not_of(" \t");
        auto te = trimmed.find_last_not_of(" \t");
        trimmed = (ts == std::string::npos) ? "" : trimmed.substr(ts, te - ts + 1);
        if (trimmed.empty() || trimmed.substr(0, 4) == "<!--") {
            flush_chunk();
            continue;
        }

        auto snippet = normalize_daily_snippet(line);
        if (!snippet) {
            flush_chunk();
            continue;
        }

        std::string next_kind = std::regex_search(trimmed, std::regex(R"(^([-*+]\s+|\d+\.\s+))"))
            ? "list" : "paragraph";

        // Check if we need to split
        bool should_split = !chunk_lines.empty() && (
            chunk_kind != next_kind ||
            static_cast<int>(chunk_lines.size()) >= DAILY_INGESTION_MAX_CHUNK_LINES
        );
        if (!should_split && !chunk_lines.empty()) {
            auto test_lines = chunk_lines;
            test_lines.push_back(*snippet);
            std::string candidate = build_daily_chunk_snippet(active_heading, test_lines, next_kind);
            if (static_cast<int>(candidate.size()) > DAILY_INGESTION_MAX_SNIPPET_CHARS)
                should_split = true;
        }

        if (should_split) flush_chunk();

        if (chunk_lines.empty()) {
            chunk_start_line = index + 1;
            chunk_kind = next_kind;
        }
        chunk_lines.push_back(*snippet);
        chunk_end_line = index + 1;

        if (static_cast<int>(chunks.size()) >= limit) break;
    }

    flush_chunk();
    if (static_cast<int>(chunks.size()) > limit) chunks.resize(limit);
    return chunks;
}

// ============================================================================
// stripManagedDailyDreamingLines (对齐 dreaming-phases.ts:294-330)
// ============================================================================

std::vector<std::string> strip_managed_daily_dreaming_lines(
    const std::vector<std::string>& lines
) {
    std::map<std::string, const ManagedBlock*> by_start;
    for (const auto& b : MANAGED_BLOCKS) by_start[b.start_marker] = &b;

    std::vector<std::string> sanitized = lines;
    for (int i = 0; i < static_cast<int>(sanitized.size()); ++i) {
        std::string trimmed = sanitized[i];
        auto s = trimmed.find_first_not_of(" \t");
        auto e = trimmed.find_last_not_of(" \t");
        trimmed = (s == std::string::npos) ? "" : trimmed.substr(s, e - s + 1);

        auto it = by_start.find(trimmed);
        if (it == by_start.end()) continue;
        const auto* block = it->second;

        int strip_until = -1;
        for (int cursor = i + 1; cursor < static_cast<int>(sanitized.size()); ++cursor) {
            std::string ct = sanitized[cursor];
            auto cs = ct.find_first_not_of(" \t");
            auto ce = ct.find_last_not_of(" \t");
            ct = (cs == std::string::npos) ? "" : ct.substr(cs, ce - cs + 1);
            if (ct == block->end_marker) { strip_until = cursor; break; }
            if (!ct.empty() && (std::regex_search(ct, std::regex(R"(^#{1,6}\s+)")) || by_start.count(ct))) {
                strip_until = cursor - 1; break;
            }
        }
        if (strip_until < i) continue;

        // Find heading above start marker
        int heading_idx = -1;
        for (int k = i - 1; k >= 0; --k) {
            std::string ht = sanitized[k];
            auto hs = ht.find_first_not_of(" \t");
            auto he = ht.find_last_not_of(" \t");
            ht = (hs == std::string::npos) ? "" : ht.substr(hs, he - hs + 1);
            if (ht.empty()) continue;
            heading_idx = (ht == block->heading) ? k : -1;
            break;
        }
        int start_idx = (heading_idx >= 0) ? heading_idx : i;
        for (int k = start_idx; k <= strip_until; ++k) sanitized[k] = "";
        i = strip_until;
    }
    return sanitized;
}

// ============================================================================
// Daily Ingestion State I/O (对齐 dreaming-phases.ts:356-414)
// ============================================================================

static const std::string DAILY_INGESTION_STATE_REL = "memory/.dreams/daily-ingestion.json";

std::string resolve_daily_ingestion_state_path(const std::string& wd) {
    return (fs::path(wd) / DAILY_INGESTION_STATE_REL).string();
}

DailyIngestionState read_daily_ingestion_state(const std::string& wd) {
    DailyIngestionState state;
    auto path = resolve_daily_ingestion_state_path(wd);
    std::ifstream f(path);
    if (!f.is_open()) return state;
    try {
        json j = json::parse(f);
        if (j.contains("files") && j["files"].is_object()) {
            for (auto& [key, val] : j["files"].items()) {
                if (!val.is_object()) continue;
                DailyIngestionFileState fs;
                fs.mtime_ms = std::max(int64_t(0), val.value("mtimeMs", int64_t(0)));
                fs.size = std::max(int64_t(0), val.value("size", int64_t(0)));
                state.files[key] = fs;
            }
        }
    } catch (...) {}
    return state;
}

void write_daily_ingestion_state(const std::string& wd, const DailyIngestionState& state) {
    auto path = resolve_daily_ingestion_state_path(wd);
    fs::create_directories(fs::path(path).parent_path());
    json j;
    j["version"] = 1;
    j["files"] = json::object();
    for (const auto& [key, fs] : state.files) {
        j["files"][key] = {{"mtimeMs", fs.mtime_ms}, {"size", fs.size}};
    }
    auto tmp = path + ".tmp";
    std::ofstream out(tmp);
    out << j.dump(2) << "\n";
    out.close();
    fs::rename(tmp, path);
}

// ============================================================================
// ingestDailyMemorySignals (对齐 dreaming-phases.ts:963-1114)
// ============================================================================

void ingest_daily_memory_signals(
    const std::string& workspace_dir,
    int lookback_days,
    int limit,
    int64_t now_ms,
    const std::string& timezone
) {
    if (now_ms <= 0)
        now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();

    auto state = read_daily_ingestion_state(workspace_dir);
    int64_t cutoff_ms = calculate_lookback_cutoff_ms(now_ms, lookback_days);
    auto memory_dir = fs::path(workspace_dir) / "memory";
    
    // Collect daily files within lookback
    struct DailyFile { std::string filename; std::string day; };
    std::vector<DailyFile> files;
    try {
        for (const auto& entry : fs::directory_iterator(memory_dir)) {
            if (!entry.is_regular_file()) continue;
            std::string name = entry.path().filename().string();
            std::smatch m;
            if (!std::regex_match(name, m, DAILY_MEMORY_FILENAME_RE)) continue;
            std::string day = m[1].str();
            if (!is_day_within_lookback(day, cutoff_ms)) continue;
            files.push_back({name, day});
        }
    } catch (...) { return; }

    // Sort by day descending
    std::sort(files.begin(), files.end(), [](const DailyFile& a, const DailyFile& b) {
        return b.day < a.day;
    });

    int total_cap = std::max(20, limit * 4);
    int per_file_cap = std::max(6, static_cast<int>(std::ceil(
        static_cast<double>(total_cap) / std::max(1, static_cast<int>(files.size())))));
    int total = 0;
    bool changed = false;
    std::map<std::string, DailyIngestionFileState> next_files;

    for (const auto& file : files) {
        std::string relative_path = "memory/" + file.filename;
        auto file_path = memory_dir / file.filename;
        
        struct stat st;
        if (::stat(file_path.string().c_str(), &st) != 0) continue;
        DailyIngestionFileState fp;
        fp.mtime_ms = static_cast<int64_t>(st.st_mtim.tv_sec) * 1000 + st.st_mtim.tv_nsec / 1000000;
        fp.size = st.st_size;
        next_files[relative_path] = fp;

        auto prev = state.files.find(relative_path);
        bool unchanged = (prev != state.files.end() &&
            prev->second.mtime_ms == fp.mtime_ms && prev->second.size == fp.size);
        if (unchanged) continue;
        changed = true;

        // Read file content
        std::ifstream f(file_path);
        if (!f.is_open()) continue;
        std::string raw((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        if (raw.empty()) continue;

        // Split into lines and strip managed blocks
        std::vector<std::string> lines;
        std::istringstream ss(raw);
        std::string line;
        while (std::getline(ss, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            lines.push_back(line);
        }
        lines = strip_managed_daily_dreaming_lines(lines);

        // Build chunks
        auto chunks = build_daily_snippet_chunks(lines, per_file_cap);
        std::vector<MemorySearchResult> results;
        for (const auto& chunk : chunks) {
            MemorySearchResult r;
            r.path = relative_path;
            r.start_line = chunk.start_line;
            r.end_line = chunk.end_line;
            r.score = DAILY_INGESTION_SCORE;
            r.snippet = chunk.snippet;
            r.source = MemorySource::Memory;
            results.push_back(r);
            if (static_cast<int>(results.size()) >= per_file_cap ||
                total + static_cast<int>(results.size()) >= total_cap) break;
        }
        if (results.empty()) continue;

        // Record signals
        record_short_term_recalls(
            workspace_dir,
            "__dreaming_daily__:" + file.day,
            results, "daily", true, file.day, now_ms, timezone);
        total += static_cast<int>(results.size());
        if (total >= total_cap) break;
    }

    // Check for removed files
    if (!changed) {
        for (const auto& [k, _] : state.files) {
            if (next_files.find(k) == next_files.end()) { changed = true; break; }
        }
        if (!changed && state.files.size() != next_files.size()) changed = true;
    }

    if (changed) {
        DailyIngestionState next;
        next.version = 1;
        next.files = next_files;
        write_daily_ingestion_state(workspace_dir, next);
    }
}

// ============================================================================
// seedHistoricalDailyMemorySignals (对齐 dreaming-phases.ts:1116-1221)
// ============================================================================

SeedHistoricalResult seed_historical_daily_memory_signals(
    const std::string& workspace_dir,
    const std::vector<std::string>& file_paths,
    int limit,
    int64_t now_ms,
    const std::string& timezone
) {
    if (now_ms <= 0)
        now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();

    SeedHistoricalResult result;
    struct ValidFile { std::string path; std::string day; };
    std::vector<ValidFile> valid;

    for (const auto& fp : file_paths) {
        std::string trimmed = fp;
        auto s = trimmed.find_first_not_of(" \t");
        if (s == std::string::npos) continue;
        trimmed = trimmed.substr(s);
        std::string name = fs::path(trimmed).filename().string();
        std::smatch m;
        if (std::regex_match(name, m, DAILY_MEMORY_FILENAME_RE)) {
            valid.push_back({trimmed, m[1].str()});
        } else {
            result.skipped_paths.push_back(trimmed);
        }
    }
    std::sort(valid.begin(), valid.end(), [](const ValidFile& a, const ValidFile& b) {
        return b.day < a.day;
    });

    int total_cap = std::max(20, limit * 4);
    int per_file_cap = std::max(6, static_cast<int>(std::ceil(
        static_cast<double>(total_cap) / std::max(1, static_cast<int>(valid.size())))));

    for (const auto& entry : valid) {
        if (result.imported_signal_count >= total_cap) break;
        std::ifstream f(entry.path);
        if (!f.is_open()) { result.skipped_paths.push_back(entry.path); continue; }
        std::string raw((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        if (raw.empty()) continue;

        std::vector<std::string> lines;
        std::istringstream ss(raw);
        std::string line;
        while (std::getline(ss, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            lines.push_back(line);
        }
        lines = strip_managed_daily_dreaming_lines(lines);
        auto chunks = build_daily_snippet_chunks(lines, per_file_cap);
        std::vector<MemorySearchResult> results;
        for (const auto& chunk : chunks) {
            MemorySearchResult r;
            r.path = "memory/" + entry.day + ".md";
            r.start_line = chunk.start_line;
            r.end_line = chunk.end_line;
            r.score = DAILY_INGESTION_SCORE;
            r.snippet = chunk.snippet;
            r.source = MemorySource::Memory;
            results.push_back(r);
            if (static_cast<int>(results.size()) >= per_file_cap ||
                result.imported_signal_count + static_cast<int>(results.size()) >= total_cap) break;
        }
        if (results.empty()) continue;
        record_short_term_recalls(workspace_dir, "__dreaming_daily__:" + entry.day,
            results, "daily", true, entry.day, now_ms, timezone);
        result.imported_signal_count += static_cast<int>(results.size());
        result.imported_file_count++;
    }
    return result;
}

// ============================================================================
// Deduplication helpers (对齐 dreaming-phases.ts:1227-1281)
// ============================================================================

static double entry_average_score(const ShortTermRecallEntry& e) {
    return e.recall_count > 0 ? std::max(0.0, std::min(1.0, e.total_score / e.recall_count)) : 0.0;
}

static std::set<std::string> tokenize_snippet(const std::string& snippet) {
    std::set<std::string> tokens;
    std::string lower = snippet;
    std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
    std::string current;
    for (char c : lower) {
        if (std::isalnum(static_cast<unsigned char>(c))) {
            current += c;
        } else {
            if (!current.empty()) { tokens.insert(current); current.clear(); }
        }
    }
    if (!current.empty()) tokens.insert(current);
    return tokens;
}

static double jaccard_similarity(const std::string& a, const std::string& b) {
    auto ta = tokenize_snippet(a);
    auto tb = tokenize_snippet(b);
    if (ta.empty() || tb.empty()) {
        std::string la = a, lb = b;
        std::transform(la.begin(), la.end(), la.begin(), ::tolower);
        std::transform(lb.begin(), lb.end(), lb.begin(), ::tolower);
        // trim
        auto sa = la.find_first_not_of(" \t"), ea = la.find_last_not_of(" \t");
        auto sb = lb.find_first_not_of(" \t"), eb = lb.find_last_not_of(" \t");
        la = (sa == std::string::npos) ? "" : la.substr(sa, ea - sa + 1);
        lb = (sb == std::string::npos) ? "" : lb.substr(sb, eb - sb + 1);
        return la == lb ? 1.0 : 0.0;
    }
    int intersection = 0;
    for (const auto& t : ta) if (tb.count(t)) intersection++;
    std::set<std::string> union_set;
    union_set.insert(ta.begin(), ta.end());
    union_set.insert(tb.begin(), tb.end());
    return union_set.empty() ? 0.0 : static_cast<double>(intersection) / union_set.size();
}

static std::vector<ShortTermRecallEntry> dedupe_entries(
    const std::vector<ShortTermRecallEntry>& entries, double threshold
) {
    std::vector<ShortTermRecallEntry> deduped;
    for (const auto& entry : entries) {
        bool found = false;
        for (auto& dup : deduped) {
            if (dup.path == entry.path && jaccard_similarity(dup.snippet, entry.snippet) >= threshold) {
                if (entry.recall_count > dup.recall_count) dup.recall_count = entry.recall_count;
                dup.total_score = std::max(dup.total_score, entry.total_score);
                dup.max_score = std::max(dup.max_score, entry.max_score);
                // Merge query hashes, recall days, concept tags
                std::set<std::string> qhs(dup.query_hashes.begin(), dup.query_hashes.end());
                for (const auto& qh : entry.query_hashes) qhs.insert(qh);
                dup.query_hashes.assign(qhs.begin(), qhs.end());
                std::set<std::string> rds(dup.recall_days.begin(), dup.recall_days.end());
                for (const auto& rd : entry.recall_days) rds.insert(rd);
                dup.recall_days.assign(rds.begin(), rds.end());
                std::sort(dup.recall_days.begin(), dup.recall_days.end());
                std::set<std::string> cts(dup.concept_tags.begin(), dup.concept_tags.end());
                for (const auto& ct : entry.concept_tags) cts.insert(ct);
                dup.concept_tags.assign(cts.begin(), cts.end());
                // Update last_recalled_at if newer
                if (entry.last_recalled_at > dup.last_recalled_at)
                    dup.last_recalled_at = entry.last_recalled_at;
                found = true;
                break;
            }
        }
        if (!found) deduped.push_back(entry);
    }
    return deduped;
}

// ============================================================================
// entryWithinLookback (对齐 dreaming-phases.ts:332-339)
// ============================================================================

static bool entry_within_lookback(const ShortTermRecallEntry& entry, int64_t cutoff_ms) {
    for (const auto& day : entry.recall_days) {
        if (is_day_within_lookback(day, cutoff_ms)) return true;
    }
    struct tm tm = {};
    if (sscanf(entry.last_recalled_at.c_str(), "%d-%d-%dT%d:%d:%d",
               &tm.tm_year, &tm.tm_mon, &tm.tm_mday,
               &tm.tm_hour, &tm.tm_min, &tm.tm_sec) >= 3) {
        tm.tm_year -= 1900; tm.tm_mon -= 1;
        int64_t ms = timegm(&tm) * 1000LL;
        return ms >= cutoff_ms;
    }
    return false;
}

// ============================================================================
// runLightDreaming (对齐 dreaming-phases.ts:1433-1511)
// ============================================================================

void run_light_dreaming(
    const std::string& workspace_dir,
    const MemoryLightDreamingConfig& config,
    const DreamingLogger& logger,
    int64_t now_ms
) {
    if (now_ms <= 0)
        now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();

    int64_t cutoff_ms = calculate_lookback_cutoff_ms(now_ms, config.lookback_days);

    // 1. Ingest daily memory signals
    ingest_daily_memory_signals(workspace_dir, config.lookback_days, config.limit, now_ms,
        config.timezone.value_or(""));

    // 2. Read entries, filter by lookback, sort, dedupe
    auto all_entries = read_short_term_recall_entries(workspace_dir, now_ms);
    std::vector<ShortTermRecallEntry> entries;
    for (const auto& e : all_entries) {
        if (entry_within_lookback(e, cutoff_ms)) entries.push_back(e);
    }
    std::sort(entries.begin(), entries.end(), [](const ShortTermRecallEntry& a, const ShortTermRecallEntry& b) {
        if (a.last_recalled_at != b.last_recalled_at)
            return a.last_recalled_at > b.last_recalled_at;
        return a.recall_count > b.recall_count;
    });
    if (static_cast<int>(entries.size()) > config.limit) entries.resize(config.limit);
    entries = dedupe_entries(entries, config.dedupe_similarity);
    auto capped = entries;
    if (static_cast<int>(capped.size()) > config.limit) capped.resize(config.limit);

    // 3. Build light body
    std::vector<std::string> body;
    if (capped.empty()) {
        body.push_back("- No notable updates.");
    } else {
        for (const auto& e : capped) {
            std::string snip = e.snippet.empty() ? "(no snippet captured)" : e.snippet;
            body.push_back("- Candidate: " + snip);
            double avg = entry_average_score(e);
            std::ostringstream avg_s;
            avg_s << std::fixed << std::setprecision(2) << avg;
            body.push_back("  - confidence: " + avg_s.str());
            body.push_back("  - evidence: " + e.path + ":" + std::to_string(e.start_line) + "-" + std::to_string(e.end_line));
            body.push_back("  - recalls: " + std::to_string(e.recall_count));
            body.push_back("  - status: staged");
        }
    }

    // 4. Write phase block
    write_daily_dreaming_phase_block(workspace_dir, "light", body, now_ms,
        config.timezone, config.storage.mode, config.storage.separate_reports);

    // 5. Record phase signals
    std::vector<std::string> keys;
    for (const auto& e : capped) keys.push_back(e.key);
    record_dreaming_phase_signals(workspace_dir, "light", keys, now_ms);

    if (config.enabled && !entries.empty() && config.storage.mode != "separate") {
        logger.info("memory-core: light dreaming staged " +
            std::to_string(std::min(static_cast<int>(entries.size()), config.limit)) +
            " candidate(s) [workspace=" + workspace_dir + "].");
    }
}

// ============================================================================
// runRemDreaming (对齐 dreaming-phases.ts:1513-1593)
// ============================================================================

void run_rem_dreaming(
    const std::string& workspace_dir,
    const MemoryRemDreamingConfig& config,
    const DreamingLogger& logger,
    int64_t now_ms
) {
    if (now_ms <= 0)
        now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();

    int64_t cutoff_ms = calculate_lookback_cutoff_ms(now_ms, config.lookback_days);

    // 1. Ingest daily memory signals
    ingest_daily_memory_signals(workspace_dir, config.lookback_days, config.limit, now_ms,
        config.timezone.value_or(""));

    // 2. Read entries, filter by lookback
    auto all_entries = read_short_term_recall_entries(workspace_dir, now_ms);
    std::vector<ShortTermRecallEntry> entries;
    for (const auto& e : all_entries) {
        if (entry_within_lookback(e, cutoff_ms)) entries.push_back(e);
    }

    // 3. Preview REM
    auto preview = preview_rem_dreaming(entries, config.limit, config.min_pattern_strength);

    // 4. Write phase block
    write_daily_dreaming_phase_block(workspace_dir, "rem", preview.body_lines, now_ms,
        config.timezone, config.storage.mode, config.storage.separate_reports);

    // 5. Record phase signals
    record_dreaming_phase_signals(workspace_dir, "rem", preview.candidate_keys, now_ms);

    if (config.enabled && !entries.empty() && config.storage.mode != "separate") {
        logger.info("memory-core: REM dreaming wrote reflections from " +
            std::to_string(entries.size()) + " recent memory trace(s) [workspace=" + workspace_dir + "].");
    }
}

}  // namespace memory
}  // namespace hunterclaw
