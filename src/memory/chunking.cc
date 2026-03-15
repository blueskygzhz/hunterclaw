#include "hunterclaw/memory/chunking.h"
#include <algorithm>
#include <cctype>
#include <fstream>
#include <sstream>
#include <regex>
#include <filesystem>
#include <functional>

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
    // 简单的字符串hash (对齐 OpenClaw hashText)
    std::hash<std::string> hasher;
    size_t hash_value = hasher(text);
    
    std::ostringstream oss;
    oss << std::hex << hash_value;
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
    const std::string& file_path,
    const std::string& content,
    int max_chunk_size) {
    
    // 对齐 OpenClaw internal.ts::chunkMarkdown
    ChunkOptions options;
    options.max_chunk_size = max_chunk_size;
    options.mode = ChunkMode::Paragraph;
    options.preserve_code_blocks = true;
    
    auto text_chunks = chunk_text_smart(content, options);
    
    std::vector<MemoryChunk> memory_chunks;
    int current_line = 1;
    
    for (const auto& text : text_chunks) {
        MemoryChunk chunk;
        chunk.path = file_path;
        chunk.start_line = current_line;
        
        // Count lines in chunk
        int line_count = 1 + std::count(text.begin(), text.end(), '\n');
        chunk.end_line = current_line + line_count - 1;
        
        chunk.text = text;
        chunk.hash = hash_text(text);
        chunk.embedding_input.text = text;
        
        memory_chunks.push_back(chunk);
        current_line = chunk.end_line + 1;
    }
    
    return memory_chunks;
}

// ============================================================================
// Embedding Input Limits
// ============================================================================

std::vector<std::string> split_text_to_byte_limit(
    const std::string& text,
    int max_bytes) {
    
    if (text.length() <= static_cast<size_t>(max_bytes)) {
        return {text};
    }
    
    return chunk_by_length(text, max_bytes);
}

std::vector<MemoryChunk> enforce_embedding_max_input_tokens(
    const std::vector<MemoryChunk>& chunks,
    int max_input_tokens) {
    
    std::vector<MemoryChunk> result;
    
    for (const auto& chunk : chunks) {
        int estimated_bytes = estimate_utf8_bytes(chunk.text);
        
        if (estimated_bytes <= max_input_tokens) {
            result.push_back(chunk);
            continue;
        }
        
        // Split large chunk
        auto split_texts = split_text_to_byte_limit(chunk.text, max_input_tokens);
        
        for (const auto& text : split_texts) {
            MemoryChunk new_chunk = chunk;
            new_chunk.text = text;
            new_chunk.hash = hash_text(text);
            new_chunk.embedding_input.text = text;
            result.push_back(new_chunk);
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
        
        return core::Result<std::vector<FileEntry>>::ok_value(files);
        
    } catch (const std::exception& e) {
        return core::Result<std::vector<FileEntry>>::error(
            std::string("Failed to scan directory: ") + e.what()
        );
    }
}

}  // namespace memory
}  // namespace hunterclaw
