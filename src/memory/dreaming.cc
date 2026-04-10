#include "hunterclaw/memory/dreaming.h"
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

// 严格对齐 OpenClaw dreaming.ts + dreaming-phases.ts + dreaming-narrative.ts + dreaming-markdown.ts

namespace fs = std::filesystem;

namespace hunterclaw {
namespace memory {

// ============================================================================
// Markdown block replacement helper
// ============================================================================

static std::string replace_managed_block(
    const std::string& original,
    const std::string& heading,
    const std::string& start_marker,
    const std::string& end_marker,
    const std::string& body
) {
    // Check if block already exists
    auto start_pos = original.find(start_marker);
    auto end_pos = original.find(end_marker);
    if (start_pos != std::string::npos && end_pos != std::string::npos && end_pos > start_pos) {
        // Replace existing block content
        std::string result = original.substr(0, start_pos + start_marker.size());
        result += "\n" + body + "\n";
        result += original.substr(end_pos);
        return result;
    }
    // Append new block
    std::string trailing = original.empty() ? "" : (original.back() == '\n' ? "" : "\n");
    return original + trailing + "\n" + heading + "\n\n" + start_marker + "\n" + body + "\n" + end_marker + "\n";
}

static std::string with_trailing_newline(const std::string& content) {
    if (content.empty()) return "";
    return content.back() == '\n' ? content : (content + "\n");
}

// ============================================================================
// writeDailyDreamingPhaseBlock (対齐 dreaming-markdown.ts:58-122)
// ============================================================================

DreamingPhaseBlockResult write_daily_dreaming_phase_block(
    const std::string& workspace_dir,
    const std::string& phase,
    const std::vector<std::string>& body_lines,
    int64_t now_ms,
    const std::optional<std::string>& timezone,
    const std::string& storage_mode,
    bool separate_reports
) {
    if (now_ms <= 0)
        now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();

    std::string body = body_lines.empty()
        ? "- No notable updates."
        : [&]() { std::ostringstream s; for (const auto& l : body_lines) s << l << "\n"; return s.str(); }();

    DreamingPhaseBlockResult result;
    std::string heading = (phase == "light") ? "## Light Sleep" : "## REM Sleep";
    std::string start = "<!-- openclaw:dreaming:" + phase + ":start -->";
    std::string end_m = "<!-- openclaw:dreaming:" + phase + ":end -->";
    std::string day = format_memory_dreaming_day(now_ms, timezone.value_or(""));

    // Inline write
    bool should_inline = (storage_mode == "inline" || storage_mode == "both");
    if (should_inline) {
        std::string inline_path = (fs::path(workspace_dir) / "memory" / (day + ".md")).string();
        fs::create_directories(fs::path(inline_path).parent_path());
        std::string original;
        {
            std::ifstream f(inline_path);
            if (f.is_open()) original = std::string(
                (std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        }
        std::string updated = replace_managed_block(original, heading, start, end_m, body);
        std::ofstream out(inline_path);
        out << with_trailing_newline(updated);
        result.inline_path = inline_path;
    }

    // Separate report
    bool should_separate = (storage_mode == "separate" || storage_mode == "both" || separate_reports);
    if (should_separate) {
        std::string report_path = (fs::path(workspace_dir) / "memory" / "dreaming" / phase / (day + ".md")).string();
        fs::create_directories(fs::path(report_path).parent_path());
        std::string title = (phase == "light") ? "# Light Sleep" : "# REM Sleep";
        std::ofstream out(report_path);
        out << title << "\n\n" << body << "\n";
        result.report_path = report_path;
    }

    return result;
}

// ============================================================================
// writeDeepDreamingReport (対齐 dreaming-markdown.ts:124-149)
// ============================================================================

std::optional<std::string> write_deep_dreaming_report(
    const std::string& workspace_dir,
    const std::vector<std::string>& body_lines,
    int64_t now_ms,
    const std::optional<std::string>& timezone,
    const std::string& storage_mode,
    bool separate_reports
) {
    bool should_write = (storage_mode == "separate" || storage_mode == "both" || separate_reports);
    if (!should_write) return std::nullopt;

    if (now_ms <= 0)
        now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();

    std::string day = format_memory_dreaming_day(now_ms, timezone.value_or(""));
    std::string report_path = (fs::path(workspace_dir) / "memory" / "dreaming" / "deep" / (day + ".md")).string();
    fs::create_directories(fs::path(report_path).parent_path());

    std::string body = body_lines.empty()
        ? "- No durable changes."
        : [&]() { std::ostringstream s; for (const auto& l : body_lines) s << l << "\n"; return s.str(); }();

    std::ofstream out(report_path);
    out << "# Deep Sleep\n\n" << body << "\n";
    return report_path;
}

// ============================================================================
// buildNarrativePrompt (対齐 dreaming-narrative.ts:78-101)
// ============================================================================

std::string build_narrative_prompt(const NarrativePhaseData& data) {
    std::ostringstream lines;
    lines << "Write a dream diary entry from these memory fragments:\n\n";
    int count = 0;
    for (const auto& snippet : data.snippets) {
        if (++count > 12) break;
        lines << "- " << snippet << "\n";
    }
    if (!data.themes.empty()) {
        lines << "\nRecurring themes:\n";
        int tc = 0;
        for (const auto& theme : data.themes) {
            if (++tc > 6) break;
            lines << "- " << theme << "\n";
        }
    }
    if (!data.promotions.empty()) {
        lines << "\nMemories that crystallized into something lasting:\n";
        int pc = 0;
        for (const auto& promo : data.promotions) {
            if (++pc > 5) break;
            lines << "- " << promo << "\n";
        }
    }
    return lines.str();
}

// ============================================================================
// appendNarrativeEntry (対齐 dreaming-narrative.ts:381-430)
// ============================================================================

static std::string format_narrative_date(int64_t epoch_ms, const std::string& /*timezone*/) {
    time_t t = epoch_ms / 1000;
    struct tm tm;
    gmtime_r(&t, &tm);
    char buf[64];
    strftime(buf, sizeof(buf), "%B %d, %Y, %I:%M %p", &tm);
    return std::string(buf);
}

std::string append_narrative_entry(
    const std::string& workspace_dir,
    const std::string& narrative,
    int64_t now_ms,
    const std::optional<std::string>& timezone
) {
    if (now_ms <= 0)
        now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();

    // Resolve DREAMS.md path
    std::string dreams_path = (fs::path(workspace_dir) / "DREAMS.md").string();
    if (!fs::exists(dreams_path)) {
        std::string alt = (fs::path(workspace_dir) / "dreams.md").string();
        if (fs::exists(alt)) dreams_path = alt;
    }
    fs::create_directories(fs::path(dreams_path).parent_path());

    std::string date_str = format_narrative_date(now_ms, timezone.value_or(""));
    std::string entry = "\n---\n\n*" + date_str + "*\n\n" + narrative + "\n";

    std::string existing;
    {
        std::ifstream f(dreams_path);
        if (f.is_open()) existing = std::string(
            (std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    }

    std::string diary_start = "<!-- openclaw:dreaming:diary:start -->";
    std::string diary_end = "<!-- openclaw:dreaming:diary:end -->";

    std::string updated;
    if (existing.find(diary_start) != std::string::npos &&
        existing.find(diary_end) != std::string::npos) {
        auto end_idx = existing.rfind(diary_end);
        updated = existing.substr(0, end_idx) + entry + "\n" + existing.substr(end_idx);
    } else {
        std::string diary = "# Dream Diary\n\n" + diary_start + entry + "\n" + diary_end + "\n";
        updated = existing.empty() ? diary : (diary + "\n" + existing);
    }

    std::ofstream out(dreams_path);
    out << (updated.back() == '\n' ? updated : (updated + "\n"));
    return dreams_path;
}

// ============================================================================
// REM Dreaming Preview (対齐 dreaming-phases.ts:1396-1431)
// ============================================================================

RemDreamingPreview preview_rem_dreaming(
    const std::vector<ShortTermRecallEntry>& entries,
    int limit,
    double min_pattern_strength
) {
    RemDreamingPreview preview;
    preview.source_entry_count = static_cast<int>(entries.size());

    // Build concept tag reflections (対齐:1352-1394)
    std::map<std::string, int> tag_counts;
    std::map<std::string, std::set<std::string>> tag_evidence;
    for (const auto& entry : entries) {
        std::string ev = entry.path + ":" + std::to_string(entry.start_line) + "-" + std::to_string(entry.end_line);
        for (const auto& tag : entry.concept_tags) {
            if (tag.empty()) continue;
            tag_counts[tag]++;
            tag_evidence[tag].insert(ev);
        }
    }

    // Rank themes
    struct TagRank { std::string tag; double strength; int count; std::set<std::string> evidence; };
    std::vector<TagRank> ranked;
    for (const auto& [tag, count] : tag_counts) {
        double strength = std::min(1.0, (static_cast<double>(count) / std::max(1, static_cast<int>(entries.size()))) * 2.0);
        if (strength >= min_pattern_strength) {
            ranked.push_back({tag, strength, count, tag_evidence[tag]});
        }
    }
    std::sort(ranked.begin(), ranked.end(), [](const TagRank& a, const TagRank& b) {
        if (a.strength != b.strength) return a.strength > b.strength;
        if (a.count != b.count) return a.count > b.count;
        return a.tag < b.tag;
    });
    if (static_cast<int>(ranked.size()) > limit) ranked.resize(limit);

    if (ranked.empty()) {
        preview.reflections.push_back("- No strong patterns surfaced.");
    } else {
        for (const auto& r : ranked) {
            preview.reflections.push_back("- Theme: `" + r.tag + "` kept surfacing across " + std::to_string(r.count) + " memories.");
            preview.reflections.push_back("  - confidence: " + std::to_string(r.strength).substr(0, 4));
            std::string ev_str;
            int ec = 0;
            for (const auto& e : r.evidence) {
                if (++ec > 3) break;
                if (!ev_str.empty()) ev_str += ", ";
                ev_str += e;
            }
            preview.reflections.push_back("  - evidence: " + ev_str);
            preview.reflections.push_back("  - note: reflection");
        }
    }

    // Candidate truths (対齐:1330-1349)
    int truth_limit = std::max(1, std::min(3, limit));
    for (const auto& entry : entries) {
        if (entry.promoted_at.has_value()) continue;
        double recency_str = std::min(1.0, std::log1p(entry.recall_count) / std::log1p(6));
        double avg = entry.recall_count > 0
            ? std::max(0.0, std::min(1.0, entry.total_score / entry.recall_count)) : 0.0;
        double consolidation = std::min(1.0, static_cast<double>(entry.recall_days.size()) / 3.0);
        double conceptual = std::min(1.0, static_cast<double>(entry.concept_tags.size()) / 6.0);
        double confidence = avg * 0.45 + recency_str * 0.25 + consolidation * 0.2 + conceptual * 0.1;
        confidence = std::max(0.0, std::min(1.0, confidence));
        if (confidence < 0.45) continue;
        std::string ev = entry.path + ":" + std::to_string(entry.start_line) + "-" + std::to_string(entry.end_line);
        preview.candidate_truths.push_back({
            entry.snippet.empty() ? "(no snippet captured)" : entry.snippet,
            confidence, ev});
        preview.candidate_keys.push_back(entry.key);
        if (static_cast<int>(preview.candidate_truths.size()) >= truth_limit) break;
    }

    // Body lines
    preview.body_lines.push_back("### Reflections");
    for (const auto& r : preview.reflections) preview.body_lines.push_back(r);
    preview.body_lines.push_back("");
    preview.body_lines.push_back("### Possible Lasting Truths");
    if (preview.candidate_truths.empty()) {
        preview.body_lines.push_back("- No strong candidate truths surfaced.");
    } else {
        for (const auto& ct : preview.candidate_truths) {
            std::ostringstream line;
            line << "- " << ct.snippet
                 << " [confidence=" << std::fixed << std::setprecision(2) << ct.confidence
                 << " evidence=" << ct.evidence << "]";
            preview.body_lines.push_back(line.str());
        }
    }

    return preview;
}

// ============================================================================
// runDreamingSweepPhases (対齐 dreaming-phases.ts:1595-1632)
// ============================================================================

void run_dreaming_sweep_phases(
    const std::string& workspace_dir,
    const DreamingLogger& logger,
    int64_t now_ms
) {
    if (now_ms <= 0)
        now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();

    // Light dreaming phase
    {
        auto entries = read_short_term_recall_entries(workspace_dir, now_ms);
        // Sort by last recalled, take top 10
        std::sort(entries.begin(), entries.end(), [](const ShortTermRecallEntry& a, const ShortTermRecallEntry& b) {
            return a.last_recalled_at > b.last_recalled_at;
        });
        if (entries.size() > 10) entries.resize(10);

        std::vector<std::string> body;
        if (entries.empty()) {
            body.push_back("- No notable updates.");
        } else {
            for (const auto& e : entries) {
                std::string snip = e.snippet.empty() ? "(no snippet captured)" : e.snippet;
                body.push_back("- Candidate: " + snip);
                double avg = e.recall_count > 0 ? e.total_score / e.recall_count : 0.0;
                body.push_back("  - confidence: " + std::to_string(avg).substr(0, 4));
                body.push_back("  - evidence: " + e.path + ":" + std::to_string(e.start_line) + "-" + std::to_string(e.end_line));
                body.push_back("  - recalls: " + std::to_string(e.recall_count));
                body.push_back("  - status: staged");
            }
        }

        write_daily_dreaming_phase_block(workspace_dir, "light", body, now_ms);
        // Record phase signals
        std::vector<std::string> keys;
        for (const auto& e : entries) keys.push_back(e.key);
        record_dreaming_phase_signals(workspace_dir, "light", keys, now_ms);
        logger.info("memory-core: light dreaming staged " + std::to_string(entries.size()) + " candidate(s)");
    }

    // REM dreaming phase
    {
        auto entries = read_short_term_recall_entries(workspace_dir, now_ms);
        auto preview = preview_rem_dreaming(entries, 5, 0.15);
        write_daily_dreaming_phase_block(workspace_dir, "rem", preview.body_lines, now_ms);
        record_dreaming_phase_signals(workspace_dir, "rem", preview.candidate_keys, now_ms);
        logger.info("memory-core: REM dreaming wrote reflections from " + std::to_string(entries.size()) + " trace(s)");
    }
}

// ============================================================================
// runShortTermDreamingPromotion (対齐 dreaming.ts:452-617)
// ============================================================================

DreamingPromotionResult run_short_term_dreaming_promotion(
    const std::vector<std::string>& workspace_dirs,
    const ShortTermPromotionDreamingConfig& config,
    const DreamingLogger& logger
) {
    DreamingPromotionResult result;

    for (const auto& wd : workspace_dirs) {
        try {
            int64_t sweep_now = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count();

            // 1. Sweep phases
            run_dreaming_sweep_phases(wd, logger, sweep_now);

            // 2. Repair artifacts
            std::vector<std::string> report_lines;
            auto repair = repair_short_term_promotion_artifacts(wd);
            if (repair.changed) {
                logger.info("memory-core: normalized recall artifacts before dreaming");
                report_lines.push_back("- Repaired recall artifacts.");
            }

            // 3. Rank candidates
            RankShortTermPromotionOptions rank_opts;
            rank_opts.workspace_dir = wd;
            rank_opts.limit = config.limit;
            rank_opts.min_score = config.min_score;
            rank_opts.min_recall_count = config.min_recall_count;
            rank_opts.min_unique_queries = config.min_unique_queries;
            rank_opts.recency_half_life_days = config.recency_half_life_days;
            rank_opts.max_age_days = config.max_age_days;
            rank_opts.now_ms = sweep_now;

            auto candidates = rank_short_term_promotion_candidates(rank_opts);
            result.total_candidates += static_cast<int>(candidates.size());
            report_lines.push_back("- Ranked " + std::to_string(candidates.size()) + " candidate(s) for durable promotion.");

            // 4. Apply promotions
            ApplyShortTermPromotionsOptions apply_opts;
            apply_opts.workspace_dir = wd;
            apply_opts.candidates = candidates;
            apply_opts.limit = config.limit;
            apply_opts.min_score = config.min_score;
            apply_opts.min_recall_count = config.min_recall_count;
            apply_opts.min_unique_queries = config.min_unique_queries;
            apply_opts.max_age_days = config.max_age_days;
            apply_opts.timezone = config.timezone;
            apply_opts.now_ms = sweep_now;

            auto applied = apply_short_term_promotions(apply_opts);
            result.total_applied += applied.applied;
            report_lines.push_back("- Promoted " + std::to_string(applied.applied) + " candidate(s) into MEMORY.md.");

            // 5. Write deep dreaming report
            write_deep_dreaming_report(wd, report_lines, sweep_now, config.timezone,
                config.storage.mode, config.storage.separate_reports);

        } catch (const std::exception& e) {
            result.failed_workspaces++;
            logger.error("memory-core: dreaming promotion failed for " + wd + ": " + e.what());
        }
    }

    logger.info("memory-core: dreaming promotion complete (workspaces=" +
        std::to_string(workspace_dirs.size()) +
        ", candidates=" + std::to_string(result.total_candidates) +
        ", applied=" + std::to_string(result.total_applied) +
        ", failed=" + std::to_string(result.failed_workspaces) + ").");

    return result;
}

// ============================================================================
// buildMemoryFlushPlan (対齐 flush-plan.ts:95-139)
// ============================================================================

static const std::string SILENT_REPLY_TOKEN = "[SILENT]";
static const std::string FLUSH_TARGET_HINT =
    "Store durable memories only in memory/YYYY-MM-DD.md (create memory/ if needed).";
static const std::string FLUSH_APPEND_HINT =
    "If memory/YYYY-MM-DD.md already exists, APPEND new content only and do not overwrite existing entries.";
static const std::string FLUSH_READONLY_HINT =
    "Treat workspace bootstrap/reference files such as MEMORY.md, DREAMS.md, SOUL.md, TOOLS.md, and AGENTS.md as read-only during this flush; never overwrite, replace, or edit them.";

// 对齐 flush-plan.ts:67-72
static std::string ensure_no_reply_hint(const std::string& text) {
    if (text.find(SILENT_REPLY_TOKEN) != std::string::npos) return text;
    return text + "\n\nIf no user-visible reply is needed, start with " + SILENT_REPLY_TOKEN + ".";
}

// 对齐 flush-plan.ts:74-82
static std::string ensure_memory_flush_safety_hints(const std::string& text) {
    std::string next = text;
    // trim
    auto s = next.find_first_not_of(" \t\n\r");
    if (s == std::string::npos) next = ""; else { auto e = next.find_last_not_of(" \t\n\r"); next = next.substr(s, e - s + 1); }

    const std::string hints[] = {FLUSH_TARGET_HINT, FLUSH_APPEND_HINT, FLUSH_READONLY_HINT};
    for (const auto& hint : hints) {
        if (next.find(hint) == std::string::npos) {
            next = next.empty() ? hint : (next + "\n\n" + hint);
        }
    }
    return next;
}

std::optional<MemoryFlushPlan> build_memory_flush_plan(
    int64_t now_ms,
    const std::optional<MemoryFlushConfig>& config
) {
    if (now_ms <= 0)
        now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();

    // 对齐 flush-plan.ts:105: if (defaults?.enabled === false) return null
    if (config && !config->enabled) {
        return std::nullopt;
    }

    // 对齐 flush-plan.ts:109-116
    int soft_tokens = (config && config->soft_threshold_tokens) ? *config->soft_threshold_tokens : 4000;
    int64_t force_bytes = (config && config->force_flush_transcript_bytes) ? *config->force_flush_transcript_bytes : (2LL * 1024 * 1024);
    int reserve_floor = (config && config->reserve_tokens_floor) ? *config->reserve_tokens_floor : 20000;

    std::string date_stamp = format_memory_dreaming_day(now_ms, 
        (config && config->timezone) ? *config->timezone : "");
    std::string relative_path = "memory/" + date_stamp + ".md";

    // 对齐 flush-plan.ts:122-128 — 从 config 读取或使用默认 prompt
    std::string default_prompt = "Pre-compaction memory flush. " + FLUSH_TARGET_HINT + " " +
        FLUSH_READONLY_HINT + " " + FLUSH_APPEND_HINT +
        " Do NOT create timestamped variant files (e.g., " + date_stamp + "-HHMM.md); always use the canonical " + date_stamp + ".md filename." +
        " If nothing to store, reply with " + SILENT_REPLY_TOKEN + ".";

    std::string default_system = "Pre-compaction memory flush turn. The session is near auto-compaction; capture durable memories to disk. " +
        FLUSH_TARGET_HINT + " " + FLUSH_READONLY_HINT + " " + FLUSH_APPEND_HINT +
        " You may reply, but usually " + SILENT_REPLY_TOKEN + " is correct.";

    std::string prompt_base = (config && config->prompt && !config->prompt->empty()) ? *config->prompt : default_prompt;
    std::string sys_prompt = (config && config->system_prompt && !config->system_prompt->empty()) ? *config->system_prompt : default_system;

    // 对齐 flush-plan.ts:122-128 — ensureNoReplyHint + ensureMemoryFlushSafetyHints
    prompt_base = ensure_no_reply_hint(ensure_memory_flush_safety_hints(prompt_base));
    sys_prompt = ensure_no_reply_hint(ensure_memory_flush_safety_hints(sys_prompt));

    // Replace YYYY-MM-DD placeholders (对齐 flush-plan.ts:135-136)
    auto replace_all = [](std::string& str, const std::string& from, const std::string& to) {
        size_t pos = 0;
        while ((pos = str.find(from, pos)) != std::string::npos) {
            str.replace(pos, from.size(), to);
            pos += to.size();
        }
    };
    replace_all(prompt_base, "YYYY-MM-DD", date_stamp);
    replace_all(sys_prompt, "YYYY-MM-DD", date_stamp);

    MemoryFlushPlan plan;
    plan.soft_threshold_tokens = soft_tokens;
    plan.force_flush_transcript_bytes = force_bytes;
    plan.reserve_tokens_floor = reserve_floor;
    plan.prompt = prompt_base;
    plan.system_prompt = sys_prompt;
    plan.relative_path = relative_path;

    return plan;
}

}  // namespace memory
}  // namespace hunterclaw
