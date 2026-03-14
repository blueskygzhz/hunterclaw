#pragma once

#include <string>
#include <vector>
#include <optional>
#include <unordered_map>
#include <functional>
#include <cstdint>
#include <array>

namespace hunterclaw {
namespace shared {

// ============================================================================
// String Normalization (对齐 shared/string-normalization.ts)
// ============================================================================

// Normalize list of string entries, trim and filter empty
std::vector<std::string> normalize_string_entries(
    const std::vector<std::string>& list);

// Normalize to lowercase
std::vector<std::string> normalize_string_entries_lower(
    const std::vector<std::string>& list);

// Normalize hyphen-separated slug (e.g., "My Agent" -> "my-agent")
std::string normalize_hyphen_slug(const std::string& raw);

// Normalize @/#-prefixed slug (e.g., "@MyBot" -> "mybot")
std::string normalize_at_hash_slug(const std::string& raw);

// ============================================================================
// IP Address Utilities (对齐 shared/net/ip.ts)
// ============================================================================

enum class IpKind {
  IPV4,
  IPV6
};

struct ParsedIpAddress {
  IpKind kind;
  std::string normalized;
  std::array<uint16_t, 8> parts;  // IPv6 format (IPv4 mapped if needed)
  
  std::string to_string() const;
  std::string range() const;  // "loopback", "private", "unicast", etc.
};

// Parse canonical IP address (strict: only standard formats)
std::optional<ParsedIpAddress> parse_canonical_ip_address(const std::string& raw);

// Parse loose IP address (lenient: accepts various formats)
std::optional<ParsedIpAddress> parse_loose_ip_address(const std::string& raw);

// Normalize IP address to canonical form
std::optional<std::string> normalize_ip_address(const std::string& raw);

// Check if canonical dotted-decimal IPv4 (e.g., "192.168.1.1")
bool is_canonical_dotted_decimal_ipv4(const std::string& raw);

// Check if legacy IPv4 literal (e.g., "0xC0A80101", "3232235777")
bool is_legacy_ipv4_literal(const std::string& raw);

// Check if loopback address (127.0.0.1, ::1)
bool is_loopback_ip_address(const std::string& raw);

// Check if private or loopback (10.0.0.0/8, 192.168.0.0/16, 172.16.0.0/12, etc.)
bool is_private_or_loopback_ip_address(const std::string& raw);

// Check if RFC1918 private IPv4 (10.0.0.0/8, 192.168.0.0/16, 172.16.0.0/12)
bool is_rfc1918_ipv4_address(const std::string& raw);

// Check if blocked special-use IPv6 (unspecified, multicast, unique-local, etc.)
bool is_blocked_special_use_ipv6_address(const ParsedIpAddress& addr);

// Strip IPv6 brackets: "[::1]" -> "::1"
std::string strip_ipv6_brackets(const std::string& value);

// ============================================================================
// ANSI Terminal Utilities (对齐 terminal/ansi.ts)
// ============================================================================

// Strip ANSI escape sequences (SGR + OSC-8 hyperlinks)
std::string strip_ansi(const std::string& input);

// Split text into grapheme clusters (Unicode-aware)
std::vector<std::string> split_graphemes(const std::string& input);

// Calculate visible width (accounting for full-width chars, emoji, etc.)
size_t visible_width(const std::string& input);

// Sanitize text for log output (strip ANSI + control chars for CWE-117)
std::string sanitize_for_log(const std::string& value);

// Check if code point is zero-width (combining marks, ZWJ, etc.)
bool is_zero_width_code_point(uint32_t codepoint);

// Check if code point is full-width (CJK, emoji, etc.)
bool is_full_width_code_point(uint32_t codepoint);

// Calculate grapheme width (0 for zero-width, 2 for full-width, 1 otherwise)
size_t grapheme_width(const std::string& grapheme);

// ============================================================================
// Chat Content Utilities (对齐 shared/chat-content.ts)
// ============================================================================

// Extract text from chat content (string or structured array)
std::optional<std::string> extract_text_from_chat_content(
    const std::string& content,
    const std::function<std::string(const std::string&)>& sanitize_text = nullptr,
    const std::string& join_with = " ");

// Strip assistant internal scaffolding (reasoning tags, memory tags)
std::string strip_assistant_internal_scaffolding(const std::string& text);

// Strip reasoning tags from text
std::string strip_reasoning_tags_from_text(
    const std::string& text,
    const std::string& mode = "preserve",  // "preserve" or "strip"
    const std::string& trim = "start");     // "start", "end", "both", "none"

// Strip relevant memories tags
std::string strip_relevant_memories_tags(const std::string& text);

// ============================================================================
// Config Evaluation (对齐 shared/config-eval.ts)
// ============================================================================

// Check if value is truthy (non-empty, non-zero, true)
bool is_truthy(const std::string& value);
bool is_truthy(bool value);
bool is_truthy(int value);

// Resolve config path (e.g., "agent.hooks.enabled" -> value)
std::optional<std::string> resolve_config_path(
    const std::unordered_map<std::string, std::string>& config,
    const std::string& path_str);

// Check if config path is truthy with defaults
bool is_config_path_truthy_with_defaults(
    const std::unordered_map<std::string, std::string>& config,
    const std::string& path_str,
    const std::unordered_map<std::string, bool>& defaults);

// Runtime requirements
struct RuntimeRequires {
  std::vector<std::string> bins;        // Required binaries
  std::vector<std::string> any_bins;    // At least one required
  std::vector<std::string> env;         // Required env vars
  std::vector<std::string> config;      // Required config paths
};

struct RuntimeRequirementEvalParams {
  RuntimeRequires requires;
  std::function<bool(const std::string&)> has_bin;
  std::function<bool(const std::string&)> has_remote_bin;
  std::function<bool(const std::vector<std::string>&)> has_any_remote_bin;
  std::function<bool(const std::string&)> has_env;
  std::function<bool(const std::string&)> is_config_path_truthy;
};

// Evaluate runtime requirements
bool evaluate_runtime_requires(const RuntimeRequirementEvalParams& params);

// ============================================================================
// Terminal Table Rendering (对齐 terminal/table.ts)
// ============================================================================

enum class TableAlign {
  LEFT,
  RIGHT,
  CENTER
};

enum class TableBorder {
  UNICODE,
  ASCII,
  NONE
};

struct TableColumn {
  std::string key;
  std::string header;
  TableAlign align = TableAlign::LEFT;
  size_t min_width = 0;
  size_t max_width = 0;
  bool flex = false;
};

struct RenderTableOptions {
  std::vector<TableColumn> columns;
  std::vector<std::unordered_map<std::string, std::string>> rows;
  size_t width = 80;
  size_t padding = 1;
  TableBorder border = TableBorder::UNICODE;
};

// Render ASCII/Unicode table
std::string render_table(const RenderTableOptions& options);

// Pad cell text to width with alignment
std::string pad_cell(const std::string& text, size_t width, TableAlign align);

// Wrap line to max width (ANSI-aware)
std::vector<std::string> wrap_line(const std::string& text, size_t width);

// Resolve default border style based on platform/terminal
TableBorder resolve_default_border(
    const std::string& platform,
    const std::unordered_map<std::string, std::string>& env);

// ============================================================================
// Terminal Theme & Palette (对齐 terminal/theme.ts, terminal/palette.ts)
// ============================================================================

// Lobster palette (CLI theme colors)
struct LobsterPalette {
  std::string accent = "#FF5A2D";
  std::string accent_bright = "#FF7A3D";
  std::string accent_dim = "#D14A22";
  std::string info = "#FF8A5B";
  std::string success = "#2FBF71";
  std::string warn = "#FFB020";
  std::string error = "#E23D2D";
  std::string muted = "#8B7F77";
};

// Get lobster palette instance
const LobsterPalette& get_lobster_palette();

// Colorize text with ANSI codes
std::string colorize_text(
    const std::string& text,
    const std::string& color_hex,
    bool rich = true);

// Check if terminal supports rich output
bool is_rich_terminal();

// Apply theme colors
std::string theme_accent(const std::string& text);
std::string theme_success(const std::string& text);
std::string theme_warn(const std::string& text);
std::string theme_error(const std::string& text);
std::string theme_muted(const std::string& text);
std::string theme_heading(const std::string& text);

// ============================================================================
// Text Processing (对齐 shared/text/)
// ============================================================================

// Find code regions in text (markdown ```...```)
struct CodeRegion {
  size_t start;
  size_t end;
  std::string language;
};

std::vector<CodeRegion> find_code_regions(const std::string& text);

// Check if position is inside code region
bool is_inside_code(size_t pos, const std::vector<CodeRegion>& regions);

// Join text segments with separator
std::string join_segments(
    const std::vector<std::string>& segments,
    const std::string& separator = " ");

// Sample text to max length (with ellipsis)
std::string sample_text(const std::string& text, size_t max_length);

}  // namespace shared
}  // namespace hunterclaw
