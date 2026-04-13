// 对齐 OC: src/shared/net/ip.ts (354行) + src/terminal/ansi.ts (116行)
//          + src/terminal/note.ts (167行) + src/terminal/stream-writer.ts (69行)
// 网络安全辅助 + 终端 ANSI 处理 + 终端 note 格式化 + 安全流写入
#include <string>
#include <vector>
#include <regex>
#include <set>
#include <array>
#include <algorithm>
#include <cstdint>
#include <sstream>
#include <functional>
#include <optional>

namespace hunterclaw {

// =========================================================================
// 对齐 OC shared/net/ip.ts: IP Address Safety
// =========================================================================

namespace net {

// 对齐 OC: BLOCKED_IPV4_SPECIAL_USE_RANGES
enum class Ipv4Range {
    UNICAST, UNSPECIFIED, BROADCAST, MULTICAST, LINK_LOCAL,
    LOOPBACK, CARRIER_GRADE_NAT, PRIVATE, RESERVED
};

struct ParsedIpv4 {
    std::array<uint8_t, 4> octets{};
    bool valid = false;
};

// 对齐 OC: parseIpv4
static ParsedIpv4 parse_ipv4(const std::string& ip) {
    ParsedIpv4 result;
    std::regex re(R"(^(\d{1,3})\.(\d{1,3})\.(\d{1,3})\.(\d{1,3})$)");
    std::smatch match;
    if (!std::regex_match(ip, match, re)) return result;
    for (int i = 0; i < 4; ++i) {
        int val = std::stoi(match[i + 1].str());
        if (val < 0 || val > 255) return result;
        result.octets[i] = static_cast<uint8_t>(val);
    }
    result.valid = true;
    return result;
}

// 对齐 OC: classifyIpv4Range
static Ipv4Range classify_ipv4_range(const ParsedIpv4& ip) {
    if (!ip.valid) return Ipv4Range::UNICAST;
    uint8_t a = ip.octets[0], b = ip.octets[1];

    // 对齐 OC: special-use range classification
    if (a == 0 && b == 0 && ip.octets[2] == 0 && ip.octets[3] == 0) return Ipv4Range::UNSPECIFIED;
    if (a == 255 && b == 255 && ip.octets[2] == 255 && ip.octets[3] == 255) return Ipv4Range::BROADCAST;
    if (a == 127) return Ipv4Range::LOOPBACK;
    if (a == 10) return Ipv4Range::PRIVATE;
    if (a == 172 && b >= 16 && b <= 31) return Ipv4Range::PRIVATE;
    if (a == 192 && b == 168) return Ipv4Range::PRIVATE;
    if (a == 169 && b == 254) return Ipv4Range::LINK_LOCAL;
    if (a == 100 && b >= 64 && b <= 127) return Ipv4Range::CARRIER_GRADE_NAT;
    if (a >= 224 && a <= 239) return Ipv4Range::MULTICAST;
    if (a >= 240) return Ipv4Range::RESERVED;
    return Ipv4Range::UNICAST;
}

// 对齐 OC: isSpecialUseIpv4
bool is_special_use_ipv4(const std::string& ip) {
    auto parsed = parse_ipv4(ip);
    if (!parsed.valid) return false;
    auto range = classify_ipv4_range(parsed);
    static const std::set<Ipv4Range> blocked = {
        Ipv4Range::UNSPECIFIED, Ipv4Range::BROADCAST, Ipv4Range::MULTICAST,
        Ipv4Range::LINK_LOCAL, Ipv4Range::LOOPBACK,
        Ipv4Range::CARRIER_GRADE_NAT, Ipv4Range::PRIVATE, Ipv4Range::RESERVED
    };
    return blocked.count(range) > 0;
}

// 对齐 OC: isLoopbackIpAddress
bool is_loopback_ip_address(const std::string& ip) {
    auto parsed = parse_ipv4(ip);
    if (parsed.valid) return classify_ipv4_range(parsed) == Ipv4Range::LOOPBACK;
    // IPv6 loopback
    return ip == "::1" || ip == "0:0:0:0:0:0:0:1";
}

// 对齐 OC: isCanonicalDottedDecimalIPv4
bool is_canonical_dotted_decimal_ipv4(const std::string& ip) {
    auto parsed = parse_ipv4(ip);
    if (!parsed.valid) return false;
    // Ensure canonical form (no leading zeros)
    std::string canonical = std::to_string(parsed.octets[0]) + "." +
                            std::to_string(parsed.octets[1]) + "." +
                            std::to_string(parsed.octets[2]) + "." +
                            std::to_string(parsed.octets[3]);
    return ip == canonical;
}

// 对齐 OC: isPrivateOrLoopbackIp
bool is_private_or_loopback_ip(const std::string& ip) {
    auto parsed = parse_ipv4(ip);
    if (!parsed.valid) return false;
    auto range = classify_ipv4_range(parsed);
    static const std::set<Ipv4Range> private_loopback = {
        Ipv4Range::LOOPBACK, Ipv4Range::PRIVATE,
        Ipv4Range::LINK_LOCAL, Ipv4Range::CARRIER_GRADE_NAT
    };
    return private_loopback.count(range) > 0;
}

// 对齐 OC shared/net/redact-sensitive-url.ts
std::string redact_sensitive_url(const std::string& url) {
    // Redact userinfo (username:password@) and api_key query params
    std::regex userinfo_re(R"(://([^@/]+)@)");
    std::string result = std::regex_replace(url, userinfo_re, "://[REDACTED]@");

    std::regex api_key_re(R"(([\?&](?:api_?key|token|secret|password|auth)=)[^&]*)");
    result = std::regex_replace(result, api_key_re, "$1[REDACTED]");
    return result;
}

}  // namespace net

// =========================================================================
// 对齐 OC terminal/ansi.ts: ANSI Escape Handling
// =========================================================================

namespace terminal {

// 对齐 OC: ANSI regex patterns
static const std::regex ANSI_CSI_REGEX(R"(\x1b\[[\x20-\x3f]*[\x40-\x7e])");
static const std::regex OSC8_REGEX(R"(\x1b\]8;;.*?\x1b\\|\x1b\]8;;\x1b\\)");

// 对齐 OC: stripAnsi
std::string strip_ansi(const std::string& input) {
    std::string result = std::regex_replace(input, OSC8_REGEX, "");
    return std::regex_replace(result, ANSI_CSI_REGEX, "");
}

// 对齐 OC: sanitizeForLog
std::string sanitize_for_log(const std::string& input) {
    std::string out = strip_ansi(input);
    // Remove C0 control characters (U+0000–U+001F) and DEL (U+007F)
    std::string cleaned;
    cleaned.reserve(out.size());
    for (char c : out) {
        if (static_cast<unsigned char>(c) <= 0x1f || c == 0x7f) continue;
        cleaned += c;
    }
    return cleaned;
}

// 对齐 OC: isZeroWidthCodePoint
static bool is_zero_width_code_point(uint32_t cp) {
    return (cp >= 0x0300 && cp <= 0x036f) ||
           (cp >= 0x1ab0 && cp <= 0x1aff) ||
           (cp >= 0x1dc0 && cp <= 0x1dff) ||
           (cp >= 0x20d0 && cp <= 0x20ff) ||
           (cp >= 0xfe20 && cp <= 0xfe2f) ||
           (cp >= 0xfe00 && cp <= 0xfe0f) ||
           cp == 0x200d;
}

// 对齐 OC: isFullWidthCodePoint
static bool is_full_width_code_point(uint32_t cp) {
    if (cp < 0x1100) return false;
    return (cp >= 0x1100 && cp <= 0x115f) ||   // Hangul Jamo
           (cp >= 0x2e80 && cp <= 0xa4cf) ||   // CJK
           (cp >= 0xac00 && cp <= 0xd7a3) ||   // Hangul Syllables
           (cp >= 0xf900 && cp <= 0xfaff) ||   // CJK Compat Ideographs
           (cp >= 0xfe10 && cp <= 0xfe6f) ||   // CJK Compat Forms
           (cp >= 0xff01 && cp <= 0xff60) ||   // Fullwidth Forms
           (cp >= 0xffe0 && cp <= 0xffe6) ||   // Fullwidth Signs
           (cp >= 0x20000 && cp <= 0x2fffd) || // CJK Ext B-F
           (cp >= 0x30000 && cp <= 0x3fffd);   // CJK Ext G
}

// 对齐 OC: visibleWidth (simplified for ASCII + CJK)
int visible_width(const std::string& text) {
    std::string stripped = strip_ansi(text);
    int width = 0;
    for (size_t i = 0; i < stripped.size(); ++i) {
        unsigned char c = static_cast<unsigned char>(stripped[i]);
        if (c < 0x80) {
            // ASCII
            if (c >= 0x20) width += 1;
        } else if (c >= 0xc0 && c < 0xe0) {
            // 2-byte UTF-8
            width += 1;
            i += 1;
        } else if (c >= 0xe0 && c < 0xf0) {
            // 3-byte UTF-8 (includes CJK)
            if (i + 2 < stripped.size()) {
                uint32_t cp = ((c & 0x0f) << 12) |
                              ((static_cast<unsigned char>(stripped[i + 1]) & 0x3f) << 6) |
                              (static_cast<unsigned char>(stripped[i + 2]) & 0x3f);
                if (is_zero_width_code_point(cp)) {
                    // zero width
                } else if (is_full_width_code_point(cp)) {
                    width += 2;
                } else {
                    width += 1;
                }
            }
            i += 2;
        } else if (c >= 0xf0) {
            // 4-byte UTF-8
            if (i + 3 < stripped.size()) {
                uint32_t cp = ((c & 0x07) << 18) |
                              ((static_cast<unsigned char>(stripped[i + 1]) & 0x3f) << 12) |
                              ((static_cast<unsigned char>(stripped[i + 2]) & 0x3f) << 6) |
                              (static_cast<unsigned char>(stripped[i + 3]) & 0x3f);
                width += is_full_width_code_point(cp) ? 2 : 1;
            }
            i += 3;
        }
    }
    return width;
}

// =========================================================================
// 对齐 OC terminal/note.ts: Terminal Note Formatting
// =========================================================================

// 对齐 OC: isCopySensitiveToken
static bool is_copy_sensitive_token(const std::string& word) {
    if (word.empty()) return false;
    // URLs
    if (word.find("http://") == 0 || word.find("https://") == 0 ||
        word.find("file://") == 0) return true;
    // Paths
    if (word[0] == '/' || word.find("~/") == 0 ||
        word.find("./") == 0 || word.find("../") == 0) return true;
    // Windows paths
    if (word.size() >= 3 && std::isalpha(word[0]) && word[1] == ':' &&
        (word[2] == '/' || word[2] == '\\')) return true;
    // Contains path separators
    if (word.find('/') != std::string::npos ||
        word.find('\\') != std::string::npos) return true;
    return false;
}

// 对齐 OC: wrapText
std::string wrap_text(const std::string& text, int max_width) {
    if (max_width <= 0) max_width = 80;

    std::vector<std::string> result_lines;
    std::istringstream line_stream(text);
    std::string line;

    while (std::getline(line_stream, line)) {
        if (visible_width(line) <= max_width) {
            result_lines.push_back(line);
            continue;
        }

        // Word wrap
        std::istringstream word_stream(line);
        std::string word;
        std::string current_line;
        int current_width = 0;

        while (word_stream >> word) {
            int word_width = visible_width(word);
            if (current_width > 0 && current_width + 1 + word_width > max_width) {
                // Don't break copy-sensitive tokens
                if (!is_copy_sensitive_token(word) || current_width == 0) {
                    result_lines.push_back(current_line);
                    current_line = word;
                    current_width = word_width;
                    continue;
                }
            }
            if (!current_line.empty()) {
                current_line += " ";
                current_width += 1;
            }
            current_line += word;
            current_width += word_width;
        }
        if (!current_line.empty()) {
            result_lines.push_back(current_line);
        }
    }

    std::string result;
    for (size_t i = 0; i < result_lines.size(); ++i) {
        if (i > 0) result += "\n";
        result += result_lines[i];
    }
    return result;
}

// =========================================================================
// 对齐 OC terminal/stream-writer.ts: Safe Stream Writer
// =========================================================================

struct SafeStreamWriter {
    std::function<void()> before_write;
    std::function<void(const std::string&)> on_broken_pipe;
    bool closed = false;

    bool write(std::ostream& stream, const std::string& text) {
        if (closed) return false;
        if (before_write) {
            try { before_write(); }
            catch (...) { closed = true; return false; }
        }
        try {
            stream << text;
            if (stream.fail()) {
                closed = true;
                if (on_broken_pipe) on_broken_pipe("EPIPE");
                return false;
            }
            return true;
        } catch (...) {
            closed = true;
            return false;
        }
    }

    bool write_line(std::ostream& stream, const std::string& text) {
        return write(stream, text + "\n");
    }

    void reset() { closed = false; }
    bool is_closed() const { return closed; }
};

}  // namespace terminal
}  // namespace hunterclaw
