#pragma once

#include "hunterclaw/core/result.h"
#include <string>
#include <vector>
#include <optional>

namespace hunterclaw {
namespace memory {

// ============================================================================
// Text Chunking (对齐 OpenClaw chunk.ts 和 text-chunking.ts)
// ============================================================================

// Chunk mode
enum class ChunkMode {
    Length,    // Split only when exceeding limit
    Newline,   // Prefer paragraph boundaries
    Paragraph  // Split on blank lines
};

// Chunking options
struct ChunkOptions {
    int max_chunk_size = 4000;
    ChunkMode mode = ChunkMode::Length;
    bool split_long_paragraphs = true;
    bool trim_lines = true;
    bool preserve_code_blocks = true;
};

// MemoryChunk (对齐 OpenClaw internal.ts)
struct MemoryChunk {
    std::string path;
    int start_line = 0;
    int end_line = 0;
    std::string text;
    std::string hash;
    
    // Embedding input (可以包含multimodal内容)
    struct EmbeddingInput {
        std::optional<std::string> text;
        // 未来可以扩展: images, structured_data等
    };
    EmbeddingInput embedding_input;
};

// ============================================================================
// Text Chunking Functions
// ============================================================================

/**
 * Split text into chunks by length
 * 基础的长度分块,不考虑语义边界
 */
std::vector<std::string> chunk_by_length(
    const std::string& text,
    int max_length
);

/**
 * Split text on paragraph boundaries (blank lines)
 * 对齐 OpenClaw chunkByParagraph
 * 
 * - 只在段落分隔符处断开 ("\n\n" 或更多)
 * - 将多个段落打包到单个chunk,直到达到limit
 * - 当单个段落超过limit时,回退到长度分块
 */
std::vector<std::string> chunk_by_paragraph(
    const std::string& text,
    int limit,
    bool split_long_paragraphs = true
);

/**
 * Split text on newlines, with smart paragraph handling
 * 对齐 OpenClaw chunkByNewline
 * 
 * - 在换行符处分割
 * - 折叠空行到下一个非空行
 * - 可选:分割长行
 */
std::vector<std::string> chunk_by_newline(
    const std::string& text,
    int max_line_length,
    bool split_long_lines = true,
    bool trim_lines = true
);

/**
 * Smart text chunking with markdown/code awareness
 * 对齐 OpenClaw chunkTextByBreakResolver
 * 
 * - 避免在代码块中间断开
 * - 尊重markdown结构
 * - 优先在合适的边界断开
 */
std::vector<std::string> chunk_text_smart(
    const std::string& text,
    const ChunkOptions& options = ChunkOptions{}
);

// ============================================================================
// Code Chunking (对齐 OpenClaw internal.ts::chunkMarkdown)
// ============================================================================

/**
 * Chunk markdown/code file into memory chunks
 * 对齐 OpenClaw internal.ts::chunkMarkdown
 * 
 * 返回带有行号信息的MemoryChunk数组
 */
std::vector<MemoryChunk> chunk_markdown(
    const std::string& file_path,
    const std::string& content,
    int max_chunk_size = 8000
);

/**
 * Hash text for deduplication (对齐 OpenClaw hashText)
 * 使用简单的字符串hash
 */
std::string hash_text(const std::string& text);

// ============================================================================
// Markdown/Code Fence Detection
// ============================================================================

struct FenceSpan {
    int start_line;
    int end_line;
    std::string lang;
    std::string fence_marker;  // "```" or "~~~"
};

/**
 * Parse code fence spans in markdown
 * 检测 ```language 块的起止位置
 */
std::vector<FenceSpan> parse_fence_spans(const std::string& text);

/**
 * Check if a position is safe to break (not inside code fence)
 */
bool is_safe_fence_break(const std::string& text, int position);

// ============================================================================
// Embedding Input Limits (对齐 embedding-input-limits.ts)
// ============================================================================

/**
 * Estimate UTF-8 byte count (简化版,用于token估算)
 * OpenClaw使用更精确的UTF-8编码计算,这里简化
 */
inline int estimate_utf8_bytes(const std::string& text) {
    return static_cast<int>(text.size());
}

/**
 * Split text to fit UTF-8 byte limit
 * 对齐 OpenClaw splitTextToUtf8ByteLimit
 */
std::vector<std::string> split_text_to_byte_limit(
    const std::string& text,
    int max_bytes
);

/**
 * Enforce embedding max input tokens
 * 对齐 OpenClaw enforceEmbeddingMaxInputTokens
 * 
 * 将超过token限制的chunk进一步分割
 */
std::vector<MemoryChunk> enforce_embedding_max_input_tokens(
    const std::vector<MemoryChunk>& chunks,
    int max_input_tokens
);

// ============================================================================
// File Scanning (对齐 OpenClaw manager-sync-ops.ts)
// ============================================================================

struct FileEntry {
    std::string path;
    int64_t mtime = 0;      // modification time
    int64_t size = 0;
    bool is_binary = false;
};

/**
 * Scan directory for indexable files
 * 对齐 OpenClaw的文件扫描逻辑
 * 
 * @param root_dir 根目录
 * @param ignore_patterns .gitignore风格的忽略模式
 * @return 扫描到的文件列表
 */
core::Result<std::vector<FileEntry>> scan_directory(
    const std::string& root_dir,
    const std::vector<std::string>& ignore_patterns = {}
);

/**
 * Check if file is binary (简化版)
 */
bool is_binary_file(const std::string& file_path);

/**
 * Check if file should be ignored based on patterns
 */
bool should_ignore_file(
    const std::string& file_path,
    const std::vector<std::string>& patterns
);

}  // namespace memory
}  // namespace hunterclaw
