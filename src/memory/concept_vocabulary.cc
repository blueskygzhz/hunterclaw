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
        // shared (对齐 concept-vocabulary.ts:16-81)
        "about", "after", "agent", "again", "also", "because", "before",
        "being", "between", "build", "called", "could", "daily", "default",
        "deploy", "during", "every", "file", "files", "from", "have",
        "into", "just", "line", "lines", "long", "main", "make",
        "memory", "month", "more", "most", "move", "much", "next",
        "note", "notes", "over", "part", "past", "port", "same",
        "score", "search", "session", "sessions", "short", "should",
        "since", "some", "than", "that", "their", "there", "these",
        "they", "this", "through", "today", "using", "with", "work",
        "workspace", "year",
        // english (对齐:82)
        "and", "are", "for", "into", "its", "our", "then", "were",
        // spanish (对齐:83-107)
        "al", "con", "como", "de", "del", "el", "en", "es", "la",
        "las", "los", "para", "por", "que", "se", "sin", "su", "sus",
        "una", "uno", "unos", "unas", "y",
        // french (对齐:108-132)
        "au", "aux", "avec", "dans", "des", "du", "est", "et", "le",
        "les", "ou", "pour", "qui", "sans", "ses", "son", "sur", "un", "une",
        // german (对齐:133-161)
        "auf", "aus", "bei", "das", "dem", "den", "der", "die",
        "ein", "eine", "einem", "einen", "einer",
        "im", "in", "mit", "nach", "oder", "ohne",
        "und", "von", "zu", "zum", "zur",
        // path noise (对齐:209-225)
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

// Protected glossary (対齐 concept-vocabulary.ts:234-277)
static const std::vector<std::string>& get_protected_glossary() {
    static const std::vector<std::string> GLOSSARY = {
        "backup", "backups", "embedding", "embeddings", "failover",
        "gateway", "glacier", "gpt", "kv", "network", "openai",
        "qmd", "router", "s3", "vlan",
        // multilingual (对齐:250-276)
        "sauvegarde", "routeur", "passerelle",
        "konfiguration", "sicherung",
        "respaldo", "enrutador",
        // CJK glossary entries
        "\xe3\x83\x90\xe3\x83\x83\xe3\x82\xaf\xe3\x82\xa2\xe3\x83\x83\xe3\x83\x97",  // バックアップ
        "\xe3\x83\xab\xe3\x83\xbc\xe3\x82\xbf\xe3\x83\xbc",  // ルーター
        "\xe3\x83\x8d\xe3\x83\x83\xe3\x83\x88\xe3\x83\xaf\xe3\x83\xbc\xe3\x82\xaf",  // ネットワーク
        "\xe8\xb7\xaf\xe7\x94\xb1\xe5\x99\xa8",  // 路由器
        "\xe5\xa4\x87\xe4\xbb\xbd",  // 备份
        "\xe6\x95\x85\xe9\x9a\x9c\xe8\xbd\xac\xe7\xa7\xbb",  // 故障转移
        "\xe7\xbd\x91\xe7\xbb\x9c",  // 网络
        "\xe7\xbd\x91\xe5\x85\xb3",  // 网关
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

std::string normalize_concept_token(const std::string& raw_token) {
    std::string normalized = normalize_lowercase(raw_token);
    // Replace underscore with hyphen (対齐:334)
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
        normalized.size() > 32) {
        return "";
    }
    // Reject pure numbers (対齐:340)
    bool all_digits = true;
    for (char c : normalized) {
        if (!std::isdigit(static_cast<unsigned char>(c)) && c != '-') {
            all_digits = false;
            break;
        }
    }
    if (all_digits) return "";

    // Reject date patterns (対齐:341-342)
    std::regex date_re(R"(^\d{4}-\d{2}-\d{2}$)");
    if (std::regex_match(normalized, date_re)) return "";

    // Check minimum length per script (対齐:314-319, 346-348)
    auto script = classify_concept_tag_script(normalized);
    int min_len = (script == ConceptTagScriptFamily::Cjk) ? 2 : 3;
    // For ASCII, count characters directly
    if (script == ConceptTagScriptFamily::Latin && 
        static_cast<int>(normalized.size()) < min_len) return "";

    // Check stop words (対齐:353-355)
    if (get_concept_stop_words().count(normalized) > 0) return "";
    if (get_cjk_stop_words().count(normalized) > 0) return "";

    return normalized;
}

// ============================================================================
// Token collection helpers (対齐 concept-vocabulary.ts:359-393)
// ============================================================================

// Collect glossary matches (対齐:359-369)
static std::vector<std::string> collect_glossary_matches(const std::string& source) {
    std::string lower_source = normalize_lowercase(source);
    std::vector<std::string> matches;
    for (const auto& entry : get_protected_glossary()) {
        if (lower_source.find(entry) != std::string::npos) {
            matches.push_back(entry);
        }
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

    // source = basename(path) + " " + snippet (対齐:400)
    std::string basename = std::filesystem::path(path).filename().string();
    std::string source = basename + " " + snippet;

    std::vector<std::string> tags;

    // Collect tokens in priority order (対齐:409-418)
    auto push_tag = [&](const std::string& raw_token) {
        std::string normalized = normalize_concept_token(raw_token);
        if (normalized.empty()) return;
        // Check for duplicates
        for (const auto& t : tags) {
            if (t == normalized) return;
        }
        tags.push_back(normalized);
    };

    // 1. Glossary matches (highest priority)
    for (const auto& token : collect_glossary_matches(source)) {
        push_tag(token);
        if (static_cast<int>(tags.size()) >= limit) return tags;
    }

    // 2. Compound tokens
    for (const auto& token : collect_compound_tokens(source)) {
        push_tag(token);
        if (static_cast<int>(tags.size()) >= limit) return tags;
    }

    // 3. Segment tokens
    for (const auto& token : collect_segment_tokens(source)) {
        push_tag(token);
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
