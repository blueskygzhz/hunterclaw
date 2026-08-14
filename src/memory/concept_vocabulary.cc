#include "hunterclaw/memory/concept_vocabulary.h"
#include <algorithm>
#include <cctype>
#include <regex>
#include <set>
#include <sstream>
#include <filesystem>

// 严格对齐 OpenClaw extensions/memory-core/src/concept-vocabulary.ts (474 行)

namespace hunterclaw {
namespace memory {

// ============================================================================
// Stop words (对齐 concept-vocabulary.ts:15-226 LANGUAGE_STOP_WORDS)
// ============================================================================

static const std::set<std::string>& get_concept_stop_words() {
    static const std::set<std::string> STOP_WORDS = {
        // shared (对齐 concept-vocabulary.ts:17-86)
        "about", "after", "agent", "again", "also", "assistant", "because",
        "before", "being", "between", "build", "called", "could", "daily",
        "default", "deploy", "during", "every", "file", "files", "from",
        "have", "into", "just", "line", "lines", "long", "main", "make",
        "memory", "month", "more", "most", "move", "much", "next",
        "note", "notes", "over", "part", "past", "port", "same",
        "score", "search", "session", "sessions", "short", "should",
        "since", "some", "subagent", "system", "than", "that", "their",
        "there", "these", "they", "this", "through", "today", "user",
        "using", "with", "work", "workspace", "year",
        // english (对齐:87)
        "and", "are", "for", "into", "its", "our", "the", "then", "were",
        "you", "your",
        // spanish (对齐:88-112)
        "al", "con", "como", "de", "del", "el", "en", "es", "la",
        "las", "los", "para", "por", "que", "se", "sin", "su", "sus",
        "una", "uno", "unos", "unas", "y",
        // french (对齐:113-137)
        "au", "aux", "avec", "dans", "des", "du", "est", "et", "le",
        "les", "ou", "pour", "qui", "sans", "ses", "son", "sur", "un", "une",
        // german (对齐:138-166；für/über 为 UTF-8)
        "auf", "aus", "bei", "das", "dem", "den", "der", "die",
        "ein", "eine", "einem", "einen", "einer",
        "\x66\xc3\xbc\x72",  // für
        "im", "in", "mit", "nach", "oder", "ohne",
        "\xc3\xbc\x62\x65\x72",  // über
        "und", "von", "zu", "zum", "zur",
        // path noise (对齐:213-229)
        "cjs", "cpp", "cts", "jsx", "json", "md", "mjs", "mts",
        "text", "toml", "ts", "tsx", "txt", "yaml", "yml",
    };
    return STOP_WORDS;
}

// CJK stop words stored separately (UTF-8 strings)
static const std::set<std::string>& get_cjk_stop_words() {
    static const std::set<std::string> CJK_STOPS = {
        // Japanese particles (対齐:162-177)
        "\xe3\x81\x8c", "\xe3\x81\x8b\xe3\x82\x89", "\xe3\x81\x99\xe3\x82\x8b",
        "\xe3\x81\x97\xe3\x81\xa6", "\xe3\x81\x97\xe3\x81\x9f", "\xe3\x81\xa7",
        "\xe3\x81\xa8", "\xe3\x81\xab", "\xe3\x81\xae", "\xe3\x81\xaf",
        "\xe3\x81\xb8", "\xe3\x81\xbe\xe3\x81\xa7", "\xe3\x82\x82",
        "\xe3\x82\x84", "\xe3\x82\x92",
        // Chinese (対齐:178-189)
        "\xe4\xb8\x8e", "\xe4\xb8\xba", "\xe4\xba\x86", "\xe5\x8f\x8a",
        "\xe5\x92\x8c", "\xe5\x9c\xa8", "\xe5\xb0\x86", "\xe6\x88\x96",
        "\xe6\x8a\x8a", "\xe6\x98\xaf", "\xe7\x94\xa8", "\xe7\x9a\x84",
        // Korean (対齐:190-208)
        "\xea\xb3\xbc", "\xeb\x8a\x94", "\xeb\x8f\x84", "\xeb\xa1\x9c",
        "\xeb\xa5\xbc", "\xec\x97\x90", "\xec\x97\x90\xec\x84\x9c",
        "\xec\x99\x80", "\xec\x9d\x80", "\xec\x9c\xbc\xeb\xa1\x9c",
        "\xec\x9d\x84", "\xec\x9d\xb4", "\xed\x95\x98\xeb\x8b\xa4",
        "\xed\x95\x9c", "\xed\x95\xa0", "\xed\x95\xb4",
        "\xed\x96\x88\xeb\x8b\xa4",
    };
    return CJK_STOPS;
}

// Protected glossary (対齐 concept-vocabulary.ts:238-281，共 41 条)
static const std::vector<std::string>& get_protected_glossary() {
    static const std::vector<std::string> GLOSSARY = {
        "backup", "backups", "embedding", "embeddings", "failover",
        "gateway", "glacier", "gpt", "kv", "network", "openai",
        "qmd", "router", "s3", "vlan",
        // multilingual (对齐:254-263)
        "sauvegarde", "routeur", "passerelle",
        "konfiguration", "sicherung",
        "\xc3\xbc\x62\x65\x72\x77\x61\x63\x68\x75\x6e\x67",  // überwachung
        "\x63\x6f\x6e\x66\x69\x67\x75\x72\x61\x63\x69\xc3\xb3\x6e",  // configuración
        "respaldo", "enrutador",
        "puerta-de-enlace",
        // Japanese glossary (对齐:264-269)
        "\xe3\x83\x90\xe3\x83\x83\xe3\x82\xaf\xe3\x82\xa2\xe3\x83\x83\xe3\x83\x97",  // バックアップ
        "\xe3\x83\x95\xe3\x82\xa7\xe3\x82\xa4\xe3\x83\xab\xe3\x82\xaa\xe3\x83\xbc\xe3\x83\x90\xe3\x83\xbc",  // フェイルオーバー
        "\xe3\x83\xab\xe3\x83\xbc\xe3\x82\xbf\xe3\x83\xbc",  // ルーター
        "\xe3\x83\x8d\xe3\x83\x83\xe3\x83\x88\xe3\x83\xaf\xe3\x83\xbc\xe3\x82\xaf",  // ネットワーク
        "\xe3\x82\xb2\xe3\x83\xbc\xe3\x83\x88\xe3\x82\xa6\xe3\x82\xa7\xe3\x82\xa4",  // ゲートウェイ
        "\xe9\x9a\x9c\xe5\xae\xb3\xe5\xaf\xbe\xe5\xbf\x9c",  // 障害対応
        // Chinese glossary (对齐:270-274)
        "\xe8\xb7\xaf\xe7\x94\xb1\xe5\x99\xa8",  // 路由器
        "\xe5\xa4\x87\xe4\xbb\xbd",  // 备份
        "\xe6\x95\x85\xe9\x9a\x9c\xe8\xbd\xac\xe7\xa7\xbb",  // 故障转移
        "\xe7\xbd\x91\xe7\xbb\x9c",  // 网络
        "\xe7\xbd\x91\xe5\x85\xb3",  // 网关
        // Korean glossary (对齐:275-280)
        "\xeb\x9d\xbc\xec\x9a\xb0\xed\x84\xb0",  // 라우터
        "\xeb\xb0\xb1\xec\x97\x85",  // 백업
        "\xed\x8e\x98\xec\x9d\xbc\xec\x98\xa4\xeb\xb2\x84",  // 페일오버
        "\xeb\x84\xa4\xed\x8a\xb8\xec\x9b\x8c\xed\x81\xac",  // 네트워크
        "\xea\xb2\x8c\xec\x9d\xb4\xed\x8a\xb8\xec\x9b\xa8\xec\x9d\xb4",  // 게이트웨이
        "\xec\x9e\xa5\xec\x95\xa0\xeb\x8c\x80\xec\x9d\x91",  // 장애대응
    };
    return GLOSSARY;
}

// ============================================================================
// Helper: normalize lowercase (对齐 normalizeLowercaseStringOrEmpty)
// ============================================================================
std::string normalize_lowercase(const std::string& input) {
    std::string result;
    result.reserve(input.size());
    for (unsigned char c : input) {
        result.push_back(static_cast<char>(std::tolower(c)));
    }
    // trim whitespace
    auto start = result.find_first_not_of(" \t\n\r");
    if (start == std::string::npos) return "";
    auto end = result.find_last_not_of(" \t\n\r");
    return result.substr(start, end - start + 1);
}

// ============================================================================
// Script detection helpers (対齐 concept-vocabulary.ts:279-327)
// ============================================================================

static bool has_latin(const std::string& s) {
    for (unsigned char c : s) {
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z')) return true;
    }
    return false;
}

static bool has_cjk(const std::string& s) {
    const auto* p = reinterpret_cast<const unsigned char*>(s.data());
    const auto* end = p + s.size();
    while (p < end) {
        unsigned char c = *p;
        if (c < 0x80) { p++; continue; }
        // Decode UTF-8 to codepoint
        uint32_t cp = 0;
        int len = 1;
        if ((c & 0xE0) == 0xC0) { cp = c & 0x1F; len = 2; }
        else if ((c & 0xF0) == 0xE0) { cp = c & 0x0F; len = 3; }
        else if ((c & 0xF8) == 0xF0) { cp = c & 0x07; len = 4; }
        else { p++; continue; }
        for (int i = 1; i < len && (p + i) < end; i++)
            cp = (cp << 6) | (p[i] & 0x3F);
        p += len;
        // CJK Unified Ideographs (Han)
        if (cp >= 0x4E00 && cp <= 0x9FFF) return true;
        // Hiragana
        if (cp >= 0x3040 && cp <= 0x309F) return true;
        // Katakana
        if (cp >= 0x30A0 && cp <= 0x30FF) return true;
        // Hangul Syllables
        if (cp >= 0xAC00 && cp <= 0xD7AF) return true;
    }
    return false;
}

static bool contains_letter_or_number(const std::string& s) {
    for (unsigned char c : s) {
        if (std::isalnum(c)) return true;
    }
    // Also check non-ASCII (CJK etc)
    return has_cjk(s);
}

// UTF-8 字符（code point）计数 —— 用于按字符判定最小长度（対齐 JS string.length 近似）。
static int utf8_char_count(const std::string& s) {
    int count = 0;
    for (size_t i = 0; i < s.size(); ) {
        unsigned char c = static_cast<unsigned char>(s[i]);
        int len = 1;
        if ((c & 0x80) == 0x00) len = 1;
        else if ((c & 0xE0) == 0xC0) len = 2;
        else if ((c & 0xF0) == 0xE0) len = 3;
        else if ((c & 0xF8) == 0xF0) len = 4;
        i += len;
        count++;
    }
    return count;
}

// 判定：仅含假名（无汉字/谚文），至少含平/片假名之一（対齐 concept-vocabulary.ts:325-331）。
static bool is_kana_only_token(const std::string& s) {
    const auto* p = reinterpret_cast<const unsigned char*>(s.data());
    const auto* end = p + s.size();
    bool has_han = false, has_hangul = false, has_kana = false;
    while (p < end) {
        unsigned char c = *p;
        if (c < 0x80) { p++; continue; }
        uint32_t cp = 0; int len = 1;
        if ((c & 0xE0) == 0xC0) { cp = c & 0x1F; len = 2; }
        else if ((c & 0xF0) == 0xE0) { cp = c & 0x0F; len = 3; }
        else if ((c & 0xF8) == 0xF0) { cp = c & 0x07; len = 4; }
        else { p++; continue; }
        for (int i = 1; i < len && (p + i) < end; i++)
            cp = (cp << 6) | (p[i] & 0x3F);
        p += len;
        if (cp >= 0x4E00 && cp <= 0x9FFF) has_han = true;
        else if (cp >= 0xAC00 && cp <= 0xD7AF) has_hangul = true;
        else if ((cp >= 0x3040 && cp <= 0x309F) || (cp >= 0x30A0 && cp <= 0x30FF)) has_kana = true;
    }
    return !has_han && !has_hangul && has_kana;
}

static int minimum_token_length_for_script(ConceptTagScriptFamily script) {
    return script == ConceptTagScriptFamily::Cjk ? 2 : 3;
}

// ============================================================================
// classifyConceptTagScript (対齐 concept-vocabulary.ts:294-312)
// ============================================================================

ConceptTagScriptFamily classify_concept_tag_script(const std::string& tag) {
    bool latin = has_latin(tag);
    bool cjk = has_cjk(tag);
    if (latin && cjk) return ConceptTagScriptFamily::Mixed;
    if (cjk) return ConceptTagScriptFamily::Cjk;
    if (latin) return ConceptTagScriptFamily::Latin;
    return ConceptTagScriptFamily::Other;
}

// ============================================================================
// normalizeConceptToken (対齐 concept-vocabulary.ts:329-357)
// ============================================================================

std::string normalize_concept_token(const std::string& raw_token, bool from_glossary) {
    std::string normalized = normalize_lowercase(raw_token);
    // Replace underscore with hyphen (対齐:338)
    std::replace(normalized.begin(), normalized.end(), '_', '-');
    // Trim leading/trailing non-alphanumeric
    auto start = normalized.find_first_of(
        "abcdefghijklmnopqrstuvwxyz0123456789");
    if (start == std::string::npos) {
        // Check for non-ASCII content (CJK)
        if (!contains_letter_or_number(normalized)) return "";
        // Keep as-is for CJK
    } else {
        normalized = normalized.substr(start);
    }

    if (normalized.empty() || !contains_letter_or_number(normalized) ||
        utf8_char_count(normalized) > 32) {
        return "";
    }
    // Reject pure numbers (対齐:344)
    bool all_digits = true;
    for (char c : normalized) {
        if (!std::isdigit(static_cast<unsigned char>(c)) && c != '-') {
            all_digits = false;
            break;
        }
    }
    if (all_digits) return "";

    // Reject date patterns (対齐:345-346，含带扩展名的第 3 条)
    static const std::regex date_re(R"(^\d{4}-\d{2}-\d{2}$)");
    static const std::regex date_ext_re(R"(^\d{4}-\d{2}-\d{2}\.[A-Za-z0-9]+$)");
    if (std::regex_match(normalized, date_re)) return "";
    if (std::regex_match(normalized, date_ext_re)) return "";

    // 最小长度检查（対齐:350-355）：glossary 词绕过，其余按 script 计字符数。
    auto script = classify_concept_tag_script(normalized);
    if (!from_glossary &&
        utf8_char_count(normalized) < minimum_token_length_for_script(script)) {
        return "";
    }
    // kana-only 且 <3 字符拒绝（対齐:356-358）
    if (is_kana_only_token(normalized) && utf8_char_count(normalized) < 3) {
        return "";
    }

    // Check stop words (対齐:359-361)
    if (get_concept_stop_words().count(normalized) > 0) return "";
    if (get_cjk_stop_words().count(normalized) > 0) return "";

    return normalized;
}

// ============================================================================
// Token collection helpers (対齐 concept-vocabulary.ts:359-393)
// ============================================================================

// Collect glossary matches (対齐:365-404)
//   短词（长度 < 其 script 最小长度，如 kv/s3）需 delimiter-bounded 整词匹配，
//   避免在 mkv/css3 内部误命中；较长词保留 substring 包含语义。
static bool is_alnum_at(const std::string& src, long index) {
    if (index < 0 || index >= static_cast<long>(src.size())) return false;
    unsigned char c = static_cast<unsigned char>(src[index]);
    // ASCII 字母数字，或任意非 ASCII 字节（保守视为字母，避免短词误命中 CJK 边界）。
    return std::isalnum(c) || c >= 0x80;
}

static bool includes_standalone_term(const std::string& src, const std::string& entry) {
    size_t from = src.find(entry);
    while (from != std::string::npos) {
        long before = static_cast<long>(from) - 1;
        long after = static_cast<long>(from + entry.size());
        if (!is_alnum_at(src, before) && !is_alnum_at(src, after)) return true;
        from = src.find(entry, from + 1);
    }
    return false;
}

static std::vector<std::string> collect_glossary_matches(const std::string& source) {
    std::string lower_source = normalize_lowercase(source);
    std::vector<std::string> matches;
    for (const auto& entry : get_protected_glossary()) {
        bool whole_word = utf8_char_count(entry)
            < minimum_token_length_for_script(classify_concept_tag_script(entry));
        bool present = whole_word
            ? includes_standalone_term(lower_source, entry)
            : (lower_source.find(entry) != std::string::npos);
        if (present) matches.push_back(entry);
    }
    return matches;
}

// Collect compound tokens (対齐:371-373)
// Pattern: sequences of letters/numbers joined by ._/-
static std::vector<std::string> collect_compound_tokens(const std::string& source) {
    std::vector<std::string> results;
    std::regex compound_re(R"([\w]+(?:[._/-][\w]+)+)");
    auto begin = std::sregex_iterator(source.begin(), source.end(), compound_re);
    auto end = std::sregex_iterator();
    for (auto it = begin; it != end; ++it) {
        results.push_back(it->str());
    }
    return results;
}

// Collect segment tokens (対齐:375-382)
// Split by non-letter/non-number characters
static std::vector<std::string> collect_segment_tokens(const std::string& source) {
    std::vector<std::string> tokens;
    std::string current;
    for (unsigned char c : source) {
        if (std::isalnum(c) || c == '_' || c >= 0x80) {
            current.push_back(static_cast<char>(c));
        } else {
            if (!current.empty()) {
                tokens.push_back(current);
                current.clear();
            }
        }
    }
    if (!current.empty()) tokens.push_back(current);
    return tokens;
}

// ============================================================================
// deriveConceptTags (対齐 concept-vocabulary.ts:395-419)
// ============================================================================

std::vector<std::string> derive_concept_tags(
    const std::string& path,
    const std::string& snippet,
    int limit
) {
    if (limit <= 0) return {};

    // 剥离 HTML 注释（対齐:440-442）：召回注解是控制元数据，不应派生为标签。
    static const std::regex HTML_COMMENT_RE(R"(<!--[\s\S]*?-->)");
    std::string visible_snippet = std::regex_replace(snippet, HTML_COMMENT_RE, " ");

    // source = basename(path) + " " + visibleSnippet (対齐:443)
    std::string basename = std::filesystem::path(path).filename().string();
    std::string source = basename + " " + visible_snippet;

    std::vector<std::string> tags;

    // Collect tokens in priority order (対齐:452-464)
    auto push_tag = [&](const std::string& raw_token, bool from_glossary) {
        std::string normalized = normalize_concept_token(raw_token, from_glossary);
        if (normalized.empty()) return;
        // Check for duplicates
        for (const auto& t : tags) {
            if (t == normalized) return;
        }
        tags.push_back(normalized);
        if (static_cast<int>(tags.size()) > limit) tags.resize(limit);
    };

    // 1. Glossary matches (highest priority；from_glossary=true 绕过长度门)
    for (const auto& token : collect_glossary_matches(source)) {
        push_tag(token, /*from_glossary=*/true);
        if (static_cast<int>(tags.size()) >= limit) return tags;
    }

    // 2. Compound tokens
    for (const auto& token : collect_compound_tokens(source)) {
        push_tag(token, /*from_glossary=*/false);
        if (static_cast<int>(tags.size()) >= limit) return tags;
    }

    // 3. Segment tokens
    for (const auto& token : collect_segment_tokens(source)) {
        push_tag(token, /*from_glossary=*/false);
        if (static_cast<int>(tags.size()) >= limit) return tags;
    }

    return tags;
}

// ============================================================================
// summarizeConceptTagScriptCoverage (対齐 concept-vocabulary.ts:422-466)
// ============================================================================

ConceptTagScriptCoverage summarize_concept_tag_script_coverage(
    const std::vector<std::vector<std::string>>& concept_tags_by_entry
) {
    ConceptTagScriptCoverage coverage;

    for (const auto& concept_tags : concept_tags_by_entry) {
        bool entry_has_latin = false;
        bool entry_has_cjk = false;
        bool entry_has_other = false;

        for (const auto& tag : concept_tags) {
            auto family = classify_concept_tag_script(tag);
            if (family == ConceptTagScriptFamily::Mixed) {
                entry_has_latin = true;
                entry_has_cjk = true;
            } else if (family == ConceptTagScriptFamily::Latin) {
                entry_has_latin = true;
            } else if (family == ConceptTagScriptFamily::Cjk) {
                entry_has_cjk = true;
            } else {
                entry_has_other = true;
            }
        }

        if (entry_has_latin && entry_has_cjk) {
            coverage.mixed_entry_count++;
        } else if (entry_has_cjk) {
            coverage.cjk_entry_count++;
        } else if (entry_has_latin) {
            coverage.latin_entry_count++;
        } else if (entry_has_other) {
            coverage.other_entry_count++;
        }
    }

    return coverage;
}

}  // namespace memory
}  // namespace hunterclaw
