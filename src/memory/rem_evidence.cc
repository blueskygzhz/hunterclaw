#include "hunterclaw/memory/rem_evidence.h"
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <regex>
#include <set>
#include <sstream>

// 严格对齐 OpenClaw extensions/memory-core/src/rem-evidence.ts (1078 行)

namespace fs = std::filesystem;

namespace hunterclaw {
namespace memory {

// ============================================================================
// Signal Regex Patterns (对齐 rem-evidence.ts:4-57)
// ============================================================================

static bool re_search(const std::string& text, const std::regex& re) {
    return std::regex_search(text, re);
}

static const std::regex& rem_blocked_section_re() {
    static const std::regex re(
        R"(\b(morning reminders|tasks? for today|to-?do|pickups?|action items?|next steps?|open questions?|stats|setup tasks?|priority contacts|visitors?|top priority candidates|timeline coverage|action items for morning review|test .* skill|heartbeat checks?|date semantics guardrail|still broken|last message (?:&|and) status|plugin \/ service warning|email triage cron)\b)",
        std::regex::icase);
    return re;
}

static const std::regex& rem_generic_section_re() {
    static const std::regex re(
        R"(^(setup|session notes?|notes|summary|major accomplishments?|infrastructure|process improvements?)$)",
        std::regex::icase);
    return re;
}

static const std::regex& rem_memory_signal_re() {
    static const std::regex re(
        R"(\b(always use|prefers?|preference|preferences|standing rule|rule:|use .* calendar|durable|remember)\b)",
        std::regex::icase);
    return re;
}

static const std::regex& rem_build_signal_re() {
    static const std::regex re(
        R"(\b(set up|setup|created|built|rewrite|rewrote|implemented|installed|configured|added|updated|exported|documented)\b)",
        std::regex::icase);
    return re;
}

static const std::regex& rem_incident_signal_re() {
    static const std::regex re(
        R"(\b(fail(?:ed|ing)?|error|issue|problem|auth|expired|broken|unable|missing|required|root cause|consecutive failures?)\b)",
        std::regex::icase);
    return re;
}

static const std::regex& rem_logistics_signal_re() {
    static const std::regex re(
        R"(\b(visitor|arriv(?:e|al|ing)|flight|calendar|reservation|schedule|coordinate|travel|pickup)\b)",
        std::regex::icase);
    return re;
}

static const std::regex& rem_task_signal_re() {
    static const std::regex re(
        R"(\b(reminder|task|to-?do|action item|next step|need to|follow up|respond to|call\b|check\b)\b)",
        std::regex::icase);
    return re;
}

static const std::regex& rem_routing_signal_re() {
    static const std::regex re(
        R"(\b(categor(?:ize|ized|ization)|route|routing|workflow|processor|read later|auto-implement|codex|razor)\b)",
        std::regex::icase);
    return re;
}

static const std::regex& rem_operator_rule_signal_re() {
    static const std::regex re(R"(\b(learned:|rule:|always [a-z])\b)", std::regex::icase);
    return re;
}

static const std::regex& rem_externalization_signal_re() {
    static const std::regex re(
        R"(\b(obsidian|memory|tracker|notes captured|committed to memory|updated .*md|documented|file comparison table)\b)",
        std::regex::icase);
    return re;
}

static const std::regex& rem_retry_signal_re() {
    static const std::regex re(
        R"(\b(repeat(?:ed|edly)?|again|retry|root cause|third attempt|fourth|fifth|consecutive failures?)\b)",
        std::regex::icase);
    return re;
}

static const std::regex& rem_person_pattern_signal_re() {
    static const std::regex re(
        R"(\b(relationship|who:|patterns?:|failure modes?:|best stance:|space|boundaries|timing|family quick reference)\b)",
        std::regex::icase);
    return re;
}

static const std::regex& rem_situational_signal_re() {
    static const std::regex re(
        R"(\b(hotel|address|phone|reservation|check-?in|check-?out|flight|arrival|departure|terminal|price shown|invoice|pending items|screenshot|butler)\b)",
        std::regex::icase);
    return re;
}

static const std::regex& rem_persistence_signal_re() {
    static const std::regex re(
        R"(\b(always|preference|prefers?|standing rule|best stance|failure modes?|key patterns?|relationship|who:|important .* keep track|people in .* life|partner|wife|husband|boyfriend|girlfriend)\b)",
        std::regex::icase);
    return re;
}

static const std::regex& rem_transient_signal_re() {
    static const std::regex re(
        R"(\b(today|this session|in progress|installed|booked|confirmed|pending|status:|action pending|open items?|next steps?|issue:|diagnostics|screenshot|source file|insight files|thread\b|ticket|price shown|calendar fix|cron fixes|security audit|updates? this session|bought:|order\b)\b)",
        std::regex::icase);
    return re;
}

static const std::regex& rem_section_persistence_title_re() {
    static const std::regex re(
        R"(\b(preferences? learned|preference|people update|relationship|standing|patterns?|identity|memory)\b)",
        std::regex::icase);
    return re;
}

static const std::regex& rem_section_transient_title_re() {
    static const std::regex re(
        R"(\b(setup|fix|fixes|audit|booked|call|today|session|updates?|file paths|open items?|next steps?|research pipeline|info gathered|calendar|tickets?)\b)",
        std::regex::icase);
    return re;
}

static const std::regex& rem_metadata_heavy_signal_re() {
    static const std::regex re(
        R"(\b(address|phone|email|website|google maps|source file|insight files|conversation id|thread has|order\b|reservation\b|price\b|cost\b|ticket|uuid|url:|model:|workspace:|bindings:|accountid|config change|path:)\b)",
        std::regex::icase);
    return re;
}

static const std::regex& rem_project_meta_signal_re() {
    static const std::regex re(
        R"(\b(strategy|audit|discussion|research|topic|candidate|north star|pipeline|data dump|export|draft|insights? draft|weekly|analysis|findings)\b)",
        std::regex::icase);
    return re;
}

static const std::regex& rem_process_frame_signal_re() {
    static const std::regex re(
        R"(\b(dossier|registry|cadence|framework|facts,\s*timeline|open loops|next actions|auto preference rollups?|insights? draft created)\b)",
        std::regex::icase);
    return re;
}

static const std::regex& rem_tooling_meta_signal_re() {
    static const std::regex re(
        R"(\b(cli|tool|tools\.md|agents\.md|sessionssend|subagents?|spawn|tmux|xurl|bird|codex exec|interactive codex)\b)",
        std::regex::icase);
    return re;
}

static const std::regex& rem_monitoring_signal_re() {
    static const std::regex re(
        R"(\b(heartbeat|ariston|collect-temps|low pressure|exit code|invalid[_-]?grant|token expired|token revoked|warning\/error|warning|alert(?:ing)?|checkpoint at|daily note file already existed|header creation|local time verified|calendar access failed|gmail .* failed|no proactive .* sent|silent log only|gateway restarted successfully|still no response|no reply yet|blocked\b|passkey|credential|password in bws|working correctly|catchup completed)\b)",
        std::regex::icase);
    return re;
}

static const std::regex& rem_stable_person_signal_re() {
    static const std::regex re(
        R"(\b(partner|wife|husband|boyfriend|girlfriend|relationship interest|lives in)\b)",
        std::regex::icase);
    return re;
}

static const std::regex& rem_explicit_preference_signal_re() {
    static const std::regex re(
        R"(\b(explicitly|wants?|does not want|don't want|default .* should|should default to|likes?|dislikes?|treat .* as|prefers?)\b)",
        std::regex::icase);
    return re;
}

static const std::regex& rem_travel_decision_signal_re() {
    static const std::regex re(
        R"(\b(routing|cabin|business class|trip brief|departure|arrival|hotel|reservation|tickets?|show tonight|cheaper alternatives?|venue timing)\b)",
        std::regex::icase);
    return re;
}

static const std::regex& rem_specificity_burden_re() {
    static const std::regex re(
        R"(\b(?:jan|feb|mar|apr|may|jun|jul|aug|sep|oct|nov|dec)\b|€|\$\d|→|\b\d{1,2}:\d{2}\b|\+\d{6,})",
        std::regex::icase);
    return re;
}

static const std::regex& rem_time_prefix_re() {
    static const std::regex re(R"(^\d{1,2}:\d{2}\s*-\s*)");
    return re;
}

static const std::regex& rem_code_fence_re() {
    static const std::regex re(R"(^\s*```)");
    return re;
}

static const std::regex& rem_table_re() {
    static const std::regex re(R"(^\s*\|.*\|\s*$)");
    return re;
}

static const std::regex& rem_table_divider_re() {
    static const std::regex re(R"(^\s*\|?[\s:-]+\|[\s|:-]*$)");
    return re;
}

// Constants (对齐 rem-evidence.ts:59-65)
static constexpr int MAX_GROUNDED_REM_FILES = 512;
static constexpr int64_t MAX_GROUNDED_REM_FILE_BYTES = 1000000;
static constexpr int REM_SUMMARY_FACT_LIMIT = 4;
static constexpr int REM_SUMMARY_REFLECTION_LIMIT = 4;
static constexpr int REM_SUMMARY_MEMORY_LIMIT = 3;

// ============================================================================
// Helper Functions (对齐 rem-evidence.ts:128-154)
// ============================================================================

static std::string normalize_whitespace(const std::string& value) {
    std::string trimmed;
    auto start = value.find_first_not_of(" \t\n\r");
    auto end = value.find_last_not_of(" \t\n\r");
    if (start == std::string::npos) return "";
    trimmed = value.substr(start, end - start + 1);
    return std::regex_replace(trimmed, std::regex(R"(\s+)"), " ");
}

static std::string strip_markdown(const std::string& text) {
    std::string result = text;
    result = std::regex_replace(result, std::regex(R"(!\[[^\]]*\]\([^)]*\))"), "");
    result = std::regex_replace(result, std::regex(R"(\[([^\]]+)\]\([^)]*\))"), "$1");
    result = std::regex_replace(result, std::regex(R"([`*_~>#])"), "");
    result = std::regex_replace(result, std::regex(R"(\s+)"), " ");
    return normalize_whitespace(result);
}

static std::string sanitize_section_title(const std::string& title) {
    return normalize_whitespace(std::regex_replace(strip_markdown(title), rem_time_prefix_re(), ""));
}

static std::string make_ref(const std::string& path_value, int start_line, int end_line = -1) {
    if (end_line < 0) end_line = start_line;
    return start_line == end_line
        ? path_value + ":" + std::to_string(start_line)
        : path_value + ":" + std::to_string(start_line) + "-" + std::to_string(end_line);
}

// ============================================================================
// Markdown Section Parsing (对齐 rem-evidence.ts:156-215)
// ============================================================================

struct ParsedSectionLine {
    int line = 0;
    std::string text;
};

struct ParsedMarkdownSection {
    std::string title;
    int start_line = 0;
    int end_line = 0;
    std::vector<ParsedSectionLine> lines;
};

static std::vector<ParsedMarkdownSection> parse_markdown_sections(const std::string& content) {
    std::vector<ParsedMarkdownSection> sections;
    std::vector<std::string> raw_lines;
    std::istringstream stream(content);
    std::string line;
    while (std::getline(stream, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        raw_lines.push_back(line);
    }

    ParsedMarkdownSection* current = nullptr;
    bool in_code_fence = false;
    static const std::regex heading_re(R"(^\s{0,3}(#{2,6})\s+(.+)$)");

    auto flush = [&]() {
        if (!current) return;
        std::vector<ParsedSectionLine> meaningful;
        for (auto& entry : current->lines) {
            if (normalize_whitespace(entry.text).length() > 0) {
                meaningful.push_back(entry);
            }
        }
        if (!meaningful.empty()) {
            int end = meaningful.back().line;
            ParsedMarkdownSection sec = *current;
            sec.end_line = end;
            sec.lines = std::move(meaningful);
            sections.push_back(std::move(sec));
        }
        current = nullptr;
    };

    ParsedMarkdownSection temp;
    for (size_t index = 0; index < raw_lines.size(); ++index) {
        const auto& raw_line = raw_lines[index];
        int line_number = static_cast<int>(index) + 1;

        if (re_search(raw_line, rem_code_fence_re())) {
            in_code_fence = !in_code_fence;
            continue;
        }
        if (in_code_fence) continue;

        std::smatch m;
        if (std::regex_match(raw_line, m, heading_re) && m.size() >= 3) {
            flush();
            temp = {};
            temp.title = sanitize_section_title(m[2].str());
            temp.start_line = line_number;
            temp.end_line = line_number;
            current = &temp;
            continue;
        }
        if (!current) continue;

        current->end_line = line_number;
        std::string trimmed = raw_line;
        auto ts = trimmed.find_first_not_of(" \t");
        if (ts != std::string::npos) trimmed = trimmed.substr(ts);
        else trimmed = "";
        auto te = trimmed.find_last_not_of(" \t");
        if (te != std::string::npos) trimmed = trimmed.substr(0, te + 1);

        if (trimmed.empty()) continue;
        if (std::regex_match(trimmed, std::regex(R"(^---+$)"))) continue;
        if (re_search(trimmed, rem_table_re())) continue;
        if (re_search(trimmed, rem_table_divider_re())) continue;

        current->lines.push_back({line_number, raw_line});
    }
    flush();
    return sections;
}

// ============================================================================
// Section to Snippets (对齐 rem-evidence.ts:217-239)
// ============================================================================

struct SectionSnippet {
    std::string text;
    int line = 0;
};

static std::vector<SectionSnippet> section_to_snippets(const ParsedMarkdownSection& section) {
    std::vector<SectionSnippet> snippets;
    std::set<std::string> seen;
    static const std::regex bullet_re(R"(^(?:[-*+]|\d+\.)\s+(?:\[[ xX]\]\s*)?(.*))");

    for (const auto& entry : section.lines) {
        std::string trimmed = entry.text;
        auto s = trimmed.find_first_not_of(" \t");
        if (s != std::string::npos) trimmed = trimmed.substr(s);
        auto e = trimmed.find_last_not_of(" \t");
        if (e != std::string::npos) trimmed = trimmed.substr(0, e + 1);
        if (trimmed.empty()) continue;

        std::smatch m;
        std::string candidate = trimmed;
        if (std::regex_match(trimmed, m, bullet_re) && m.size() >= 2) {
            candidate = m[1].str();
        }
        std::string text = normalize_whitespace(strip_markdown(candidate));
        if (text.length() < 10) continue;

        std::string dedupe_key = text;
        std::transform(dedupe_key.begin(), dedupe_key.end(), dedupe_key.begin(),
                       [](unsigned char c) { return std::tolower(c); });
        if (seen.count(dedupe_key)) continue;
        seen.insert(dedupe_key);
        snippets.push_back({text, entry.line});
    }
    return snippets;
}

static int count_matching_snippets(const std::vector<SectionSnippet>& snippets, const std::regex& pattern) {
    int count = 0;
    for (const auto& s : snippets) {
        if (re_search(s.text, pattern)) ++count;
    }
    return count;
}

// ============================================================================
// Section Scoring (对齐 rem-evidence.ts:251-291)
// ============================================================================

struct SectionScores {
    int preference = 0;
    int build = 0;
    int incident = 0;
    int logistics = 0;
    int tasks = 0;
    int routing = 0;
    int externalization = 0;
    int retries = 0;
    double overall = 0.0;
};

static SectionScores score_section(const ParsedMarkdownSection& section, const std::vector<SectionSnippet>& snippets) {
    const std::string& title = section.title;
    auto title_bonus = [&](const std::regex& pattern) -> int { return re_search(title, pattern) ? 1 : 0; };

    SectionScores scores;
    scores.preference = count_matching_snippets(snippets, rem_memory_signal_re()) + title_bonus(rem_memory_signal_re());
    scores.build = count_matching_snippets(snippets, rem_build_signal_re()) + title_bonus(rem_build_signal_re());
    scores.incident = count_matching_snippets(snippets, rem_incident_signal_re()) + title_bonus(rem_incident_signal_re());
    scores.logistics = count_matching_snippets(snippets, rem_logistics_signal_re()) + title_bonus(rem_logistics_signal_re());
    scores.tasks = count_matching_snippets(snippets, rem_task_signal_re()) + title_bonus(rem_task_signal_re());
    scores.routing = count_matching_snippets(snippets, rem_routing_signal_re()) + title_bonus(rem_routing_signal_re());
    scores.externalization = count_matching_snippets(snippets, rem_externalization_signal_re()) + title_bonus(rem_externalization_signal_re());
    scores.retries = count_matching_snippets(snippets, rem_retry_signal_re()) + title_bonus(rem_retry_signal_re());
    scores.overall =
        scores.preference * 2.0 +
        scores.build * 1.6 +
        scores.incident * 1.6 +
        scores.logistics * 1.2 +
        scores.routing * 1.8 +
        scores.externalization * 1.4 +
        std::min(static_cast<int>(snippets.size()), 3) * 0.3 -
        (re_search(title, rem_generic_section_re()) ? 0.8 : 0.0);
    return scores;
}

// ============================================================================
// Snippet Scoring (对齐 rem-evidence.ts:293-323)
// ============================================================================

static double score_snippet(const std::string& text, const std::string& title) {
    double score = 1.0;
    if (re_search(text, rem_memory_signal_re())) score += 2.2;
    if (re_search(text, rem_build_signal_re())) score += 1.2;
    if (re_search(text, rem_incident_signal_re())) score += 1.2;
    if (re_search(text, rem_logistics_signal_re())) score += 0.9;
    if (re_search(text, rem_routing_signal_re())) score += 1.4;
    if (re_search(text, rem_externalization_signal_re())) score += 1.1;
    if (re_search(text, rem_retry_signal_re())) score += 0.9;
    if (re_search(text, rem_task_signal_re()) && !re_search(text, rem_build_signal_re())) score -= 0.8;
    if (!title.empty() && !re_search(title, rem_generic_section_re())) score += 0.25;
    return score;
}

// ============================================================================
// chooseSummarySnippets (对齐 rem-evidence.ts:325-341)
// ============================================================================

static std::vector<SectionSnippet> choose_summary_snippets(
    const ParsedMarkdownSection& section,
    const std::vector<SectionSnippet>& snippets
) {
    int limit = re_search(section.title, rem_generic_section_re()) ? 2 : 3;
    std::vector<SectionSnippet> sorted = snippets;
    std::sort(sorted.begin(), sorted.end(), [&](const SectionSnippet& a, const SectionSnippet& b) {
        double sa = score_snippet(a.text, section.title);
        double sb = score_snippet(b.text, section.title);
        if (sb != sa) return sb > sa;
        return a.line < b.line;
    });
    if (static_cast<int>(sorted.size()) > limit) sorted.resize(limit);
    std::sort(sorted.begin(), sorted.end(), [](const SectionSnippet& a, const SectionSnippet& b) {
        return a.line < b.line;
    });
    return sorted;
}

// ============================================================================
// Summary helpers (对齐 rem-evidence.ts:343-377)
// ============================================================================

static std::string join_summary_parts(const std::vector<std::string>& parts) {
    if (parts.size() <= 1) return parts.empty() ? "" : parts[0];
    if (parts.size() == 2) return parts[0] + " and " + parts[1];
    std::string result;
    for (size_t i = 0; i < parts.size() - 1; ++i) {
        if (i > 0) result += "; ";
        result += parts[i];
    }
    result += "; and " + parts.back();
    return result;
}

struct SectionSummary {
    std::string title;
    std::string text;
    std::vector<std::string> refs;
    SectionScores scores;
};

static std::optional<SectionSummary> summarize_section(
    const std::string& path_value,
    const ParsedMarkdownSection& section
) {
    if (re_search(section.title, rem_blocked_section_re())) return std::nullopt;
    auto snippets = section_to_snippets(section);
    if (snippets.empty()) return std::nullopt;
    auto selected = choose_summary_snippets(section, snippets);
    if (selected.empty()) return std::nullopt;

    std::string title = sanitize_section_title(section.title);
    std::vector<std::string> parts;
    for (const auto& s : selected) parts.push_back(s.text);
    std::string body = join_summary_parts(parts);
    std::string text = title.empty() || re_search(title, rem_generic_section_re()) ? body : title + ": " + body;

    std::vector<std::string> refs;
    for (const auto& s : selected) refs.push_back(make_ref(path_value, s.line));

    return SectionSummary{title, text, refs, score_section(section, snippets)};
}

// ============================================================================
// Candidate Scoring (对齐 rem-evidence.ts:419-488)
// ============================================================================

static double score_candidate_snippet(const std::string& text, const std::string& title) {
    double score = 0.0;
    if (re_search(text, rem_persistence_signal_re())) score += 3.2;
    if (re_search(text, rem_memory_signal_re())) score += 2.4;
    if (re_search(text, rem_explicit_preference_signal_re())) score += 1.8;
    if (re_search(text, rem_person_pattern_signal_re())) score += 2.3;
    if (re_search(text, rem_operator_rule_signal_re())) score += 1.6;
    if (re_search(text, rem_section_persistence_title_re())) score += 1.2;
    if (re_search(text, rem_stable_person_signal_re())) score += 1.5;
    if (re_search(text, rem_metadata_heavy_signal_re())) score -= 2.4;
    std::string combined = title + " " + text;
    if (re_search(combined, rem_project_meta_signal_re())) score -= 2.2;
    if (re_search(text, rem_process_frame_signal_re())) score -= 2.4;
    if (re_search(text, rem_tooling_meta_signal_re()) && !re_search(text, rem_stable_person_signal_re())) score -= 2.1;
    std::string title_text = title + " " + text;
    if (re_search(title_text, rem_monitoring_signal_re()) && !re_search(text, rem_memory_signal_re())) score -= 4.2;
    if (re_search(text, rem_travel_decision_signal_re())) score -= 2.6;
    if (re_search(text, rem_specificity_burden_re()) && !re_search(text, rem_stable_person_signal_re())) score -= 1.2;
    if (re_search(text, rem_situational_signal_re())) score -= 2.8;
    if (re_search(text, rem_transient_signal_re())) score -= 2.0;
    if (re_search(text, rem_incident_signal_re())) score -= 1.6;
    if (re_search(text, rem_task_signal_re())) score -= 1.2;
    if (re_search(text, rem_logistics_signal_re()) && !re_search(text, rem_memory_signal_re())) score -= 1.4;
    if (re_search(text, rem_build_signal_re()) && !re_search(text, rem_memory_signal_re())) score -= 0.8;
    if (re_search(title, rem_section_transient_title_re()) && !re_search(title, rem_section_persistence_title_re())) score -= 1.2;
    if (text.find('`') != std::string::npos || text.find('/') != std::string::npos ||
        re_search(text, std::regex(R"(https?://)", std::regex::icase))) score -= 0.8;
    return score;
}

static bool is_durable_signal_snippet(const std::string& text, const std::string& title) {
    return re_search(text, rem_memory_signal_re()) ||
           re_search(text, rem_persistence_signal_re()) ||
           re_search(text, rem_explicit_preference_signal_re()) ||
           re_search(title + " " + text, rem_stable_person_signal_re()) ||
           re_search(text, rem_person_pattern_signal_re());
}

static std::string classify_candidate_lean(const std::string& text, const std::string& title) {
    double score = score_candidate_snippet(text, title);
    if (score >= 4.0) return "likely_durable";
    if (score <= 0.25 || re_search(text, rem_situational_signal_re()) || re_search(text, rem_transient_signal_re()))
        return "likely_situational";
    return "unclear";
}

// ============================================================================
// Claim Atomization (对齐 rem-evidence.ts:575-642)
// ============================================================================

static int find_top_level_delimiter(const std::string& text, char delimiter) {
    int round_depth = 0, square_depth = 0;
    for (size_t i = 0; i < text.size(); ++i) {
        char c = text[i];
        if (c == '(') round_depth++;
        else if (c == ')') round_depth = std::max(0, round_depth - 1);
        else if (c == '[') square_depth++;
        else if (c == ']') square_depth = std::max(0, square_depth - 1);
        else if (c == delimiter && round_depth == 0 && square_depth == 0) return static_cast<int>(i);
    }
    return -1;
}

static std::vector<std::string> split_top_level_clauses(const std::string& text, char delimiter) {
    std::vector<std::string> parts;
    std::string rest = text;
    while (!rest.empty()) {
        int split_at = find_top_level_delimiter(rest, delimiter);
        if (split_at < 0) { parts.push_back(rest); break; }
        parts.push_back(rest.substr(0, split_at));
        rest = rest.substr(split_at + 1);
    }
    std::vector<std::string> result;
    for (auto& p : parts) {
        auto nw = normalize_whitespace(p);
        if (!nw.empty()) result.push_back(nw);
    }
    return result;
}

static std::vector<std::string> atomize_claim_text(const std::string& text) {
    auto normalized = normalize_whitespace(text);
    if (normalized.empty()) return {};
    auto clauses = split_top_level_clauses(normalized, ';');
    std::vector<std::string> atomic;
    for (const auto& c : clauses) atomic.push_back(normalize_whitespace(c));
    // Remove empty and deduplicate
    std::set<std::string> seen;
    std::vector<std::string> result;
    for (const auto& a : atomic) {
        if (!a.empty() && !seen.count(a)) {
            seen.insert(a);
            result.push_back(a);
            if (result.size() >= 3) break;
        }
    }
    return result;
}

// ============================================================================
// Compact helpers (对齐 rem-evidence.ts:379-407)
// ============================================================================

static std::string compact_candidate_title(const std::string& title) {
    std::string compact = sanitize_section_title(title);
    compact = std::regex_replace(compact, std::regex(R"(\s*\((?:via:|from qmd \+ memory|this session)[^)]+\)\s*)", std::regex::icase), " ");
    compact = std::regex_replace(compact, std::regex(R"(\s*[—-]\s*(?:research results.*|in progress.*|working.*|installed.*|booked.*|proposed.*|clarified.*|candidate.*|fixes.*|updates?.*)$)", std::regex::icase), "");
    compact = normalize_whitespace(compact);
    if (std::regex_match(compact, std::regex(R"(^(?:preferences? learned|candidate facts?)$)", std::regex::icase))) return "";
    compact = std::regex_replace(compact, std::regex(R"(^preference:\s*)", std::regex::icase), "");
    return compact;
}

static std::string compact_candidate_snippet_text(const std::string& text, const std::string& title) {
    auto normalized = normalize_whitespace(text);
    std::string title_text = title + " " + normalized;
    if (re_search(title_text, rem_monitoring_signal_re())) {
        normalized = std::regex_replace(normalized, std::regex(R"(\b(?:local time verified[^.;]*[.;]?\s*))", std::regex::icase), "");
        normalized = std::regex_replace(normalized, std::regex(R"(\b(?:daily note file already existed[^.;]*[.;]?\s*))", std::regex::icase), "");
        normalized = std::regex_replace(normalized, std::regex(R"(\b(?:header creation[^.;]*[.;]?\s*))", std::regex::icase), "");
        return normalize_whitespace(normalized);
    }
    if (re_search(title_text, rem_stable_person_signal_re())) {
        auto dot = normalized.find('.');
        if (dot != std::string::npos && dot > 0) return normalize_whitespace(normalized.substr(0, dot + 1));
    }
    return normalized;
}

static std::string build_fact_text(const std::string& title, const std::string& text) {
    auto compact = compact_candidate_title(title);
    if (compact.empty()) return text;
    if (re_search(compact, rem_section_persistence_title_re()) ||
        re_search(compact, rem_stable_person_signal_re()) ||
        re_search(compact, std::regex(R"(\b(relationship|people mentioned|people update|identity)\b)", std::regex::icase))) {
        return compact + ": " + text;
    }
    return text;
}

// ============================================================================
// Fact / Candidate choosers (对齐 rem-evidence.ts:490-569)
// ============================================================================

struct ScoredSnippet {
    SectionSnippet snippet;
    double score;
};

static std::vector<SectionSnippet> choose_fact_snippets(
    const ParsedMarkdownSection& section,
    const std::vector<SectionSnippet>& snippets
) {
    std::vector<ScoredSnippet> scored;
    for (auto& s : snippets) {
        auto text = compact_candidate_snippet_text(s.text, section.title);
        double sc = score_candidate_snippet(text, section.title) + (re_search(text, rem_memory_signal_re()) ? 0.6 : 0.0);
        std::string title_text = section.title + " " + text;
        if (re_search(title_text, rem_monitoring_signal_re()) && !is_durable_signal_snippet(text, section.title)) continue;
        if (text.length() < 18 || sc < 1.4) continue;
        scored.push_back({{text, s.line}, sc});
    }
    std::sort(scored.begin(), scored.end(), [](const ScoredSnippet& a, const ScoredSnippet& b) {
        if (b.score != a.score) return b.score < a.score;
        return a.snippet.line < b.snippet.line;
    });
    std::vector<SectionSnippet> result;
    for (size_t i = 0; i < std::min(scored.size(), size_t(2)); ++i) result.push_back(scored[i].snippet);
    std::sort(result.begin(), result.end(), [](const SectionSnippet& a, const SectionSnippet& b) {
        return a.line < b.line;
    });
    return result;
}

static std::vector<ScoredSnippet> choose_candidate_snippets_scored(
    const ParsedMarkdownSection& section,
    const std::vector<SectionSnippet>& snippets
) {
    std::vector<ScoredSnippet> all_scored;
    for (auto& s : snippets) {
        auto text = compact_candidate_snippet_text(s.text, section.title);
        auto claims = atomize_claim_text(text);
        double max_claim_score = -1e9;
        for (const auto& claim : claims) max_claim_score = std::max(max_claim_score, score_candidate_snippet(claim, section.title));
        double sc = std::max(score_candidate_snippet(text, section.title), max_claim_score);
        std::string title_text = section.title + " " + text;
        if (re_search(title_text, rem_monitoring_signal_re()) && !is_durable_signal_snippet(text, section.title)) continue;
        if (text.length() < 18 || sc < 1.8) continue;
        // Expand claims into individual candidates
        for (const auto& claim : claims) {
            double claim_score = score_candidate_snippet(claim, section.title);
            auto candidate_text = build_fact_text(section.title, claim);
            if (candidate_text.length() >= 12 && claim_score >= 1.8) {
                all_scored.push_back({{candidate_text, s.line}, claim_score});
            }
        }
    }
    std::sort(all_scored.begin(), all_scored.end(), [](const ScoredSnippet& a, const ScoredSnippet& b) {
        if (b.score != a.score) return b.score < a.score;
        return a.snippet.line < b.snippet.line;
    });
    return all_scored;
}

// ============================================================================
// Reflection helpers (对齐 rem-evidence.ts:655-678)
// ============================================================================

static void add_reflection(
    std::vector<GroundedRemPreviewItem>& reflections,
    std::set<std::string>& seen,
    const std::string& text,
    const std::vector<std::string>& refs
) {
    auto normalized = normalize_whitespace(text);
    std::string key = normalized;
    std::transform(key.begin(), key.end(), key.begin(), [](unsigned char c) { return std::tolower(c); });
    if (normalized.empty() || seen.count(key)) return;
    seen.insert(key);
    reflections.push_back({normalized, refs});
}

// ============================================================================
// previewGroundedRemForFile (对齐 rem-evidence.ts:680-1019)
// ============================================================================

GroundedRemFilePreview preview_grounded_rem_for_file(
    const std::string& rel_path,
    const std::string& content
) {
    auto sections = parse_markdown_sections(content);

    // Section scores
    struct SectionWithSnippets {
        ParsedMarkdownSection section;
        std::vector<SectionSnippet> snippets;
    };
    std::vector<SectionWithSnippets> section_scores;
    for (auto& sec : sections) {
        section_scores.push_back({sec, section_to_snippets(sec)});
    }

    // Monitoring signal count
    int monitoring_signal = 0;
    for (auto& ss : section_scores) {
        monitoring_signal += count_matching_snippets(ss.snippets, rem_monitoring_signal_re());
        if (re_search(ss.section.title, rem_monitoring_signal_re())) monitoring_signal++;
    }

    // Summaries
    std::vector<SectionSummary> summaries;
    for (auto& ss : section_scores) {
        auto sum = summarize_section(rel_path, ss.section);
        if (sum) summaries.push_back(*sum);
    }

    // Fact summaries
    struct FactSummary {
        std::string text;
        std::vector<std::string> refs;
        double score;
    };
    std::vector<FactSummary> fact_summaries;
    for (auto& sec : sections) {
        if (re_search(sec.title, rem_blocked_section_re())) continue;
        auto snips = section_to_snippets(sec);
        if (snips.empty()) continue;
        for (auto& s : choose_fact_snippets(sec, snips)) {
            fact_summaries.push_back({
                build_fact_text(sec.title, s.text),
                {make_ref(rel_path, s.line)},
                score_candidate_snippet(s.text, sec.title)
            });
        }
    }

    // Memory implications
    std::vector<GroundedRemPreviewItem> memory_implications;
    for (auto& sum : summaries) {
        bool is_op_rule = re_search(sum.title, std::regex(R"(process improvements?)", std::regex::icase)) || re_search(sum.text, rem_operator_rule_signal_re());
        if (sum.scores.preference > 0 || is_op_rule) {
            std::string impl_text = std::regex_replace(sum.text, std::regex(R"(^[^:]+:\s*)"), "");
            memory_implications.push_back({impl_text, sum.refs});
        }
    }
    // Deduplicate
    {
        std::set<std::string> seen_impl;
        std::vector<GroundedRemPreviewItem> deduped;
        for (auto& m : memory_implications) {
            if (!seen_impl.count(m.text)) { seen_impl.insert(m.text); deduped.push_back(m); }
        }
        memory_implications = deduped;
        if (memory_implications.size() > static_cast<size_t>(REM_SUMMARY_MEMORY_LIMIT))
            memory_implications.resize(REM_SUMMARY_MEMORY_LIMIT);
    }

    // Candidate snippets
    std::vector<ScoredSnippet> all_candidate_snippets;
    for (auto& sec : sections) {
        if (re_search(sec.title, rem_blocked_section_re())) continue;
        auto snips = section_to_snippets(sec);
        if (snips.empty()) continue;
        auto scored = choose_candidate_snippets_scored(sec, snips);
        all_candidate_snippets.insert(all_candidate_snippets.end(), scored.begin(), scored.end());
    }

    // Sort candidates by lean rank then score
    auto lean_rank = [](const std::string& lean) -> int {
        if (lean == "likely_durable") return 0;
        if (lean == "unclear") return 1;
        return 2;
    };
    std::vector<GroundedRemCandidate> candidates;
    for (auto& cs : all_candidate_snippets) {
        std::string lean = classify_candidate_lean(cs.snippet.text, "");
        candidates.push_back({cs.snippet.text, {make_ref(rel_path, cs.snippet.line)}, lean});
    }
    std::sort(candidates.begin(), candidates.end(), [&](const GroundedRemCandidate& a, const GroundedRemCandidate& b) {
        int la = lean_rank(a.lean), lb = lean_rank(b.lean);
        if (la != lb) return la < lb;
        // Can't recover score easily, sort by text deterministically
        return a.text < b.text;
    });
    // Deduplicate
    {
        std::set<std::string> seen_c;
        std::vector<GroundedRemCandidate> deduped;
        for (auto& c : candidates) {
            if (!seen_c.count(c.text)) { seen_c.insert(c.text); deduped.push_back(c); }
        }
        candidates = deduped;
        if (candidates.size() > 4) candidates.resize(4);
    }

    // Durable implications from candidates
    std::vector<GroundedRemPreviewItem> durable_implications;
    for (auto& cs : all_candidate_snippets) {
        std::string lean = classify_candidate_lean(cs.snippet.text, "");
        if (lean == "likely_durable" || cs.score >= 4.0) {
            durable_implications.push_back({cs.snippet.text, {make_ref(rel_path, cs.snippet.line)}});
        }
    }
    {
        std::set<std::string> seen_d;
        std::vector<GroundedRemPreviewItem> deduped;
        for (auto& d : durable_implications) {
            if (!seen_d.count(d.text)) { seen_d.insert(d.text); deduped.push_back(d); }
        }
        durable_implications = deduped;
        if (durable_implications.size() > static_cast<size_t>(REM_SUMMARY_MEMORY_LIMIT))
            durable_implications.resize(REM_SUMMARY_MEMORY_LIMIT);
    }

    // Effective memory implications
    auto& effective = !durable_implications.empty() ? durable_implications : memory_implications;

    // Facts
    std::vector<GroundedRemPreviewItem> facts;
    std::set<std::string> used_fact_texts;
    std::sort(fact_summaries.begin(), fact_summaries.end(), [](const FactSummary& a, const FactSummary& b) {
        return b.score < a.score;
    });
    for (auto& fs_item : fact_summaries) {
        std::string key = fs_item.text;
        std::transform(key.begin(), key.end(), key.begin(), [](unsigned char c) { return std::tolower(c); });
        if (used_fact_texts.count(key)) continue;
        used_fact_texts.insert(key);
        facts.push_back({fs_item.text, fs_item.refs});
        if (static_cast<int>(facts.size()) >= REM_SUMMARY_FACT_LIMIT) break;
    }

    // Fallback facts from summaries when no fact snippets extracted
    if (facts.empty() && monitoring_signal < 3) {
        auto best_for = [&](auto metric_fn) -> SectionSummary* {
            SectionSummary* best = nullptr;
            double best_val = -1;
            for (auto& s : summaries) {
                double v = metric_fn(s);
                if (v > 0 && (v > best_val || (v == best_val && (!best || s.scores.overall > best->scores.overall)))) {
                    best = &s;
                    best_val = v;
                }
            }
            return best;
        };
        std::vector<SectionSummary*> priority = {
            best_for([](const SectionSummary& s) { return static_cast<double>(s.scores.preference); }),
            best_for([](const SectionSummary& s) { return static_cast<double>(s.scores.routing); }),
            best_for([](const SectionSummary& s) { return static_cast<double>(s.scores.externalization); }),
        };
        // Add overall-sorted summaries
        auto sorted_sums = summaries;
        std::sort(sorted_sums.begin(), sorted_sums.end(), [](const SectionSummary& a, const SectionSummary& b) {
            return b.scores.overall < a.scores.overall;
        });
        for (auto& s : sorted_sums) priority.push_back(&s);

        for (auto* sp : priority) {
            if (!sp) continue;
            std::string key = sp->text;
            std::transform(key.begin(), key.end(), key.begin(), [](unsigned char c) { return std::tolower(c); });
            if (used_fact_texts.count(key)) continue;
            used_fact_texts.insert(key);
            facts.push_back({sp->text, sp->refs});
            if (static_cast<int>(facts.size()) >= REM_SUMMARY_FACT_LIMIT) break;
        }
    }

    // Reflections (对齐 rem-evidence.ts:833-968)
    std::vector<GroundedRemPreviewItem> reflections;
    std::set<std::string> seen_reflections;

    int build_signal = 0, incident_signal = 0, logistics_signal = 0;
    int routing_signal_total = 0, externalization_signal_total = 0, retry_signal = 0, task_signal = 0;
    for (auto& s : summaries) {
        build_signal += s.scores.build;
        incident_signal += s.scores.incident;
        logistics_signal += s.scores.logistics;
        routing_signal_total += s.scores.routing;
        externalization_signal_total += s.scores.externalization;
        retry_signal += s.scores.retries;
    }
    for (auto& ss : section_scores) {
        task_signal += score_section(ss.section, ss.snippets).tasks;
    }

    bool multi_relationship = false;
    {
        int rel_count = 0;
        for (auto& f : facts) if (re_search(f.text, rem_stable_person_signal_re())) rel_count++;
        multi_relationship = rel_count >= 2;
    }

    if (facts.empty() && monitoring_signal >= 3) {
        int sl = sections.empty() ? 1 : sections.front().start_line;
        int el = sections.empty() ? 1 : sections.back().end_line;
        add_reflection(reflections, seen_reflections,
            "This day reads mostly as monitoring and operational state, not as durable memory. It should be treated as current-state exhaust unless a clearer rule or preference appears.",
            {make_ref(rel_path, sl, el)});
    }
    if (!effective.empty()) {
        std::vector<std::string> eff_refs;
        for (auto& e : effective) for (auto& r : e.refs) { eff_refs.push_back(r); if (eff_refs.size() >= 3) break; }
        add_reflection(reflections, seen_reflections,
            "A stable rule or preference was stated explicitly, which suggests operating choices are being made legible instead of left implicit.",
            eff_refs);
    }
    if (multi_relationship) {
        std::vector<std::string> rel_refs;
        for (auto& f : facts) if (re_search(f.text, rem_stable_person_signal_re())) for (auto& r : f.refs) { rel_refs.push_back(r); if (rel_refs.size() >= 3) break; }
        add_reflection(reflections, seen_reflections,
            "More than one active relationship thread appears in the same day, which means person-memory matters operationally: who each person is should be kept separate from the transient date or venue details attached to them.",
            rel_refs);
    }
    if (!multi_relationship && !facts.empty() && routing_signal_total >= 2 && build_signal >= incident_signal) {
        SectionSummary* best_routing = nullptr;
        for (auto& s : summaries) if (s.scores.routing > 0 || re_search(s.text, rem_routing_signal_re())) {
            if (!best_routing || s.scores.overall > best_routing->scores.overall) best_routing = &s;
        }
        if (best_routing) {
            add_reflection(reflections, seen_reflections,
                "The strongest pattern here is a preference for converting messy inbound information into routed workflows with different downstream actions, instead of handling each case manually.",
                best_routing->refs);
        }
    }
    if (!multi_relationship && !facts.empty() && externalization_signal_total >= 2) {
        SectionSummary* best_ext = nullptr;
        for (auto& s : summaries) if (s.scores.externalization > 0) {
            if (!best_ext || s.scores.overall > best_ext->scores.overall) best_ext = &s;
        }
        if (best_ext) {
            add_reflection(reflections, seen_reflections,
                "Important context tends to get externalized quickly into notes, trackers, or memory surfaces, which suggests a preference for explicit systems over holding context informally.",
                best_ext->refs);
        }
    }
    if (!multi_relationship && !facts.empty() && build_signal >= 2) {
        std::vector<std::string> build_refs;
        for (auto& f : facts) if (re_search(f.text, rem_build_signal_re())) for (auto& r : f.refs) { build_refs.push_back(r); if (build_refs.size() >= 3) break; }
        if (!build_refs.empty()) {
            add_reflection(reflections, seen_reflections,
                "The day leaned toward building operator infrastructure, which suggests the interaction is often used to reshape the system around recurring needs rather than just complete isolated tasks.",
                build_refs);
        }
    }
    if (!facts.empty() && incident_signal >= 2) {
        SectionSummary* best_inc = nullptr;
        for (auto& s : summaries) if (s.scores.incident > 0) {
            if (!best_inc || s.scores.overall > best_inc->scores.overall) best_inc = &s;
        }
        if (best_inc) {
            add_reflection(reflections, seen_reflections,
                retry_signal >= 2
                    ? "When something breaks repeatedly, the response is systematic: retries, root-cause narrowing, and preserving enough state to resume once the blocker is fixed."
                    : "A meaningful share of the day went into friction, and the interaction pattern looks pragmatic rather than emotional: diagnose the blocker, preserve state, and move on.",
                best_inc->refs);
        }
    }
    if (!multi_relationship && !facts.empty() && logistics_signal >= 2) {
        std::vector<std::string> log_refs;
        for (auto& f : facts) if (re_search(f.text, rem_logistics_signal_re())) for (auto& r : f.refs) { log_refs.push_back(r); if (log_refs.size() >= 3) break; }
        if (!log_refs.empty()) {
            add_reflection(reflections, seen_reflections,
                "Personal logistics and operating-system work are being managed in the same surface, which suggests a preference for one integrated control plane rather than separate personal and technical loops.",
                log_refs);
        }
    }
    if (task_signal >= 3 && reflections.empty()) {
        int sl = sections.empty() ? 1 : sections.front().start_line;
        int el = sections.empty() ? 1 : sections.back().end_line;
        add_reflection(reflections, seen_reflections,
            "The raw note is mostly task and current-state material, so it should not be over-read as memory.",
            {make_ref(rel_path, sl, el)});
    }

    // Reflection limit (对齐 rem-evidence.ts:970-976)
    int reflection_limit = facts.empty() ? 1
        : (facts.size() == 1 ? 2 : std::min(REM_SUMMARY_REFLECTION_LIMIT, static_cast<int>(facts.size()) + 1));
    if (static_cast<int>(reflections.size()) > reflection_limit)
        reflections.resize(reflection_limit);

    // Render markdown (对齐 rem-evidence.ts:978-1009)
    std::ostringstream md;
    md << "## What Happened\n";
    if (facts.empty()) {
        md << "1. No grounded facts were extracted.\n";
    } else {
        for (size_t i = 0; i < facts.size(); ++i) {
            md << (i + 1) << ". " << facts[i].text << " [";
            for (size_t j = 0; j < facts[i].refs.size(); ++j) {
                if (j > 0) md << ", ";
                md << facts[i].refs[j];
            }
            md << "]\n";
        }
    }
    md << "\n## Reflections\n";
    if (reflections.empty()) {
        md << "1. No grounded reflections emerged from this note yet.\n";
    } else {
        for (size_t i = 0; i < reflections.size(); ++i) {
            md << (i + 1) << ". " << reflections[i].text << " [";
            for (size_t j = 0; j < reflections[i].refs.size(); ++j) {
                if (j > 0) md << ", ";
                md << reflections[i].refs[j];
            }
            md << "]\n";
        }
    }
    if (!candidates.empty()) {
        md << "\n## Candidates\n";
        for (auto& c : candidates) {
            md << "- [" << c.lean << "] " << c.text << " [";
            for (size_t j = 0; j < c.refs.size(); ++j) {
                if (j > 0) md << ", ";
                md << c.refs[j];
            }
            md << "]\n";
        }
    }
    if (!effective.empty()) {
        md << "\n## Possible Lasting Updates\n";
        for (auto& e : effective) {
            md << "- " << e.text << " [";
            for (size_t j = 0; j < e.refs.size(); ++j) {
                if (j > 0) md << ", ";
                md << e.refs[j];
            }
            md << "]\n";
        }
    }

    return {
        rel_path,
        facts,
        reflections,
        effective,
        candidates,
        md.str()
    };
}

// ============================================================================
// previewGroundedRemMarkdown (对齐 rem-evidence.ts:1021-1077)
// ============================================================================

static void collect_markdown_files(const fs::path& target, std::set<fs::path>& found) {
    if (found.size() >= static_cast<size_t>(MAX_GROUNDED_REM_FILES)) return;
    try {
        auto status = fs::symlink_status(target);
        if (fs::is_symlink(status)) return;
        if (fs::is_directory(status)) {
            for (auto& entry : fs::directory_iterator(target)) {
                if (entry.is_directory()) {
                    std::string name = entry.path().filename().string();
                    if (name == ".git" || name == "node_modules") continue;
                }
                collect_markdown_files(entry.path(), found);
            }
            return;
        }
        if (fs::is_regular_file(status) &&
            fs::file_size(target) <= static_cast<uintmax_t>(MAX_GROUNDED_REM_FILE_BYTES)) {
            std::string ext = target.extension().string();
            std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return std::tolower(c); });
            if (ext == ".md") found.insert(fs::canonical(target));
        }
    } catch (...) {}
}

GroundedRemPreviewResult preview_grounded_rem_markdown(
    const std::string& workspace_dir,
    const std::vector<std::string>& input_paths
) {
    std::set<fs::path> found;
    for (const auto& input : input_paths) {
        auto trimmed = input;
        auto s = trimmed.find_first_not_of(" \t\n\r");
        if (s == std::string::npos) continue;
        trimmed = trimmed.substr(s);
        auto e = trimmed.find_last_not_of(" \t\n\r");
        if (e != std::string::npos) trimmed = trimmed.substr(0, e + 1);
        if (trimmed.empty()) continue;
        collect_markdown_files(fs::path(trimmed), found);
    }

    std::vector<std::string> sorted_files(found.begin(), found.end());
    std::sort(sorted_files.begin(), sorted_files.end());

    std::vector<GroundedRemFilePreview> previews;
    for (const auto& file_path : sorted_files) {
        try {
            std::ifstream ifs(file_path);
            std::string content((std::istreambuf_iterator<char>(ifs)),
                                std::istreambuf_iterator<char>());
            auto ws = fs::path(workspace_dir);
            auto rel = fs::relative(fs::path(file_path), ws).string();
            // Normalize path separators
            std::replace(rel.begin(), rel.end(), '\\', '/');
            if (rel.substr(0, 2) == "./") rel = rel.substr(2);
            previews.push_back(preview_grounded_rem_for_file(rel, content));
        } catch (...) {}
    }

    return {
        workspace_dir,
        static_cast<int>(sorted_files.size()),
        previews
    };
}

}  // namespace memory
}  // namespace hunterclaw
