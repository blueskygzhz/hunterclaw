#include <string>
#include <vector>
#include <optional>
#include <algorithm>
#include <sstream>
#include <regex>

namespace hunterclaw {
namespace security {

// ============================================================================
// Channel Metadata - 不可信通道元数据安全包装
// 严格对齐 OpenClaw: src/security/channel-metadata.ts
// ============================================================================

namespace {

constexpr int DEFAULT_MAX_CHARS = 800;
constexpr int DEFAULT_MAX_ENTRY_CHARS = 400;

std::string normalize_entry(const std::string& entry) {
    // Replace whitespace sequences with single space, trim
    std::string result = std::regex_replace(entry, std::regex(R"(\s+)"), " ");
    size_t start = result.find_first_not_of(" ");
    size_t end = result.find_last_not_of(" ");
    if (start == std::string::npos) return "";
    return result.substr(start, end - start + 1);
}

std::string truncate_text(const std::string& value, int max_chars) {
    if (max_chars <= 0) return "";
    if ((int)value.size() <= max_chars) return value;
    int trim_len = std::max(0, max_chars - 3);
    std::string trimmed = value.substr(0, trim_len);
    // Trim trailing whitespace
    size_t end = trimmed.find_last_not_of(" \t\r\n");
    if (end != std::string::npos) trimmed = trimmed.substr(0, end + 1);
    return trimmed + "...";
}

} // anonymous namespace

// Forward declaration from audit.cc
std::string wrap_external_content(const std::string& content, int source_type);

/**
 * build_untrusted_channel_metadata - 构建不可信通道元数据
 * 对齐 OpenClaw: buildUntrustedChannelMetadata(params)
 *
 * @param source 来源标识
 * @param label 标签
 * @param entries 元数据条目列表
 * @param max_chars 最大字符数
 * @return 包装后的安全元数据字符串，或 nullopt
 */
std::optional<std::string> build_untrusted_channel_metadata(
    const std::string& source,
    const std::string& label,
    const std::vector<std::string>& entries,
    int max_chars
) {
    if (max_chars <= 0) max_chars = DEFAULT_MAX_CHARS;
    
    // Clean and dedupe entries
    std::vector<std::string> cleaned;
    for (const auto& entry : entries) {
        std::string normalized = normalize_entry(entry);
        if (normalized.empty()) continue;
        std::string truncated = truncate_text(normalized, DEFAULT_MAX_ENTRY_CHARS);
        cleaned.push_back(truncated);
    }
    
    // Deduplicate
    std::vector<std::string> deduped;
    for (const auto& entry : cleaned) {
        bool found = false;
        for (const auto& existing : deduped) {
            if (existing == entry) { found = true; break; }
        }
        if (!found) deduped.push_back(entry);
    }
    
    if (deduped.empty()) return std::nullopt;
    
    // Build body
    std::string body;
    for (size_t i = 0; i < deduped.size(); i++) {
        if (i > 0) body += "\n";
        body += deduped[i];
    }
    
    std::string header = "UNTRUSTED channel metadata (" + source + ")";
    std::string labeled = label + ":\n" + body;
    std::string full = header + "\n" + labeled;
    std::string truncated = truncate_text(full, max_chars);
    
    // Wrap with external content markers (source type 4 = ChannelMetadata, no warning)
    // Simple inline wrapping without full warning
    return "<<<EXTERNAL_UNTRUSTED_CONTENT>>>\n" + truncated + "\n<<<END_EXTERNAL_UNTRUSTED_CONTENT>>>";
}

}  // namespace security
}  // namespace hunterclaw
