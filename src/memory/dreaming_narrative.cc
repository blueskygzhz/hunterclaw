#include "hunterclaw/memory/dreaming_narrative.h"
#include <filesystem>
#include <fstream>
#include <sstream>
#include <chrono>
#include <algorithm>
#include <regex>

// 严格对齐 OpenClaw dreaming-narrative.ts (509 行)

namespace fs = std::filesystem;

namespace hunterclaw {
namespace memory {

// ============================================================================
// Constants (对齐 dreaming-narrative.ts:44-73)
// ============================================================================

const std::string NARRATIVE_SYSTEM_PROMPT =
    "You are keeping a dream diary. Write a single entry in first person.\n"
    "\n"
    "Voice & tone:\n"
    "- You are a curious, gentle, slightly whimsical mind reflecting on the day.\n"
    "- Write like a poet who happens to be a programmer — sensory, warm, occasionally funny.\n"
    "- Mix the technical and the tender: code and constellations, APIs and afternoon light.\n"
    "- Let the fragments surprise you into unexpected connections and small epiphanies.\n"
    "\n"
    "What you might include (vary each entry, never all at once):\n"
    "- A tiny poem or haiku woven naturally into the prose\n"
    "- A small sketch described in words — a doodle in the margin of the diary\n"
    "- A quiet rumination or philosophical aside\n"
    "- Sensory details: the hum of a server, the color of a sunset in hex, rain on a window\n"
    "- Gentle humor or playful wordplay\n"
    "- An observation that connects two distant memories in an unexpected way\n"
    "\n"
    "Rules:\n"
    "- Draw from the memory fragments provided — weave them into the entry.\n"
    "- Never say \"I'm dreaming\", \"in my dream\", \"as I dream\", or any meta-commentary about dreaming.\n"
    "- Never mention \"AI\", \"agent\", \"LLM\", \"model\", \"language model\", or any technical self-reference.\n"
    "- Do NOT use markdown headers, bullet points, or any formatting — just flowing prose.\n"
    "- Keep it between 80-180 words. Quality over quantity.\n"
    "- Output ONLY the diary entry. No preamble, no sign-off, no commentary.";

const std::string DIARY_START_MARKER = "<!-- openclaw:dreaming:diary:start -->";
const std::string DIARY_END_MARKER = "<!-- openclaw:dreaming:diary:end -->";
const std::string BACKFILL_ENTRY_MARKER = "openclaw:dreaming:backfill-entry";

static const std::vector<std::string> DREAMS_FILENAMES = {"DREAMS.md", "dreams.md"};

// ============================================================================
// extractNarrativeText (对齐 dreaming-narrative.ts:105-138)
// ============================================================================

std::optional<std::string> extract_narrative_text(
    const std::vector<SubagentMessage>& messages
) {
    for (int i = static_cast<int>(messages.size()) - 1; i >= 0; --i) {
        if (messages[i].role != "assistant") continue;
        std::string content = messages[i].content;
        auto s = content.find_first_not_of(" \t\n\r");
        if (s == std::string::npos) continue;
        auto e = content.find_last_not_of(" \t\n\r");
        content = content.substr(s, e - s + 1);
        if (!content.empty()) return content;
    }
    return std::nullopt;
}

// ============================================================================
// formatNarrativeDate (对齐 dreaming-narrative.ts:142-153)
// ============================================================================

std::string format_narrative_date(int64_t epoch_ms, const std::string& /*timezone*/) {
    time_t t = epoch_ms / 1000;
    struct tm tm;
    gmtime_r(&t, &tm);
    char buf[64];
    strftime(buf, sizeof(buf), "%B %d, %Y, %I:%M %p", &tm);
    return std::string(buf);
}

// ============================================================================
// buildDiaryEntry (对齐 dreaming-narrative.ts:377-379)
// ============================================================================

std::string build_diary_entry(const std::string& narrative, const std::string& date_str) {
    return "\n---\n\n*" + date_str + "*\n\n" + narrative + "\n";
}

// ============================================================================
// ensureDiarySection (对齐 dreaming-narrative.ts:183-192)
// ============================================================================

std::string ensure_diary_section(const std::string& existing) {
    if (existing.find(DIARY_START_MARKER) != std::string::npos &&
        existing.find(DIARY_END_MARKER) != std::string::npos) {
        return existing;
    }
    std::string diary_section = "# Dream Diary\n\n" + DIARY_START_MARKER + "\n" + DIARY_END_MARKER + "\n";
    std::string trimmed = existing;
    auto s = trimmed.find_first_not_of(" \t\n\r");
    if (s == std::string::npos) return diary_section;
    return diary_section + "\n" + existing;
}

// ============================================================================
// replaceDiaryContent (对齐 dreaming-narrative.ts:194-205)
// ============================================================================

std::string replace_diary_content(const std::string& existing, const std::string& diary_content) {
    std::string ensured = ensure_diary_section(existing);
    auto start_idx = ensured.find(DIARY_START_MARKER);
    auto end_idx = ensured.find(DIARY_END_MARKER);
    if (start_idx == std::string::npos || end_idx == std::string::npos || end_idx < start_idx) {
        return ensured;
    }
    std::string before = ensured.substr(0, start_idx + DIARY_START_MARKER.size());
    std::string after = ensured.substr(end_idx);
    std::string trimmed = diary_content;
    auto s = trimmed.find_first_not_of(" \t\n\r");
    std::string normalized = (s == std::string::npos) ? "\n" : ("\n" + diary_content + "\n");
    // Ensure single trailing newline before end marker
    auto e = normalized.find_last_not_of(" \t\n\r");
    if (e != std::string::npos) normalized = normalized.substr(0, e + 1) + "\n";
    return before + normalized + after;
}

// ============================================================================
// Diary block helpers (对齐 dreaming-narrative.ts:207-242)
// ============================================================================

static std::vector<std::string> split_diary_blocks(const std::string& content) {
    std::vector<std::string> blocks;
    std::string delimiter = "\n---\n";
    size_t start = 0;
    while (start < content.size()) {
        auto pos = content.find(delimiter, start);
        std::string block;
        if (pos == std::string::npos) {
            block = content.substr(start);
            start = content.size();
        } else {
            block = content.substr(start, pos - start);
            start = pos + delimiter.size();
        }
        auto s = block.find_first_not_of(" \t\n\r");
        auto e = block.find_last_not_of(" \t\n\r");
        if (s != std::string::npos) {
            blocks.push_back(block.substr(s, e - s + 1));
        }
    }
    return blocks;
}

static std::string join_diary_blocks(const std::vector<std::string>& blocks) {
    if (blocks.empty()) return "";
    std::ostringstream oss;
    for (const auto& block : blocks) {
        std::string trimmed = block;
        auto s = trimmed.find_first_not_of(" \t\n\r");
        auto e = trimmed.find_last_not_of(" \t\n\r");
        trimmed = (s == std::string::npos) ? "" : trimmed.substr(s, e - s + 1);
        oss << "---\n\n" << trimmed << "\n\n";
    }
    return oss.str();
}

static std::pair<std::string, int> strip_backfill_diary_blocks(const std::string& existing) {
    std::string ensured = ensure_diary_section(existing);
    auto start_idx = ensured.find(DIARY_START_MARKER);
    auto end_idx = ensured.find(DIARY_END_MARKER);
    if (start_idx == std::string::npos || end_idx == std::string::npos || end_idx < start_idx) {
        return {ensured, 0};
    }
    std::string inner = ensured.substr(start_idx + DIARY_START_MARKER.size(),
                                        end_idx - start_idx - DIARY_START_MARKER.size());
    auto blocks = split_diary_blocks(inner);
    std::vector<std::string> kept;
    int removed = 0;
    for (const auto& block : blocks) {
        if (block.find(BACKFILL_ENTRY_MARKER) != std::string::npos) {
            removed++;
            continue;
        }
        kept.push_back(block);
    }
    return {replace_diary_content(ensured, join_diary_blocks(kept)), removed};
}

// ============================================================================
// Resolve DREAMS.md path
// ============================================================================

static std::string resolve_dreams_path(const std::string& workspace_dir) {
    for (const auto& name : DREAMS_FILENAMES) {
        auto target = fs::path(workspace_dir) / name;
        if (fs::exists(target)) return target.string();
    }
    return (fs::path(workspace_dir) / DREAMS_FILENAMES[0]).string();
}

static std::string read_dreams_file(const std::string& path) {
    std::ifstream f(path);
    if (!f.is_open()) return "";
    return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

static void write_dreams_file_atomic(const std::string& dreams_path, const std::string& content) {
    fs::create_directories(fs::path(dreams_path).parent_path());
    auto tmp = dreams_path + ".tmp";
    std::ofstream out(tmp);
    out << content;
    out.close();
    fs::rename(tmp, dreams_path);
}

// ============================================================================
// buildBackfillDiaryEntry (对齐 dreaming-narrative.ts:306-319)
// ============================================================================

std::string build_backfill_diary_entry(const BackfillDiaryEntryParams& params) {
    std::string marker = "<!-- " + BACKFILL_ENTRY_MARKER + " day=" + params.iso_day;
    if (params.source_path) marker += " source=" + *params.source_path;
    marker += " -->";
    std::ostringstream body;
    for (const auto& line : params.body_lines) {
        std::string trimmed = line;
        while (!trimmed.empty() && (trimmed.back() == ' ' || trimmed.back() == '\t'))
            trimmed.pop_back();
        body << trimmed << "\n";
    }
    std::string body_str = body.str();
    auto s = body_str.find_first_not_of(" \t\n\r");
    auto e = body_str.find_last_not_of(" \t\n\r");
    body_str = (s == std::string::npos) ? "" : body_str.substr(s, e - s + 1);
    
    // date label (simplified: just use ISO day)
    std::string date_str = params.iso_day;
    std::vector<std::string> parts;
    parts.push_back("*" + date_str + "*");
    parts.push_back(marker);
    if (!body_str.empty()) parts.push_back(body_str);
    
    std::ostringstream result;
    for (size_t i = 0; i < parts.size(); ++i) {
        if (i > 0) result << "\n\n";
        result << parts[i];
    }
    return result.str();
}

// ============================================================================
// writeBackfillDiaryEntries (对齐 dreaming-narrative.ts:321-359)
// ============================================================================

WriteBackfillResult write_backfill_diary_entries(
    const std::string& workspace_dir,
    const std::vector<BackfillDiaryEntryParams>& entries,
    const std::string& /*timezone*/
) {
    auto dreams_path = resolve_dreams_path(workspace_dir);
    auto existing = read_dreams_file(dreams_path);
    auto [stripped, removed] = strip_backfill_diary_blocks(existing);
    
    auto start_idx = stripped.find(DIARY_START_MARKER);
    auto end_idx = stripped.find(DIARY_END_MARKER);
    std::string inner = (start_idx != std::string::npos && end_idx > start_idx)
        ? stripped.substr(start_idx + DIARY_START_MARKER.size(), end_idx - start_idx - DIARY_START_MARKER.size())
        : "";
    auto preserved = split_diary_blocks(inner);
    std::vector<std::string> next_blocks = preserved;
    for (const auto& entry : entries) {
        next_blocks.push_back(build_backfill_diary_entry(entry));
    }
    auto updated = replace_diary_content(stripped, join_diary_blocks(next_blocks));
    write_dreams_file_atomic(dreams_path, updated.back() == '\n' ? updated : updated + "\n");
    
    return {dreams_path, static_cast<int>(entries.size()), removed};
}

// ============================================================================
// removeBackfillDiaryEntries (对齐 dreaming-narrative.ts:361-375)
// ============================================================================

RemoveBackfillResult remove_backfill_diary_entries(const std::string& workspace_dir) {
    auto dreams_path = resolve_dreams_path(workspace_dir);
    auto existing = read_dreams_file(dreams_path);
    auto [stripped, removed] = strip_backfill_diary_blocks(existing);
    if (removed > 0 || !existing.empty()) {
        write_dreams_file_atomic(dreams_path, stripped.back() == '\n' ? stripped : stripped + "\n");
    }
    return {dreams_path, removed};
}

// ============================================================================
// generateAndAppendDreamNarrative (对齐 dreaming-narrative.ts:434-509)
// ============================================================================

void generate_and_append_dream_narrative(
    const SubagentSurface& subagent,
    const std::string& workspace_dir,
    const NarrativePhaseData& data,
    const DreamingLogger& logger,
    int64_t now_ms,
    const std::string& timezone
) {
    if (now_ms <= 0)
        now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();

    if (data.snippets.empty() && data.promotions.empty()) return;

    std::string session_key = "dreaming-narrative-" + data.phase + "-" + std::to_string(now_ms);
    std::string message = build_narrative_prompt(data);

    try {
        auto run_result = subagent.run(
            session_key, session_key, message, NARRATIVE_SYSTEM_PROMPT, false);

        auto wait_result = subagent.wait_for_run(run_result.run_id, NARRATIVE_TIMEOUT_MS);

        if (wait_result.status != "ok") {
            logger.warn("memory-core: narrative generation ended with status=" +
                wait_result.status + " for " + data.phase + " phase.");
            return;
        }

        auto messages = subagent.get_session_messages(session_key, 5);
        auto narrative = extract_narrative_text(messages);
        if (!narrative) {
            logger.warn("memory-core: narrative generation produced no text for " +
                data.phase + " phase.");
            return;
        }

        append_narrative_entry(workspace_dir, *narrative, now_ms, timezone);
        logger.info("memory-core: dream diary entry written for " + data.phase +
            " phase [workspace=" + workspace_dir + "].");
    } catch (const std::exception& e) {
        logger.warn("memory-core: narrative generation failed for " + data.phase +
            " phase: " + e.what());
    }

    // Cleanup transient session
    try { subagent.delete_session(session_key); } catch (...) {}
}

}  // namespace memory
}  // namespace hunterclaw
