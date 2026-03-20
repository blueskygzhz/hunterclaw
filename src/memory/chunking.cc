#include "hunterclaw/memory/chunking.h"
#include "hunterclaw/memory/multimodal.h"
#include <algorithm>
#include <cctype>
#include <fstream>
#include <sstream>
#include <filesystem>
#include <functional>
#include <openssl/sha.h>
#include <iomanip>
#include <map>
#include <cmath>
#include <set>

namespace fs = std::filesystem;

namespace hunterclaw {
namespace memory {

// ============================================================================
// Helper Functions
// ============================================================================

static std::vector<std::string> split_by_newline(const std::string& text) {
    std::vector<std::string> lines;
    std::istringstream stream(text);
    std::string line;
    
    while (std::getline(stream, line)) {
        lines.push_back(line);
    }
    
    return lines;
}

static std::string trim(const std::string& str) {
    auto start = std::find_if_not(str.begin(), str.end(), [](unsigned char ch) {
        return std::isspace(ch);
    });
    auto end = std::find_if_not(str.rbegin(), str.rend(), [](unsigned char ch) {
        return std::isspace(ch);
    }).base();
    
    return (start < end) ? std::string(start, end) : "";
}

// ============================================================================
// Basic Chunking Functions
// ============================================================================

std::vector<std::string> chunk_by_length(const std::string& text, int max_length) {
    if (text.empty() || max_length <= 0) {
        return {};
    }
    
    if (text.length() <= static_cast<size_t>(max_length)) {
        return {text};
    }
    
    std::vector<std::string> chunks;
    size_t pos = 0;
    
    while (pos < text.length()) {
        size_t chunk_size = std::min(static_cast<size_t>(max_length), text.length() - pos);
        chunks.push_back(text.substr(pos, chunk_size));
        pos += chunk_size;
    }
    
    return chunks;
}

std::vector<std::string> chunk_by_paragraph(
    const std::string& text,
    int limit,
    bool split_long_paragraphs) {
    
    if (text.empty() || limit <= 0) {
        return text.empty() ? std::vector<std::string>{} : std::vector<std::string>{text};
    }
    
    // Normalize line endings
    std::string normalized = text;
    std::replace(normalized.begin(), normalized.end(), '\r', '\n');
    
    // Detect paragraph boundaries (blank lines)
    std::regex paragraph_re(R"(\n[\t ]*\n+)");
    
    // Fast path: no paragraph separators
    if (!std::regex_search(normalized, paragraph_re)) {
        if (normalized.length() <= static_cast<size_t>(limit)) {
            return {normalized};
        }
        // Single long paragraph
        if (split_long_paragraphs) {
            return chunk_by_length(normalized, limit);
        }
        return {normalized};
    }
    
    // Split into paragraphs
    std::vector<std::string> paragraphs;
    std::sregex_token_iterator iter(normalized.begin(), normalized.end(), paragraph_re, -1);
    std::sregex_token_iterator end;
    
    for (; iter != end; ++iter) {
        std::string para = *iter;
        if (!para.empty()) {
            paragraphs.push_back(para);
        }
    }
    
    // Pack paragraphs into chunks
    std::vector<std::string> chunks;
    std::string current_chunk;
    
    for (const auto& para : paragraphs) {
        // If paragraph itself exceeds limit
        if (para.length() > static_cast<size_t>(limit)) {
            // Flush current chunk
            if (!current_chunk.empty()) {
                chunks.push_back(current_chunk);
                current_chunk.clear();
            }
            
            // Handle long paragraph
            if (split_long_paragraphs) {
                auto para_chunks = chunk_by_length(para, limit);
                chunks.insert(chunks.end(), para_chunks.begin(), para_chunks.end());
            } else {
                chunks.push_back(para);
            }
            continue;
        }
        
        // Try to add to current chunk
        std::string test_chunk = current_chunk.empty() 
            ? para 
            : current_chunk + "\n\n" + para;
        
        if (test_chunk.length() <= static_cast<size_t>(limit)) {
            current_chunk = test_chunk;
        } else {
            // Flush and start new chunk
            if (!current_chunk.empty()) {
                chunks.push_back(current_chunk);
            }
            current_chunk = para;
        }
    }
    
    if (!current_chunk.empty()) {
        chunks.push_back(current_chunk);
    }
    
    return chunks.empty() ? std::vector<std::string>{text} : chunks;
}

std::vector<std::string> chunk_by_newline(
    const std::string& text,
    int max_line_length,
    bool split_long_lines,
    bool trim_lines) {
    
    if (text.empty() || max_line_length <= 0) {
        return text.empty() ? std::vector<std::string>{} : std::vector<std::string>{text};
    }
    
    auto lines = split_by_newline(text);
    std::vector<std::string> chunks;
    int pending_blank_lines = 0;
    
    for (const auto& line : lines) {
        std::string trimmed = trim(line);
        
        if (trimmed.empty()) {
            pending_blank_lines++;
            continue;
        }
        
        // Add blank line prefix
        std::string prefix;
        if (pending_blank_lines > 0) {
            int max_prefix = std::max(0, max_line_length - 1);
            int capped_blanks = std::min(pending_blank_lines, max_prefix);
            prefix = std::string(capped_blanks, '\n');
            pending_blank_lines = 0;
        }
        
        std::string line_value = trim_lines ? trimmed : line;
        
        if (!split_long_lines || line_value.length() + prefix.length() <= static_cast<size_t>(max_line_length)) {
            chunks.push_back(prefix + line_value);
            continue;
        }
        
        // Split long line
        int first_limit = std::max(1, max_line_length - static_cast<int>(prefix.length()));
        std::string first = line_value.substr(0, first_limit);
        chunks.push_back(prefix + first);
        
        std::string remaining = line_value.substr(first_limit);
        if (!remaining.empty()) {
            auto remaining_chunks = chunk_by_length(remaining, max_line_length);
            chunks.insert(chunks.end(), remaining_chunks.begin(), remaining_chunks.end());
        }
    }
    
    // Append trailing blank lines
    if (pending_blank_lines > 0 && !chunks.empty()) {
        chunks.back() += std::string(pending_blank_lines, '\n');
    }
    
    return chunks;
}

// ============================================================================
// Smart Chunking with Markdown/Code Awareness
// ============================================================================

std::vector<std::string> chunk_text_smart(
    const std::string& text,
    const ChunkOptions& options) {
    
    switch (options.mode) {
        case ChunkMode::Paragraph:
            return chunk_by_paragraph(text, options.max_chunk_size, options.split_long_paragraphs);
        
        case ChunkMode::Newline:
            return chunk_by_newline(text, options.max_chunk_size, true, options.trim_lines);
        
        case ChunkMode::Length:
        default:
            return chunk_by_length(text, options.max_chunk_size);
    }
}

// ============================================================================
// Markdown/Code Chunking
// ============================================================================

std::string hash_text(const std::string& text) {
    unsigned char digest[SHA256_DIGEST_LENGTH];
    SHA256(reinterpret_cast<const unsigned char*>(text.data()), text.size(), digest);
    std::ostringstream oss;
    for (int i = 0; i < SHA256_DIGEST_LENGTH; ++i) {
        oss << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(digest[i]);
    }
    return oss.str();
}

std::vector<FenceSpan> parse_fence_spans(const std::string& text) {
    std::vector<FenceSpan> spans;
    auto lines = split_by_newline(text);
    
    std::regex fence_start_re(R"(^[\t ]*(```|~~~)(\w*))");
    FenceSpan* current_fence = nullptr;
    
    for (size_t i = 0; i < lines.size(); ++i) {
        std::smatch match;
        if (std::regex_search(lines[i], match, fence_start_re)) {
            if (!current_fence) {
                // Start new fence
                FenceSpan span;
                span.start_line = static_cast<int>(i + 1);
                span.fence_marker = match[1].str();
                span.lang = match[2].str();
                spans.push_back(span);
                current_fence = &spans.back();
            } else {
                // End current fence
                current_fence->end_line = static_cast<int>(i + 1);
                current_fence = nullptr;
            }
        }
    }
    
    // Close unclosed fence
    if (current_fence) {
        current_fence->end_line = static_cast<int>(lines.size());
    }
    
    return spans;
}

bool is_safe_fence_break(const std::string& text, int position) {
    auto fences = parse_fence_spans(text);
    
    // Count lines before position
    int line = 1;
    for (int i = 0; i < position && i < static_cast<int>(text.length()); ++i) {
        if (text[i] == '\n') {
            line++;
        }
    }
    
    // Check if line is inside any fence
    for (const auto& fence : fences) {
        if (line >= fence.start_line && line <= fence.end_line) {
            return false;
        }
    }
    
    return true;
}

std::vector<MemoryChunk> chunk_markdown(
    const std::string& content,
    const ChunkingConfig& chunking) {

    if (content.empty()) {
        return {};
    }

    const int max_chars = std::max(32, chunking.tokens * 4);
    const int overlap_chars = std::max(0, chunking.overlap * 4);

    struct LineEntry {
        std::string line;
        int line_no;  // 1-indexed
    };

    std::vector<MemoryChunk> chunks;
    std::vector<LineEntry> current;
    int current_chars = 0;

    // flush: 将 current 中的内容生成一个 chunk
    auto flush = [&]() {
        if (current.empty()) return;
        const auto& first = current.front();
        const auto& last = current.back();
        std::ostringstream text_ss;
        for (size_t i = 0; i < current.size(); ++i) {
            if (i > 0) text_ss << "\n";
            text_ss << current[i].line;
        }
        std::string text = text_ss.str();
        MemoryChunk chunk;
        chunk.start_line = first.line_no;
        chunk.end_line = last.line_no;
        chunk.text = text;
        chunk.hash = hash_text(text);
        chunk.embedding_input = build_text_embedding_input(text);
        chunks.push_back(std::move(chunk));
    };

    // carryOverlap: 保留末尾 overlapChars 的内容
    auto carry_overlap = [&]() {
        if (overlap_chars <= 0 || current.empty()) {
            current.clear();
            current_chars = 0;
            return;
        }
        int acc = 0;
        std::vector<LineEntry> kept;
        for (int i = static_cast<int>(current.size()) - 1; i >= 0; --i) {
            acc += static_cast<int>(current[i].line.size()) + 1;
            kept.insert(kept.begin(), current[i]);
            if (acc >= overlap_chars) break;
        }
        current = kept;
        current_chars = 0;
        for (const auto& e : current) {
            current_chars += static_cast<int>(e.line.size()) + 1;
        }
    };

    // 按行处理
    std::vector<std::string> lines;
    {
        std::istringstream ss(content);
        std::string line;
        while (std::getline(ss, line)) {
            lines.push_back(line);
        }
    }

    for (int i = 0; i < static_cast<int>(lines.size()); ++i) {
        const std::string& line = lines[i];
        int line_no = i + 1;  // 1-indexed

        // 长行按 maxChars 分段 (对齐 openclaw: segments)
        std::vector<std::string> segments;
        if (line.empty()) {
            segments.push_back("");
        } else {
            for (int start = 0; start < static_cast<int>(line.size()); start += max_chars) {
                segments.push_back(line.substr(start, max_chars));
            }
        }

        for (const auto& segment : segments) {
            int line_size = static_cast<int>(segment.size()) + 1;
            if (current_chars + line_size > max_chars && !current.empty()) {
                flush();
                carry_overlap();
            }
            current.push_back({segment, line_no});
            current_chars += line_size;
        }
    }

    flush();
    return chunks;
}

// ============================================================================
// remap_chunk_lines (对齐 openclaw internal.ts:remapChunkLines)
// ============================================================================
void remap_chunk_lines(
    std::vector<MemoryChunk>& chunks,
    const std::vector<int>& line_map) {

    if (line_map.empty()) return;

    for (auto& chunk : chunks) {
        // startLine/endLine are 1-indexed; lineMap is 0-indexed
        int start_idx = chunk.start_line - 1;
        int end_idx = chunk.end_line - 1;

        if (start_idx >= 0 && start_idx < static_cast<int>(line_map.size())) {
            chunk.start_line = line_map[start_idx];
        }
        if (end_idx >= 0 && end_idx < static_cast<int>(line_map.size())) {
            chunk.end_line = line_map[end_idx];
        }
    }
}

// ============================================================================
// parse_embedding (对齐 openclaw internal.ts:parseEmbedding)
// ============================================================================
std::vector<float> parse_embedding(const std::string& raw) {
    std::vector<float> result;
    try {
        // Simple JSON array parser
        std::string s = raw;
        s.erase(std::remove(s.begin(), s.end(), '['), s.end());
        s.erase(std::remove(s.begin(), s.end(), ']'), s.end());
        std::istringstream ss(s);
        std::string token;
        while (std::getline(ss, token, ',')) {
            try {
                result.push_back(std::stof(token));
            } catch (...) {}
        }
    } catch (...) {}
    return result;
}

// ============================================================================
// cosine_similarity (对齐 openclaw internal.ts:cosineSimilarity)
// ============================================================================
float cosine_similarity(const std::vector<float>& a, const std::vector<float>& b) {
    if (a.empty() || b.empty()) return 0.0f;
    size_t len = std::min(a.size(), b.size());
    float dot = 0.0f, norm_a = 0.0f, norm_b = 0.0f;
    for (size_t i = 0; i < len; ++i) {
        dot += a[i] * b[i];
        norm_a += a[i] * a[i];
        norm_b += b[i] * b[i];
    }
    if (norm_a == 0.0f || norm_b == 0.0f) return 0.0f;
    return dot / (std::sqrt(norm_a) * std::sqrt(norm_b));
}

// ============================================================================
// normalize_rel_path (对齐 openclaw internal.ts:normalizeRelPath)
// ============================================================================
std::string normalize_rel_path(const std::string& value) {
    std::string trimmed = value;
    // trim whitespace (对齐 openclaw: value.trim())
    trimmed.erase(0, trimmed.find_first_not_of(" \t\n\r"));
    if (trimmed.empty()) return "";
    trimmed.erase(trimmed.find_last_not_of(" \t\n\r") + 1);
    // 对齐 openclaw: .replace(/^[./]+/, "") — 去掉所有前导 '.' 和 '/'
    size_t start = 0;
    while (start < trimmed.size() && (trimmed[start] == '.' || trimmed[start] == '/')) {
        ++start;
    }
    trimmed = trimmed.substr(start);
    // normalize backslashes (对齐 openclaw: .replace(/\\/g, "/"))
    std::replace(trimmed.begin(), trimmed.end(), '\\', '/');
    return trimmed;
}

// ============================================================================
// normalize_extra_memory_paths (对齐 openclaw internal.ts:normalizeExtraMemoryPaths)
// ============================================================================
std::vector<std::string> normalize_extra_memory_paths(
    const std::string& workspace_dir,
    const std::vector<std::string>& extra_paths) {

    std::vector<std::string> result;
    for (const auto& p : extra_paths) {
        std::string trimmed = p;
        trimmed.erase(0, trimmed.find_first_not_of(" \t\n\r"));
        trimmed.erase(trimmed.find_last_not_of(" \t\n\r") + 1);
        if (trimmed.empty()) continue;

        fs::path resolved;
        if (fs::path(trimmed).is_absolute()) {
            resolved = fs::path(trimmed);
        } else {
            resolved = fs::path(workspace_dir) / trimmed;
        }
        result.push_back(resolved.lexically_normal().string());
    }
    // deduplicate
    std::sort(result.begin(), result.end());
    result.erase(std::unique(result.begin(), result.end()), result.end());
    return result;
}

// ============================================================================
// is_memory_path (对齐 openclaw internal.ts:isMemoryPath)
// ============================================================================
bool is_memory_path(const std::string& rel_path) {
    std::string normalized = normalize_rel_path(rel_path);
    if (normalized.empty()) return false;
    // 对齐 openclaw internal.ts:73-76:
    // normalized === "MEMORY.md" || normalized === "memory.md" || normalized.startsWith("memory/")
    if (normalized == "MEMORY.md" || normalized == "memory.md") return true;
    return normalized.rfind("memory/", 0) == 0;
}

// ============================================================================
// list_memory_files (对齐 openclaw internal.ts:listMemoryFiles)
// ============================================================================

// isAllowedMemoryFilePath (对齐 openclaw internal.ts:isAllowedMemoryFilePath)
static bool is_allowed_memory_file_path(
    const std::string& file_path,
    const MemoryMultimodalSettings* multimodal) {
    if (file_path.size() >= 3 &&
        file_path.substr(file_path.size() - 3) == ".md") {
        return true;
    }
    if (multimodal) {
        return classify_memory_multimodal_path(file_path, *multimodal).has_value();
    }
    return false;
}

static void walk_dir(
    const std::string& dir,
    std::vector<std::string>& files,
    const MemoryMultimodalSettings* multimodal = nullptr) {
    try {
        for (const auto& entry : fs::recursive_directory_iterator(dir)) {
            if (entry.is_symlink()) continue;
            if (!entry.is_regular_file()) continue;
            if (is_allowed_memory_file_path(entry.path().string(), multimodal)) {
                files.push_back(entry.path().string());
            }
        }
    } catch (...) {}
}

core::Result<std::vector<std::string>> list_memory_files(
    const std::string& workspace_dir,
    const std::vector<std::string>& extra_paths,
    const MemoryMultimodalSettings* multimodal) {

    std::vector<std::string> result;

    // 1. MEMORY.md + memory.md (对齐 openclaw: addMarkdownFile)
    auto add_md = [&](const std::string& abs_path) {
        try {
            auto stat = fs::symlink_status(abs_path);
            if (!fs::is_symlink(stat) && fs::is_regular_file(stat)) {
                if (abs_path.size() >= 3 &&
                    abs_path.substr(abs_path.size() - 3) == ".md") {
                    result.push_back(abs_path);
                }
            }
        } catch (...) {}
    };

    add_md((fs::path(workspace_dir) / "MEMORY.md").string());
    add_md((fs::path(workspace_dir) / "memory.md").string());

    // 2. memory/ directory
    try {
        fs::path memory_dir = fs::path(workspace_dir) / "memory";
        auto stat = fs::symlink_status(memory_dir);
        if (!fs::is_symlink(stat) && fs::is_directory(stat)) {
            walk_dir(memory_dir.string(), result);
        }
    } catch (...) {}

    // 3. extra_paths (对齐 openclaw: normalizedExtraPaths)
    auto normalized = normalize_extra_memory_paths(workspace_dir, extra_paths);
    for (const auto& p : normalized) {
        try {
            auto stat = fs::symlink_status(p);
            if (fs::is_symlink(stat)) continue;
            if (fs::is_directory(stat)) {
                walk_dir(p, result, multimodal);
            } else if (fs::is_regular_file(stat) &&
                       is_allowed_memory_file_path(p, multimodal)) {
                result.push_back(p);
            }
        } catch (...) {}
    }

    // 4. deduplicate (对齐 openclaw: deduped via realpath)
    if (result.size() > 1) {
        std::vector<std::string> deduped;
        std::set<std::string> seen;
        for (const auto& f : result) {
            std::string key = f;
            try { key = fs::canonical(f).string(); } catch (...) {}
            if (seen.insert(key).second) {
                deduped.push_back(f);
            }
        }
        result = deduped;
    }

    return core::Result<std::vector<std::string>>::Ok(result);
}

// ============================================================================
// build_file_entry (对齐 openclaw internal.ts:buildFileEntry)
// ============================================================================
std::optional<MemoryFileEntry> build_file_entry(
    const std::string& abs_path,
    const std::string& workspace_dir,
    const MemoryMultimodalSettings* multimodal) {

    try {
        auto stat = fs::status(abs_path);
        if (!fs::is_regular_file(stat)) return std::nullopt;

        // relative path (对齐 openclaw: path.relative + replace backslash)
        auto rel = fs::relative(abs_path, workspace_dir).string();
        std::replace(rel.begin(), rel.end(), '\\', '/');

        auto mtime = fs::last_write_time(abs_path);
        auto mtime_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            mtime.time_since_epoch()
        ).count();
        int64_t file_size = static_cast<int64_t>(fs::file_size(abs_path));

        // 检查是否是 multimodal 文件 (对齐 openclaw: classifyMemoryMultimodalPath)
        if (multimodal) {
            auto modality = classify_memory_multimodal_path(abs_path, *multimodal);
            if (modality.has_value()) {
                // 对齐 openclaw: size > maxFileBytes -> return null
                if (file_size > static_cast<int64_t>(multimodal->max_file_bytes)) {
                    return std::nullopt;
                }
                // 读取文件内容
                std::ifstream bin_file(abs_path, std::ios::binary);
                if (!bin_file.is_open()) return std::nullopt;
                std::string buffer((std::istreambuf_iterator<char>(bin_file)),
                                    std::istreambuf_iterator<char>());

                // 对齐 openclaw: mimeType check (mimeType.startsWith(`${modality}/`))
                // HunterClaw 简化：跳过 MIME 检测，直接按扩展名分类
                std::string modality_str;
                if (*modality == MemoryMultimodalModality::IMAGE) modality_str = "image";
                else if (*modality == MemoryMultimodalModality::AUDIO) modality_str = "audio";

                std::string content_text = build_memory_multimodal_label(*modality, rel);
                std::string data_hash = hash_text(buffer);

                // chunkHash = hashText(JSON.stringify({path, contentText, mimeType, dataHash}))
                // 对齐 openclaw: 使用相同的 hash 输入
                std::string hash_input = "{\"path\":\"" + rel +
                    "\",\"contentText\":\"" + content_text +
                    "\",\"dataHash\":\"" + data_hash + "\"}";  // mimeType 省略
                std::string chunk_hash = hash_text(hash_input);

                MemoryFileEntry entry;
                entry.path = rel;
                entry.abs_path = abs_path;
                entry.mtime_ms = mtime_ms;
                entry.size = file_size;
                entry.hash = chunk_hash;
                entry.data_hash = data_hash;
                entry.kind = "multimodal";
                entry.content_text = content_text;
                entry.modality = *modality;
                return entry;
            }
        }

        // Markdown 文件 (对齐 openclaw: readFile + hashText)
        std::ifstream file(abs_path);
        if (!file.is_open()) return std::nullopt;
        std::string content((std::istreambuf_iterator<char>(file)),
                             std::istreambuf_iterator<char>());

        MemoryFileEntry entry;
        entry.path = rel;
        entry.abs_path = abs_path;
        entry.mtime_ms = mtime_ms;
        entry.size = file_size;
        entry.hash = hash_text(content);
        entry.kind = "markdown";

        return entry;
    } catch (...) {
        return std::nullopt;
    }
}

// ============================================================================
// ensure_dir (对齐 openclaw internal.ts:ensureDir)
// ============================================================================
std::string ensure_dir(const std::string& dir) {
    try {
        fs::create_directories(dir);
    } catch (...) {}
    return dir;
}

// ============================================================================
// build_text_embedding_input (对齐 openclaw embedding-inputs.ts:buildTextEmbeddingInput)
// ============================================================================
EmbeddingInput build_text_embedding_input(const std::string& text) {
    EmbeddingInput input;
    input.text = text;
    EmbeddingInputPart part;
    part.type = "text";
    part.text = text;
    input.parts.push_back(part);
    return input;
}

// ============================================================================
// sanitize_and_normalize_embedding
// (对齐 openclaw embedding-vectors.ts:sanitizeAndNormalizeEmbedding)
// ============================================================================
std::vector<float> sanitize_and_normalize_embedding(const std::vector<float>& embedding) {
    // 1. sanitize: replace NaN/Inf with 0
    std::vector<float> sanitized;
    sanitized.reserve(embedding.size());
    for (float v : embedding) {
        sanitized.push_back(std::isfinite(v) ? v : 0.0f);
    }

    // 2. compute magnitude
    double sum_sq = 0.0;
    for (float v : sanitized) {
        sum_sq += static_cast<double>(v) * v;
    }
    double magnitude = std::sqrt(sum_sq);

    // 3. if magnitude < 1e-10, return sanitized as-is (对齐 openclaw)
    if (magnitude < 1e-10) {
        return sanitized;
    }

    // 4. normalize
    std::vector<float> normalized;
    normalized.reserve(sanitized.size());
    for (float v : sanitized) {
        normalized.push_back(static_cast<float>(v / magnitude));
    }
    return normalized;
}

// ============================================================================
// Embedding Input Limits
// ============================================================================

// ============================================================================
// estimate_utf8_bytes (对齐 openclaw embedding-input-limits.ts:estimateUtf8Bytes)
// ============================================================================
int estimate_utf8_bytes(const std::string& text) {
    // UTF-8 字节数 = 字符串的字节长度（std::string 存储 UTF-8）
    return static_cast<int>(text.size());
}

// ============================================================================
// split_text_to_byte_limit (对齐 openclaw embedding-input-limits.ts:splitTextToUtf8ByteLimit)
// 使用二分查找精确按 UTF-8 字节边界分割，避免切断多字节字符
// ============================================================================
std::vector<std::string> split_text_to_byte_limit(
    const std::string& text,
    int max_bytes) {

    if (max_bytes <= 0) {
        return {text};
    }
    if (static_cast<int>(text.size()) <= max_bytes) {
        return {text};
    }

    std::vector<std::string> parts;
    size_t cursor = 0;
    const size_t n = text.size();

    while (cursor < n) {
        // 二分查找最大合法分割点（不超过 max_bytes 字节）
        size_t low = cursor + 1;
        size_t high = std::min(n, cursor + static_cast<size_t>(max_bytes));
        size_t best = cursor;

        while (low <= high) {
            size_t mid = (low + high) / 2;
            int bytes = static_cast<int>(mid - cursor);
            if (bytes <= max_bytes) {
                best = mid;
                low = mid + 1;
            } else {
                high = mid - 1;
            }
        }

        if (best <= cursor) {
            best = std::min(n, cursor + 1);
        }

        // 避免切断 UTF-8 多字节序列（对齐 openclaw 的 surrogate pair 检查）
        // UTF-8 continuation bytes: 10xxxxxx (0x80-0xBF)
        while (best < n && best > cursor &&
               (static_cast<unsigned char>(text[best]) & 0xC0) == 0x80) {
            --best;
        }
        if (best <= cursor) {
            best = cursor + 1;
        }

        std::string part = text.substr(cursor, best - cursor);
        if (part.empty()) break;
        parts.push_back(std::move(part));
        cursor = best;
    }

    return parts;
}

// ============================================================================
// resolve_embedding_max_input_tokens
// (对齐 openclaw embedding-model-limits.ts:resolveEmbeddingMaxInputTokens)
// ============================================================================
int resolve_embedding_max_input_tokens(
    const std::string& provider_id,
    const std::string& model,
    std::optional<int> provider_max_input_tokens) {

    // 优先使用 provider 自身声明的 maxInputTokens
    if (provider_max_input_tokens.has_value()) {
        return provider_max_input_tokens.value();
    }

    // 已知模型的精确限制 (对齐 KNOWN_EMBEDDING_MAX_INPUT_TOKENS)
    static const std::map<std::string, int> KNOWN_LIMITS = {
        {"openai:text-embedding-3-small",    8192},
        {"openai:text-embedding-3-large",    8192},
        {"openai:text-embedding-ada-002",    8191},
        {"gemini:text-embedding-004",        2048},
        {"gemini:gemini-embedding-001",      2048},
        {"gemini:gemini-embedding-2-preview",8192},
        {"voyage:voyage-3",                 32000},
        {"voyage:voyage-3-lite",            16000},
        {"voyage:voyage-code-3",            32000},
    };

    // 构造 key: "provider:model" (小写)
    std::string key = provider_id + ":" + model;
    std::transform(key.begin(), key.end(), key.begin(), ::tolower);

    auto it = KNOWN_LIMITS.find(key);
    if (it != KNOWN_LIMITS.end()) {
        return it->second;
    }

    // Provider 级别的保守回退
    std::string pid = provider_id;
    std::transform(pid.begin(), pid.end(), pid.begin(), ::tolower);
    if (pid == "gemini") return 2048;
    if (pid == "local")  return 2048;  // DEFAULT_LOCAL_EMBEDDING_MAX_INPUT_TOKENS

    return 8192;  // DEFAULT_EMBEDDING_MAX_INPUT_TOKENS
}

// ============================================================================
// enforce_embedding_max_input_tokens
// (对齐 openclaw embedding-chunk-limits.ts:enforceEmbeddingMaxInputTokens)
// ============================================================================
std::vector<MemoryChunk> enforce_embedding_max_input_tokens(
    const std::vector<MemoryChunk>& chunks,
    int max_input_tokens) {

    std::vector<MemoryChunk> result;

    for (const auto& chunk : chunks) {
        // 对齐 openclaw: hasNonTextEmbeddingParts -> 直接保留（multimodal chunk）
        if (!chunk.embedding_input.parts.empty()) {
            bool has_non_text = false;
            for (const auto& part : chunk.embedding_input.parts) {
                if (part.type != "text") {
                    has_non_text = true;
                    break;
                }
            }
            if (has_non_text) {
                result.push_back(chunk);
                continue;
            }
        }

        int estimated_bytes = estimate_utf8_bytes(chunk.text);
        if (estimated_bytes <= max_input_tokens) {
            result.push_back(chunk);
            continue;
        }

        // 按字节限制分割
        auto split_texts = split_text_to_byte_limit(chunk.text, max_input_tokens);
        for (const auto& text : split_texts) {
            MemoryChunk new_chunk;
            new_chunk.start_line = chunk.start_line;
            new_chunk.end_line   = chunk.end_line;
            new_chunk.text       = text;
            new_chunk.hash       = hash_text(text);
            new_chunk.embedding_input = build_text_embedding_input(text);
            result.push_back(std::move(new_chunk));
        }
    }

    return result;
}

// ============================================================================
// File Scanning
// ============================================================================

bool is_binary_file(const std::string& file_path) {
    std::ifstream file(file_path, std::ios::binary);
    if (!file) {
        return false;
    }
    
    // 简单检测:读取前512字节,检查是否有太多非文本字符
    char buffer[512];
    file.read(buffer, sizeof(buffer));
    std::streamsize bytes_read = file.gcount();
    
    int non_text_count = 0;
    for (std::streamsize i = 0; i < bytes_read; ++i) {
        unsigned char c = static_cast<unsigned char>(buffer[i]);
        // ASCII控制字符(排除常见的空白字符)
        if (c < 32 && c != '\n' && c != '\r' && c != '\t') {
            non_text_count++;
        }
    }
    
    // 如果超过5%是非文本字符,认为是二进制文件
    return bytes_read > 0 && (non_text_count * 100 / bytes_read) > 5;
}

bool should_ignore_file(
    const std::string& file_path,
    const std::vector<std::string>& patterns) {
    
    // 简化的模式匹配
    for (const auto& pattern : patterns) {
        if (file_path.find(pattern) != std::string::npos) {
            return true;
        }
    }
    
    return false;
}

core::Result<std::vector<FileEntry>> scan_directory(
    const std::string& root_dir,
    const std::vector<std::string>& ignore_patterns) {
    
    std::vector<FileEntry> files;
    
    try {
        for (const auto& entry : fs::recursive_directory_iterator(root_dir)) {
            if (!entry.is_regular_file()) {
                continue;
            }
            
            std::string path = entry.path().string();
            
            // Check ignore patterns
            if (should_ignore_file(path, ignore_patterns)) {
                continue;
            }
            
            // Check binary
            if (is_binary_file(path)) {
                continue;
            }
            
            FileEntry file_entry;
            file_entry.path = path;
            file_entry.size = static_cast<int64_t>(entry.file_size());
            
            // Get modification time
            auto ftime = fs::last_write_time(entry.path());
            auto sctp = std::chrono::time_point_cast<std::chrono::system_clock::duration>(
                ftime - fs::file_time_type::clock::now() + std::chrono::system_clock::now()
            );
            file_entry.mtime = std::chrono::system_clock::to_time_t(sctp);
            
            files.push_back(file_entry);
        }
        
        return core::Result<std::vector<FileEntry>>::Ok(files);
        
    } catch (const std::exception& e) {
        return core::Result<std::vector<FileEntry>>::Err(
            std::string("Failed to scan directory: ") + e.what()
        );
    }
}

}  // namespace memory
}  // namespace hunterclaw
