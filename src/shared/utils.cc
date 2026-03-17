#include "hunterclaw/shared/utils.h"
#include <algorithm>
#include <cctype>
#include <regex>
#include <sstream>
#include <iomanip>
#include <cmath>

namespace hunterclaw {
namespace shared {

// ============================================================================
// String Normalization Implementation
// ============================================================================

std::vector<std::string> normalize_string_entries(
    const std::vector<std::string>& list) {
  std::vector<std::string> result;
  for (const auto& entry : list) {
    std::string trimmed = entry;
    // Trim whitespace
    trimmed.erase(0, trimmed.find_first_not_of(" \t\n\r\f\v"));
    trimmed.erase(trimmed.find_last_not_of(" \t\n\r\f\v") + 1);
    if (!trimmed.empty()) {
      result.push_back(trimmed);
    }
  }
  return result;
}

std::vector<std::string> normalize_string_entries_lower(
    const std::vector<std::string>& list) {
  auto normalized = normalize_string_entries(list);
  for (auto& entry : normalized) {
    std::transform(entry.begin(), entry.end(), entry.begin(), ::tolower);
  }
  return normalized;
}

std::string normalize_hyphen_slug(const std::string& raw) {
  std::string trimmed = raw;
  trimmed.erase(0, trimmed.find_first_not_of(" \t\n\r\f\v"));
  trimmed.erase(trimmed.find_last_not_of(" \t\n\r\f\v") + 1);
  std::transform(trimmed.begin(), trimmed.end(), trimmed.begin(), ::tolower);
  
  if (trimmed.empty()) {
    return "";
  }
  
  // Replace whitespace with hyphens
  std::regex ws_regex(R"(\s+)");
  std::string dashed = std::regex_replace(trimmed, ws_regex, "-");
  
  // Clean non-alphanumeric (keep #@._+-)
  std::regex clean_regex(R"([^a-z0-9#@._+-]+)");
  std::string cleaned = std::regex_replace(dashed, clean_regex, "-");
  
  // Collapse multiple hyphens
  std::regex multi_hyphen(R"(-{2,})");
  cleaned = std::regex_replace(cleaned, multi_hyphen, "-");
  
  // Trim leading/trailing hyphens and dots
  std::regex trim_regex(R"(^[-.]+|[-.]+$)");
  return std::regex_replace(cleaned, trim_regex, "");
}

std::string normalize_at_hash_slug(const std::string& raw) {
  std::string trimmed = raw;
  trimmed.erase(0, trimmed.find_first_not_of(" \t\n\r\f\v"));
  trimmed.erase(trimmed.find_last_not_of(" \t\n\r\f\v") + 1);
  std::transform(trimmed.begin(), trimmed.end(), trimmed.begin(), ::tolower);
  
  if (trimmed.empty()) {
    return "";
  }
  
  // Remove leading @/# characters
  std::regex prefix_regex(R"(^[@#]+)");
  std::string without_prefix = std::regex_replace(trimmed, prefix_regex, "");
  
  // Replace spaces/underscores with hyphens
  std::regex ws_regex(R"([\s_]+)");
  std::string dashed = std::regex_replace(without_prefix, ws_regex, "-");
  
  // Clean non-alphanumeric (keep hyphens only)
  std::regex clean_regex(R"([^a-z0-9-]+)");
  std::string cleaned = std::regex_replace(dashed, clean_regex, "-");
  
  // Collapse multiple hyphens
  std::regex multi_hyphen(R"(-{2,})");
  cleaned = std::regex_replace(cleaned, multi_hyphen, "-");
  
  // Trim leading/trailing hyphens
  std::regex trim_regex(R"(^-+|-+$)");
  return std::regex_replace(cleaned, trim_regex, "");
}

// ============================================================================
// IP Address Utilities Implementation
// ============================================================================

std::string strip_ipv6_brackets(const std::string& value) {
  if (value.size() >= 2 && value.front() == '[' && value.back() == ']') {
    return value.substr(1, value.size() - 2);
  }
  return value;
}

std::string ParsedIpAddress::to_string() const {
  return normalized;
}

std::string ParsedIpAddress::range() const {
  if (kind == IpKind::IPV4) {
    // Simple IPv4 range detection
    uint32_t first_octet = parts[0] >> 8;
    if (first_octet == 127) return "loopback";
    if (first_octet == 10) return "private";
    if (first_octet == 192 && (parts[0] & 0xFF) == 168) return "private";
    if (first_octet == 172 && ((parts[0] & 0xFF) >= 16 && (parts[0] & 0xFF) <= 31)) return "private";
    if (first_octet == 0) return "unspecified";
    if (first_octet >= 224) return "multicast";
    return "unicast";
  } else {
    // IPv6 range detection
    if (parts[0] == 0 && parts[1] == 0 && parts[2] == 0 && parts[3] == 0 &&
        parts[4] == 0 && parts[5] == 0 && parts[6] == 0 && parts[7] == 1) {
      return "loopback";
    }
    if (parts[0] == 0 && parts[1] == 0 && parts[2] == 0 && parts[3] == 0 &&
        parts[4] == 0 && parts[5] == 0 && parts[6] == 0 && parts[7] == 0) {
      return "unspecified";
    }
    if ((parts[0] & 0xFE00) == 0xFC00) return "uniqueLocal";
    if ((parts[0] & 0xFFC0) == 0xFE80) return "linkLocal";
    if ((parts[0] & 0xFF00) == 0xFF00) return "multicast";
    return "unicast";
  }
}

std::optional<ParsedIpAddress> parse_canonical_ip_address(const std::string& raw) {
  std::string trimmed = raw;
  trimmed.erase(0, trimmed.find_first_not_of(" \t\n\r\f\v"));
  trimmed.erase(trimmed.find_last_not_of(" \t\n\r\f\v") + 1);
  
  if (trimmed.empty()) {
    return std::nullopt;
  }
  
  std::string normalized = strip_ipv6_brackets(trimmed);
  
  // Try to parse as IPv4
  std::regex ipv4_regex(R"(^(\d{1,3})\.(\d{1,3})\.(\d{1,3})\.(\d{1,3})$)");
  std::smatch match;
  if (std::regex_match(normalized, match, ipv4_regex)) {
    std::array<uint16_t, 8> parts = {0};
    parts[0] = (std::stoi(match[1]) << 8) | std::stoi(match[2]);
    parts[1] = (std::stoi(match[3]) << 8) | std::stoi(match[4]);
    
    return ParsedIpAddress{
      IpKind::IPV4,
      normalized,
      parts
    };
  }
  
  // Try to parse as IPv6 (simplified)
  if (normalized.find(':') != std::string::npos) {
    return ParsedIpAddress{
      IpKind::IPV6,
      normalized,
      {0, 0, 0, 0, 0, 0, 0, 0}  // Simplified: would need full IPv6 parser
    };
  }
  
  return std::nullopt;
}

std::optional<ParsedIpAddress> parse_loose_ip_address(const std::string& raw) {
  // Lenient parsing (same as canonical for now)
  return parse_canonical_ip_address(raw);
}

std::optional<std::string> normalize_ip_address(const std::string& raw) {
  auto parsed = parse_canonical_ip_address(raw);
  if (!parsed) {
    return std::nullopt;
  }
  
  std::string normalized = parsed->normalized;
  std::transform(normalized.begin(), normalized.end(), normalized.begin(), ::tolower);
  return normalized;
}

bool is_canonical_dotted_decimal_ipv4(const std::string& raw) {
  std::string trimmed = raw;
  trimmed.erase(0, trimmed.find_first_not_of(" \t\n\r\f\v"));
  trimmed.erase(trimmed.find_last_not_of(" \t\n\r\f\v") + 1);
  
  std::string normalized = strip_ipv6_brackets(trimmed);
  std::regex ipv4_regex(R"(^(\d{1,3})\.(\d{1,3})\.(\d{1,3})\.(\d{1,3})$)");
  std::smatch match;
  
  if (!std::regex_match(normalized, match, ipv4_regex)) {
    return false;
  }
  
  // Check octet ranges
  for (size_t i = 1; i <= 4; ++i) {
    int octet = std::stoi(match[i]);
    if (octet < 0 || octet > 255) {
      return false;
    }
  }
  
  return true;
}

bool is_legacy_ipv4_literal(const std::string& raw) {
  std::string trimmed = raw;
  trimmed.erase(0, trimmed.find_first_not_of(" \t\n\r\f\v"));
  trimmed.erase(trimmed.find_last_not_of(" \t\n\r\f\v") + 1);
  
  if (trimmed.empty()) {
    return false;
  }
  
  std::string normalized = strip_ipv6_brackets(trimmed);
  if (normalized.find(':') != std::string::npos) {
    return false;
  }
  
  if (is_canonical_dotted_decimal_ipv4(normalized)) {
    return false;
  }
  
  // Check for hex (0x...) or octal (0...) formats
  return std::regex_match(normalized, std::regex(R"(^0x[0-9a-fA-F]+$)")) ||
         std::regex_match(normalized, std::regex(R"(^0[0-7]+$)")) ||
         std::regex_match(normalized, std::regex(R"(^\d+$)"));
}

bool is_loopback_ip_address(const std::string& raw) {
  auto parsed = parse_canonical_ip_address(raw);
  if (!parsed) {
    return false;
  }
  return parsed->range() == "loopback";
}

bool is_private_or_loopback_ip_address(const std::string& raw) {
  auto parsed = parse_canonical_ip_address(raw);
  if (!parsed) {
    return false;
  }
  std::string range = parsed->range();
  return range == "loopback" || range == "private" || range == "linkLocal" || range == "uniqueLocal";
}

bool is_rfc1918_ipv4_address(const std::string& raw) {
  auto parsed = parse_canonical_ip_address(raw);
  if (!parsed || parsed->kind != IpKind::IPV4) {
    return false;
  }
  return parsed->range() == "private";
}

bool is_blocked_special_use_ipv6_address(const ParsedIpAddress& addr) {
  if (addr.kind != IpKind::IPV6) {
    return false;
  }
  std::string range = addr.range();
  return range == "unspecified" || range == "loopback" || range == "linkLocal" ||
         range == "uniqueLocal" || range == "multicast";
}

// ============================================================================
// ANSI Terminal Utilities Implementation
// ============================================================================

std::string strip_ansi(const std::string& input) {
  // Remove ANSI SGR codes: ESC[...m
  std::regex ansi_sgr(R"(\x1b\[[0-9;]*m)");
  std::string result = std::regex_replace(input, ansi_sgr, "");
  
  // Remove OSC-8 hyperlinks: ESC]8;;...ESC (backslash)
  std::regex osc8_regex(R"(\x1b\]8;;.*?\x1b\\|\x1b\]8;;\x1b\\)");
  result = std::regex_replace(result, osc8_regex, "");
  
  return result;
}

std::vector<std::string> split_graphemes(const std::string& input) {
  // Simplified: split by code points (UTF-8 aware)
  std::vector<std::string> graphemes;
  
  for (size_t i = 0; i < input.size(); ) {
    unsigned char c = input[i];
    size_t len = 1;
    
    // UTF-8 multi-byte detection
    if ((c & 0x80) == 0) {
      len = 1;  // ASCII
    } else if ((c & 0xE0) == 0xC0) {
      len = 2;  // 2-byte
    } else if ((c & 0xF0) == 0xE0) {
      len = 3;  // 3-byte
    } else if ((c & 0xF8) == 0xF0) {
      len = 4;  // 4-byte
    }
    
    if (i + len <= input.size()) {
      graphemes.push_back(input.substr(i, len));
    }
    i += len;
  }
  
  return graphemes;
}

bool is_zero_width_code_point(uint32_t codepoint) {
  return (codepoint >= 0x0300 && codepoint <= 0x036F) ||
         (codepoint >= 0x1AB0 && codepoint <= 0x1AFF) ||
         (codepoint >= 0x1DC0 && codepoint <= 0x1DFF) ||
         (codepoint >= 0x20D0 && codepoint <= 0x20FF) ||
         (codepoint >= 0xFE20 && codepoint <= 0xFE2F) ||
         (codepoint >= 0xFE00 && codepoint <= 0xFE0F) ||
         (codepoint == 0x200D);
}

bool is_full_width_code_point(uint32_t codepoint) {
  if (codepoint < 0x1100) {
    return false;
  }
  return (codepoint <= 0x115F) ||
         (codepoint == 0x2329 || codepoint == 0x232A) ||
         (codepoint >= 0x2E80 && codepoint <= 0x3247 && codepoint != 0x303F) ||
         (codepoint >= 0x3250 && codepoint <= 0x4DBF) ||
         (codepoint >= 0x4E00 && codepoint <= 0xA4C6) ||
         (codepoint >= 0xA960 && codepoint <= 0xA97C) ||
         (codepoint >= 0xAC00 && codepoint <= 0xD7A3) ||
         (codepoint >= 0xF900 && codepoint <= 0xFAFF) ||
         (codepoint >= 0xFE10 && codepoint <= 0xFE19) ||
         (codepoint >= 0xFE30 && codepoint <= 0xFE6B) ||
         (codepoint >= 0xFF01 && codepoint <= 0xFF60) ||
         (codepoint >= 0xFFE0 && codepoint <= 0xFFE6) ||
         (codepoint >= 0x1AFF0 && codepoint <= 0x1AFF3) ||
         (codepoint >= 0x1AFF5 && codepoint <= 0x1AFFB) ||
         (codepoint >= 0x1AFFD && codepoint <= 0x1AFFE) ||
         (codepoint >= 0x1B000 && codepoint <= 0x1B2FF) ||
         (codepoint >= 0x1F200 && codepoint <= 0x1F251) ||
         (codepoint >= 0x20000 && codepoint <= 0x3FFFD);
}

size_t grapheme_width(const std::string& grapheme) {
  if (grapheme.empty()) {
    return 0;
  }
  
  // Simplified: check first code point
  uint32_t codepoint = static_cast<unsigned char>(grapheme[0]);
  
  // UTF-8 decode (simplified)
  if ((codepoint & 0x80) == 0) {
    // ASCII: 1-byte
  } else if ((codepoint & 0xE0) == 0xC0) {
    // 2-byte UTF-8
    codepoint = ((codepoint & 0x1F) << 6) | (static_cast<unsigned char>(grapheme[1]) & 0x3F);
  } else if ((codepoint & 0xF0) == 0xE0) {
    // 3-byte UTF-8
    codepoint = ((codepoint & 0x0F) << 12) |
                ((static_cast<unsigned char>(grapheme[1]) & 0x3F) << 6) |
                (static_cast<unsigned char>(grapheme[2]) & 0x3F);
  } else if ((codepoint & 0xF8) == 0xF0) {
    // 4-byte UTF-8
    codepoint = ((codepoint & 0x07) << 18) |
                ((static_cast<unsigned char>(grapheme[1]) & 0x3F) << 12) |
                ((static_cast<unsigned char>(grapheme[2]) & 0x3F) << 6) |
                (static_cast<unsigned char>(grapheme[3]) & 0x3F);
  }
  
  if (is_zero_width_code_point(codepoint)) {
    return 0;
  }
  if (is_full_width_code_point(codepoint)) {
    return 2;
  }
  return 1;
}

size_t visible_width(const std::string& input) {
  std::string stripped = strip_ansi(input);
  auto graphemes = split_graphemes(stripped);
  
  size_t width = 0;
  for (const auto& g : graphemes) {
    width += grapheme_width(g);
  }
  return width;
}

std::string sanitize_for_log(const std::string& value) {
  std::string out = strip_ansi(value);
  
  // Remove C0 control characters (0x00-0x1F) and DEL (0x7F)
  std::string result;
  for (char c : out) {
    unsigned char uc = static_cast<unsigned char>(c);
    if (uc >= 0x20 && uc != 0x7F) {
      result += c;
    }
  }
  
  return result;
}

// ============================================================================
// Chat Content Utilities Implementation
// ============================================================================

std::optional<std::string> extract_text_from_chat_content(
    const std::string& content,
    const std::function<std::string(const std::string&)>& sanitize_text,
    const std::string& join_with) {
  
  std::string value = sanitize_text ? sanitize_text(content) : content;
  
  // Normalize whitespace
  std::regex ws_regex(R"(\s+)");
  std::string normalized = std::regex_replace(value, ws_regex, " ");
  
  // Trim
  normalized.erase(0, normalized.find_first_not_of(" \t\n\r\f\v"));
  normalized.erase(normalized.find_last_not_of(" \t\n\r\f\v") + 1);
  
  if (normalized.empty()) {
    return std::nullopt;
  }
  
  return normalized;
}

std::vector<CodeRegion> find_code_regions(const std::string& text) {
  std::vector<CodeRegion> regions;
  std::regex code_fence(R"(```([a-zA-Z0-9]*)\n(.*?)\n```)");
  
  auto begin = std::sregex_iterator(text.begin(), text.end(), code_fence);
  auto end = std::sregex_iterator();
  
  for (auto it = begin; it != end; ++it) {
    std::smatch match = *it;
    regions.push_back({
      static_cast<size_t>(match.position()),
      static_cast<size_t>(match.position() + match.length()),
      match[1].str()
    });
  }
  
  return regions;
}

bool is_inside_code(size_t pos, const std::vector<CodeRegion>& regions) {
  for (const auto& region : regions) {
    if (pos >= region.start && pos < region.end) {
      return true;
    }
  }
  return false;
}

std::string strip_reasoning_tags_from_text(
    const std::string& text,
    const std::string& mode,
    const std::string& trim) {
  // Simplified: strip <thinking>...</thinking> tags (C++17 compatible - use [\s\S] to match any char including newlines)
  std::regex thinking_regex(R"(<thinking>[\s\S]*?</thinking>)");
  std::string result = std::regex_replace(text, thinking_regex, "");
  
  if (trim == "start") {
    result.erase(0, result.find_first_not_of(" \t\n\r\f\v"));
  } else if (trim == "end") {
    result.erase(result.find_last_not_of(" \t\n\r\f\v") + 1);
  } else if (trim == "both") {
    result.erase(0, result.find_first_not_of(" \t\n\r\f\v"));
    result.erase(result.find_last_not_of(" \t\n\r\f\v") + 1);
  }
  
  return result;
}

std::string strip_relevant_memories_tags(const std::string& text) {
  std::regex memory_regex(R"(<\s*/?relevant[-_]memories\b[^<>]*>)");
  return std::regex_replace(text, memory_regex, "");
}

std::string strip_assistant_internal_scaffolding(const std::string& text) {
  std::string without_reasoning = strip_reasoning_tags_from_text(text, "preserve", "start");
  std::string result = strip_relevant_memories_tags(without_reasoning);
  
  // Trim start
  result.erase(0, result.find_first_not_of(" \t\n\r\f\v"));
  return result;
}

// ============================================================================
// Config Evaluation Implementation
// ============================================================================

bool is_truthy(const std::string& value) {
  return !value.empty();
}

bool is_truthy(bool value) {
  return value;
}

bool is_truthy(int value) {
  return value != 0;
}

std::optional<std::string> resolve_config_path(
    const std::unordered_map<std::string, std::string>& config,
    const std::string& path_str) {
  
  // Split path by '.'
  std::vector<std::string> parts;
  std::stringstream ss(path_str);
  std::string part;
  while (std::getline(ss, part, '.')) {
    if (!part.empty()) {
      parts.push_back(part);
    }
  }
  
  // Simple lookup (flat config)
  if (config.find(path_str) != config.end()) {
    return config.at(path_str);
  }
  
  return std::nullopt;
}

bool is_config_path_truthy_with_defaults(
    const std::unordered_map<std::string, std::string>& config,
    const std::string& path_str,
    const std::unordered_map<std::string, bool>& defaults) {
  
  auto value = resolve_config_path(config, path_str);
  if (!value && defaults.find(path_str) != defaults.end()) {
    return defaults.at(path_str);
  }
  return value && is_truthy(*value);
}

bool evaluate_runtime_requires(const RuntimeRequirementEvalParams& params) {
  const auto& requires = params.requires;
  
  // Check required bins
  for (const auto& bin : requires.bins) {
    if (!params.has_bin(bin) && !params.has_remote_bin(bin)) {
      return false;
    }
  }
  
  // Check any required bins
  if (!requires.any_bins.empty()) {
    bool any_found = false;
    for (const auto& bin : requires.any_bins) {
      if (params.has_bin(bin)) {
        any_found = true;
        break;
      }
    }
    if (!any_found && !params.has_any_remote_bin(requires.any_bins)) {
      return false;
    }
  }
  
  // Check required env vars
  for (const auto& env : requires.env) {
    if (!params.has_env(env)) {
      return false;
    }
  }
  
  // Check required config paths
  for (const auto& config_path : requires.config) {
    if (!params.is_config_path_truthy(config_path)) {
      return false;
    }
  }
  
  return true;
}

// ============================================================================
// Terminal Table Rendering Implementation
// ============================================================================

std::string repeat_char(char ch, size_t n) {
  return std::string(n, ch);
}

std::string pad_cell(const std::string& text, size_t width, TableAlign align) {
  size_t w = visible_width(text);
  if (w >= width) {
    return text;
  }
  
  size_t pad = width - w;
  if (align == TableAlign::RIGHT) {
    return repeat_char(' ', pad) + text;
  } else if (align == TableAlign::CENTER) {
    size_t left = pad / 2;
    size_t right = pad - left;
    return repeat_char(' ', left) + text + repeat_char(' ', right);
  }
  return text + repeat_char(' ', pad);
}

std::vector<std::string> wrap_line(const std::string& text, size_t width) {
  if (width == 0) {
    return {text};
  }
  
  // Simplified: split by spaces
  std::vector<std::string> lines;
  std::string current;
  std::stringstream ss(text);
  std::string word;
  
  while (ss >> word) {
    if (current.empty()) {
      current = word;
    } else if (visible_width(current + " " + word) <= width) {
      current += " " + word;
    } else {
      lines.push_back(current);
      current = word;
    }
  }
  
  if (!current.empty()) {
    lines.push_back(current);
  }
  
  return lines;
}

TableBorder resolve_default_border(
    const std::string& platform,
    const std::unordered_map<std::string, std::string>& env) {
  
  if (platform != "win32") {
    return TableBorder::UNICODE;
  }
  
  std::string term = env.count("TERM") ? env.at("TERM") : "";
  std::string term_program = env.count("TERM_PROGRAM") ? env.at("TERM_PROGRAM") : "";
  
  bool is_modern = env.count("WT_SESSION") || 
                   term.find("xterm") != std::string::npos ||
                   term.find("cygwin") != std::string::npos ||
                   term.find("msys") != std::string::npos ||
                   term_program == "vscode";
  
  return is_modern ? TableBorder::UNICODE : TableBorder::ASCII;
}

std::string render_table(const RenderTableOptions& options) {
  // Simplified table rendering
  std::stringstream ss;
  
  // Calculate column widths
  std::vector<size_t> widths;
  for (const auto& col : options.columns) {
    size_t max_width = visible_width(col.header);
    for (const auto& row : options.rows) {
      if (row.count(col.key)) {
        max_width = std::max(max_width, visible_width(row.at(col.key)));
      }
    }
    widths.push_back(max_width + options.padding * 2);
  }
  
  // Border characters
  std::string h_line, v_line, corner;
  if (options.border == TableBorder::UNICODE) {
    h_line = "─";
    v_line = "│";
    corner = "┼";
  } else if (options.border == TableBorder::ASCII) {
    h_line = "-";
    v_line = "|";
    corner = "+";
  } else {
    h_line = v_line = corner = "";
  }
  
  // Header
  if (!v_line.empty()) {
    ss << v_line << " ";
  }
  for (size_t i = 0; i < options.columns.size(); ++i) {
    ss << pad_cell(options.columns[i].header, widths[i], options.columns[i].align);
    if (i + 1 < options.columns.size()) {
      ss << " " << v_line << " ";
    }
  }
  if (!v_line.empty()) {
    ss << " " << v_line;
  }
  ss << "\n";
  
  // Separator
  if (!h_line.empty()) {
    for (size_t i = 0; i < options.columns.size(); ++i) {
      ss << repeat_char(h_line[0], widths[i] + 2);
      if (i + 1 < options.columns.size()) {
        ss << corner;
      }
    }
    ss << "\n";
  }
  
  // Rows
  for (const auto& row : options.rows) {
    if (!v_line.empty()) {
      ss << v_line << " ";
    }
    for (size_t i = 0; i < options.columns.size(); ++i) {
      std::string value = row.count(options.columns[i].key) ? 
                          row.at(options.columns[i].key) : "";
      ss << pad_cell(value, widths[i], options.columns[i].align);
      if (i + 1 < options.columns.size()) {
        ss << " " << v_line << " ";
      }
    }
    if (!v_line.empty()) {
      ss << " " << v_line;
    }
    ss << "\n";
  }
  
  return ss.str();
}

// ============================================================================
// Terminal Theme & Palette Implementation
// ============================================================================

const LobsterPalette& get_lobster_palette() {
  static LobsterPalette palette;
  return palette;
}

std::string colorize_text(
    const std::string& text,
    const std::string& color_hex,
    bool rich) {
  if (!rich) {
    return text;
  }
  
  // Convert hex to RGB (simplified)
  // Return text with ANSI codes (placeholder)
  return "\x1b[38;2;255;90;45m" + text + "\x1b[0m";
}

bool is_rich_terminal() {
  // Check for color support
  const char* term = std::getenv("TERM");
  const char* no_color = std::getenv("NO_COLOR");
  const char* force_color = std::getenv("FORCE_COLOR");
  
  if (force_color && *force_color != '\0' && *force_color != '0') {
    return true;
  }
  if (no_color) {
    return false;
  }
  if (term && std::string(term).find("color") != std::string::npos) {
    return true;
  }
  return false;
}

std::string theme_accent(const std::string& text) {
  return colorize_text(text, get_lobster_palette().accent, is_rich_terminal());
}

std::string theme_success(const std::string& text) {
  return colorize_text(text, get_lobster_palette().success, is_rich_terminal());
}

std::string theme_warn(const std::string& text) {
  return colorize_text(text, get_lobster_palette().warn, is_rich_terminal());
}

std::string theme_error(const std::string& text) {
  return colorize_text(text, get_lobster_palette().error, is_rich_terminal());
}

std::string theme_muted(const std::string& text) {
  return colorize_text(text, get_lobster_palette().muted, is_rich_terminal());
}

std::string theme_heading(const std::string& text) {
  return colorize_text(text, get_lobster_palette().accent, is_rich_terminal());
}

// ============================================================================
// Text Processing Implementation
// ============================================================================

std::string join_segments(
    const std::vector<std::string>& segments,
    const std::string& separator) {
  std::stringstream ss;
  for (size_t i = 0; i < segments.size(); ++i) {
    ss << segments[i];
    if (i + 1 < segments.size()) {
      ss << separator;
    }
  }
  return ss.str();
}

std::string sample_text(const std::string& text, size_t max_length) {
  if (text.size() <= max_length) {
    return text;
  }
  return text.substr(0, max_length - 3) + "...";
}

}  // namespace shared
}  // namespace hunterclaw
