// ============================================================================
// apply_patch.cc
// ✅ 严格对齐 OpenClaw: src/agents/apply-patch.ts (608行)
//                      + src/agents/apply-patch-update.ts (206行)
//
// Patch 解析与应用系统：
//   - parsePatchText: 解析 *** Begin Patch / *** End Patch 格式
//   - applyPatch: 应用 add/delete/update hunks
//   - applyUpdateHunk: context-aware hunk 应用 (seekSequence + fuzzy match)
//   - normalizePunctuation: 全角→半角标点归一化
// ============================================================================

#include <string>
#include <vector>
#include <set>
#include <optional>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <algorithm>
#include <functional>
#include <stdexcept>
#include <spdlog/spdlog.h>

namespace hunterclaw {
namespace agents {

namespace {
auto log() { return spdlog::get("apply-patch"); }
} // namespace

// ============================================================================
// 对齐 apply-patch.ts:18-26: Markers
// ============================================================================
static const std::string BEGIN_PATCH_MARKER = "*** Begin Patch";
static const std::string END_PATCH_MARKER = "*** End Patch";
static const std::string ADD_FILE_MARKER = "*** Add File: ";
static const std::string DELETE_FILE_MARKER = "*** Delete File: ";
static const std::string UPDATE_FILE_MARKER = "*** Update File: ";
static const std::string MOVE_TO_MARKER = "*** Move to: ";
static const std::string EOF_MARKER = "*** End of File";
static const std::string CHANGE_CONTEXT_MARKER = "@@ ";
static const std::string EMPTY_CHANGE_CONTEXT_MARKER = "@@";

// ============================================================================
// 对齐 apply-patch.ts:28-68: Types
// ============================================================================

struct UpdateFileChunk {
    std::optional<std::string> change_context;
    std::vector<std::string> old_lines;
    std::vector<std::string> new_lines;
    bool is_end_of_file = false;
};

struct AddFileHunk {
    std::string path;
    std::string contents;
};

struct DeleteFileHunk {
    std::string path;
};

struct UpdateFileHunk {
    std::string path;
    std::optional<std::string> move_path;
    std::vector<UpdateFileChunk> chunks;
};

enum class HunkKind { Add, Delete, Update };

struct Hunk {
    HunkKind kind;
    AddFileHunk add;
    DeleteFileHunk del;
    UpdateFileHunk update;
};

struct ApplyPatchSummary {
    std::vector<std::string> added;
    std::vector<std::string> modified;
    std::vector<std::string> deleted;
};

struct ApplyPatchResult {
    ApplyPatchSummary summary;
    std::string text;
};

struct ParsedPatch {
    std::vector<Hunk> hunks;
};

// ============================================================================
// 对齐 apply-patch-update.ts:164-196: normalizePunctuation
// ============================================================================
static std::string normalize_punctuation(const std::string& value) {
    std::string result;
    result.reserve(value.size());
    // UTF-8 aware iteration for common full-width chars
    size_t i = 0;
    while (i < value.size()) {
        unsigned char c = value[i];
        if (c < 0x80) {
            result += c;
            ++i;
            continue;
        }
        // 3-byte UTF-8 sequences (U+2010-U+201F, U+00A0, U+2002-U+2009, U+2212)
        if (c == 0xE2 && i + 2 < value.size()) {
            unsigned char c1 = value[i+1], c2 = value[i+2];
            // U+2010-U+2015 (dashes) → '-'
            if (c1 == 0x80 && c2 >= 0x90 && c2 <= 0x95) { result += '-'; i += 3; continue; }
            // U+2212 (minus) → '-'
            if (c1 == 0x88 && c2 == 0x92) { result += '-'; i += 3; continue; }
            // U+2018-U+201B (single quotes) → '\''
            if (c1 == 0x80 && c2 >= 0x98 && c2 <= 0x9B) { result += '\''; i += 3; continue; }
            // U+201C-U+201F (double quotes) → '"'
            if (c1 == 0x80 && c2 >= 0x9C && c2 <= 0x9F) { result += '"'; i += 3; continue; }
            // U+2002-U+2009 (various spaces) → ' '
            if (c1 == 0x80 && c2 >= 0x82 && c2 <= 0x89) { result += ' '; i += 3; continue; }
        }
        // U+00A0 (NBSP) → ' '
        if (c == 0xC2 && i + 1 < value.size() && (unsigned char)value[i+1] == 0xA0) {
            result += ' '; i += 2; continue;
        }
        // Pass through other multi-byte chars as-is
        int len = 1;
        if (c >= 0xC0 && c < 0xE0) len = 2;
        else if (c >= 0xE0 && c < 0xF0) len = 3;
        else if (c >= 0xF0) len = 4;
        for (int j = 0; j < len && i < value.size(); ++j) result += value[i++];
    }
    return result;
}

// ============================================================================
// 对齐 apply-patch-update.ts:150-162: linesMatch
// ============================================================================
static bool lines_match(
    const std::vector<std::string>& lines,
    const std::vector<std::string>& pattern,
    size_t start,
    std::function<std::string(const std::string&)> normalize
) {
    for (size_t idx = 0; idx < pattern.size(); ++idx) {
        if (start + idx >= lines.size()) return false;
        if (normalize(lines[start + idx]) != normalize(pattern[idx])) return false;
    }
    return true;
}

// ============================================================================
// 对齐 apply-patch-update.ts:107-148: seekSequence
// 4-pass fuzzy matching: exact → trimEnd → trim → normalize punctuation
// ============================================================================
static std::optional<size_t> seek_sequence(
    const std::vector<std::string>& lines,
    const std::vector<std::string>& pattern,
    size_t start,
    bool eof
) {
    if (pattern.empty()) return start;
    if (pattern.size() > lines.size()) return std::nullopt;

    size_t max_start = lines.size() - pattern.size();
    size_t search_start = (eof && lines.size() >= pattern.size()) ? max_start : start;
    if (search_start > max_start) return std::nullopt;

    // Helper lambdas matching OC's 4 passes
    auto identity = [](const std::string& s) { return s; };
    auto trim_end = [](const std::string& s) {
        auto e = s.find_last_not_of(" \t\r\n");
        return (e == std::string::npos) ? "" : s.substr(0, e + 1);
    };
    auto trim_both = [](const std::string& s) {
        auto b = s.find_first_not_of(" \t\r\n");
        if (b == std::string::npos) return std::string();
        auto e = s.find_last_not_of(" \t\r\n");
        return s.substr(b, e - b + 1);
    };
    auto norm_punct = [&trim_both](const std::string& s) {
        return normalize_punctuation(trim_both(s));
    };

    // Pass 1: exact
    for (size_t i = search_start; i <= max_start; ++i)
        if (lines_match(lines, pattern, i, identity)) return i;
    // Pass 2: trimEnd
    for (size_t i = search_start; i <= max_start; ++i)
        if (lines_match(lines, pattern, i, trim_end)) return i;
    // Pass 3: trim
    for (size_t i = search_start; i <= max_start; ++i)
        if (lines_match(lines, pattern, i, trim_both)) return i;
    // Pass 4: normalize punctuation
    for (size_t i = search_start; i <= max_start; ++i)
        if (lines_match(lines, pattern, i, norm_punct)) return i;

    return std::nullopt;
}

// ============================================================================
// 对齐 apply-patch-update.ts:89-105: applyReplacements
// ============================================================================
static std::vector<std::string> apply_replacements(
    const std::vector<std::string>& lines,
    std::vector<std::tuple<size_t, size_t, std::vector<std::string>>>& replacements
) {
    // Sort by start index (ascending) then apply in reverse
    std::sort(replacements.begin(), replacements.end(),
              [](const auto& a, const auto& b) { return std::get<0>(a) < std::get<0>(b); });

    std::vector<std::string> result = lines;
    for (auto it = replacements.rbegin(); it != replacements.rend(); ++it) {
        auto [start_idx, old_len, new_lines] = *it;
        // Remove old lines
        size_t actual_remove = std::min(old_len, result.size() - std::min(start_idx, result.size()));
        result.erase(result.begin() + start_idx,
                     result.begin() + start_idx + actual_remove);
        // Insert new lines
        result.insert(result.begin() + start_idx, new_lines.begin(), new_lines.end());
    }
    return result;
}

// ============================================================================
// 对齐 apply-patch-update.ts:37-87: computeReplacements
// ============================================================================
static std::vector<std::tuple<size_t, size_t, std::vector<std::string>>>
compute_replacements(
    const std::vector<std::string>& original_lines,
    const std::string& file_path,
    const std::vector<UpdateFileChunk>& chunks
) {
    std::vector<std::tuple<size_t, size_t, std::vector<std::string>>> replacements;
    size_t line_index = 0;

    for (const auto& chunk : chunks) {
        if (chunk.change_context.has_value()) {
            auto ctx_idx = seek_sequence(original_lines, {*chunk.change_context}, line_index, false);
            if (!ctx_idx.has_value()) {
                throw std::runtime_error("Failed to find context '" + *chunk.change_context + "' in " + file_path);
            }
            line_index = *ctx_idx + 1;
        }

        if (chunk.old_lines.empty()) {
            size_t insertion_idx = (!original_lines.empty() && original_lines.back().empty())
                ? original_lines.size() - 1 : original_lines.size();
            replacements.emplace_back(insertion_idx, size_t(0), chunk.new_lines);
            continue;
        }

        auto pattern = chunk.old_lines;
        auto new_slice = chunk.new_lines;
        auto found = seek_sequence(original_lines, pattern, line_index, chunk.is_end_of_file);

        // Retry without trailing empty line
        if (!found.has_value() && !pattern.empty() && pattern.back().empty()) {
            pattern.pop_back();
            if (!new_slice.empty() && new_slice.back().empty()) {
                new_slice.pop_back();
            }
            found = seek_sequence(original_lines, pattern, line_index, chunk.is_end_of_file);
        }

        if (!found.has_value()) {
            std::string preview;
            for (size_t i = 0; i < std::min(size_t(3), chunk.old_lines.size()); ++i) {
                preview += chunk.old_lines[i] + "\n";
            }
            throw std::runtime_error("Failed to find expected lines in " + file_path + ":\n" + preview);
        }

        replacements.emplace_back(*found, pattern.size(), new_slice);
        line_index = *found + pattern.size();
    }

    std::sort(replacements.begin(), replacements.end(),
              [](const auto& a, const auto& b) { return std::get<0>(a) < std::get<0>(b); });
    return replacements;
}

// ============================================================================
// 对齐 apply-patch-update.ts:14-34: applyUpdateHunk
// ============================================================================
std::string apply_update_hunk(
    const std::string& file_path,
    const std::vector<UpdateFileChunk>& chunks,
    std::function<std::string(const std::string&)> read_file_fn
) {
    std::string original_contents;
    if (read_file_fn) {
        original_contents = read_file_fn(file_path);
    } else {
        std::ifstream f(file_path);
        if (!f.is_open()) throw std::runtime_error("Failed to read file to update " + file_path);
        original_contents = std::string((std::istreambuf_iterator<char>(f)),
                                        std::istreambuf_iterator<char>());
    }

    // Split into lines
    std::vector<std::string> original_lines;
    std::istringstream iss(original_contents);
    std::string line;
    while (std::getline(iss, line)) {
        original_lines.push_back(line);
    }
    // Remove trailing empty line (matching OC)
    if (!original_lines.empty() && original_lines.back().empty()) {
        original_lines.pop_back();
    }

    auto replacements = compute_replacements(original_lines, file_path, chunks);
    auto new_lines = apply_replacements(original_lines, replacements);

    // Ensure trailing newline
    if (new_lines.empty() || !new_lines.back().empty()) {
        new_lines.push_back("");
    }

    std::string result;
    for (size_t i = 0; i < new_lines.size(); ++i) {
        if (i > 0) result += '\n';
        result += new_lines[i];
    }
    return result;
}

// ============================================================================
// Utility: split string by delimiter
// ============================================================================
static std::vector<std::string> split_lines(const std::string& text) {
    std::vector<std::string> lines;
    std::istringstream iss(text);
    std::string line;
    while (std::getline(iss, line)) {
        // Remove trailing \r
        if (!line.empty() && line.back() == '\r') line.pop_back();
        lines.push_back(line);
    }
    return lines;
}

static bool starts_with(const std::string& s, const std::string& prefix) {
    return s.size() >= prefix.size() && s.compare(0, prefix.size(), prefix) == 0;
}

// ============================================================================
// 对齐 apply-patch.ts:parsePatchText (simplified from L296-L420)
// ============================================================================
ParsedPatch parse_patch_text(const std::string& input) {
    ParsedPatch result;
    auto lines = split_lines(input);

    // Find *** Begin Patch
    size_t i = 0;
    while (i < lines.size() && lines[i].find(BEGIN_PATCH_MARKER) == std::string::npos) ++i;
    if (i >= lines.size()) return result;
    ++i; // skip begin marker

    while (i < lines.size()) {
        const auto& line = lines[i];

        if (starts_with(line, END_PATCH_MARKER)) break;

        if (starts_with(line, ADD_FILE_MARKER)) {
            Hunk hunk;
            hunk.kind = HunkKind::Add;
            hunk.add.path = line.substr(ADD_FILE_MARKER.size());
            ++i;
            std::string contents;
            while (i < lines.size() &&
                   !starts_with(lines[i], ADD_FILE_MARKER) &&
                   !starts_with(lines[i], DELETE_FILE_MARKER) &&
                   !starts_with(lines[i], UPDATE_FILE_MARKER) &&
                   !starts_with(lines[i], END_PATCH_MARKER)) {
                if (!contents.empty()) contents += '\n';
                contents += lines[i];
                ++i;
            }
            hunk.add.contents = contents;
            result.hunks.push_back(std::move(hunk));
            continue;
        }

        if (starts_with(line, DELETE_FILE_MARKER)) {
            Hunk hunk;
            hunk.kind = HunkKind::Delete;
            hunk.del.path = line.substr(DELETE_FILE_MARKER.size());
            result.hunks.push_back(std::move(hunk));
            ++i;
            continue;
        }

        if (starts_with(line, UPDATE_FILE_MARKER)) {
            Hunk hunk;
            hunk.kind = HunkKind::Update;
            hunk.update.path = line.substr(UPDATE_FILE_MARKER.size());
            ++i;

            // Check for move target
            if (i < lines.size() && starts_with(lines[i], MOVE_TO_MARKER)) {
                hunk.update.move_path = lines[i].substr(MOVE_TO_MARKER.size());
                ++i;
            }

            // Parse chunks
            UpdateFileChunk current_chunk;
            bool in_old = true; // true = reading old lines (- prefix), false = new lines (+ prefix)
            bool has_chunk = false;

            while (i < lines.size() &&
                   !starts_with(lines[i], ADD_FILE_MARKER) &&
                   !starts_with(lines[i], DELETE_FILE_MARKER) &&
                   !starts_with(lines[i], UPDATE_FILE_MARKER) &&
                   !starts_with(lines[i], END_PATCH_MARKER)) {

                const auto& cl = lines[i];

                if (starts_with(cl, CHANGE_CONTEXT_MARKER) || cl == EMPTY_CHANGE_CONTEXT_MARKER) {
                    // Flush previous chunk
                    if (has_chunk) {
                        hunk.update.chunks.push_back(std::move(current_chunk));
                        current_chunk = UpdateFileChunk{};
                    }
                    if (starts_with(cl, CHANGE_CONTEXT_MARKER)) {
                        current_chunk.change_context = cl.substr(CHANGE_CONTEXT_MARKER.size());
                    }
                    has_chunk = true;
                    in_old = true;
                    ++i;
                    continue;
                }

                if (cl == EOF_MARKER) {
                    current_chunk.is_end_of_file = true;
                    ++i;
                    continue;
                }

                has_chunk = true;
                if (!cl.empty() && cl[0] == '-') {
                    current_chunk.old_lines.push_back(cl.substr(1));
                    in_old = true;
                } else if (!cl.empty() && cl[0] == '+') {
                    current_chunk.new_lines.push_back(cl.substr(1));
                    in_old = false;
                } else if (!cl.empty() && cl[0] == ' ') {
                    // Context line — goes to both old and new
                    current_chunk.old_lines.push_back(cl.substr(1));
                    current_chunk.new_lines.push_back(cl.substr(1));
                } else {
                    // Bare line (no prefix) — treat as context
                    current_chunk.old_lines.push_back(cl);
                    current_chunk.new_lines.push_back(cl);
                }
                ++i;
            }

            if (has_chunk) {
                hunk.update.chunks.push_back(std::move(current_chunk));
            }
            result.hunks.push_back(std::move(hunk));
            continue;
        }

        ++i; // skip unrecognized lines
    }

    return result;
}

// ============================================================================
// 对齐 apply-patch.ts:129-200: applyPatch
// ============================================================================
ApplyPatchResult apply_patch(
    const std::string& input,
    const std::string& cwd,
    bool workspace_only
) {
    auto parsed = parse_patch_text(input);
    if (parsed.hunks.empty()) {
        throw std::runtime_error("No files were modified.");
    }

    ApplyPatchSummary summary;
    std::set<std::string> seen_added, seen_modified, seen_deleted;
    namespace fs = std::filesystem;

    for (const auto& hunk : parsed.hunks) {
        if (hunk.kind == HunkKind::Add) {
            fs::path target = fs::path(cwd) / hunk.add.path;
            if (workspace_only) {
                auto canonical_cwd = fs::weakly_canonical(cwd);
                auto canonical_target = fs::weakly_canonical(target);
                auto rel = canonical_target.string().find(canonical_cwd.string());
                if (rel != 0) {
                    throw std::runtime_error("Path escapes workspace: " + hunk.add.path);
                }
            }
            fs::create_directories(target.parent_path());
            std::ofstream f(target);
            f << hunk.add.contents;
            if (seen_added.insert(hunk.add.path).second) {
                summary.added.push_back(hunk.add.path);
            }
        }

        else if (hunk.kind == HunkKind::Delete) {
            fs::path target = fs::path(cwd) / hunk.del.path;
            if (fs::exists(target)) {
                fs::remove(target);
            }
            if (seen_deleted.insert(hunk.del.path).second) {
                summary.deleted.push_back(hunk.del.path);
            }
        }

        else if (hunk.kind == HunkKind::Update) {
            fs::path target = fs::path(cwd) / hunk.update.path;
            std::string new_contents = apply_update_hunk(target.string(), hunk.update.chunks, nullptr);

            // Handle move
            fs::path write_target = target;
            if (hunk.update.move_path.has_value()) {
                write_target = fs::path(cwd) / *hunk.update.move_path;
                fs::create_directories(write_target.parent_path());
                if (fs::exists(target)) fs::remove(target);
            }

            std::ofstream f(write_target);
            f << new_contents;
            std::string display = hunk.update.move_path.value_or(hunk.update.path);
            if (seen_modified.insert(display).second) {
                summary.modified.push_back(display);
            }
        }
    }

    // Build result text
    std::string text;
    if (!summary.added.empty()) {
        text += "Added: ";
        for (size_t i = 0; i < summary.added.size(); ++i) {
            if (i > 0) text += ", ";
            text += summary.added[i];
        }
        text += "\n";
    }
    if (!summary.modified.empty()) {
        text += "Modified: ";
        for (size_t i = 0; i < summary.modified.size(); ++i) {
            if (i > 0) text += ", ";
            text += summary.modified[i];
        }
        text += "\n";
    }
    if (!summary.deleted.empty()) {
        text += "Deleted: ";
        for (size_t i = 0; i < summary.deleted.size(); ++i) {
            if (i > 0) text += ", ";
            text += summary.deleted[i];
        }
        text += "\n";
    }
    if (text.empty()) text = "Patch applied (no changes).";

    return {summary, text};
}

} // namespace agents
} // namespace hunterclaw
