#include "hunterclaw/memory/query_expansion.h"
#include <algorithm>
#include <cctype>
#include <regex>
#include <sstream>

namespace hunterclaw {
namespace memory {

namespace {

// Stop words for multiple languages (aligned with OpenClaw)
const std::unordered_set<std::string> STOP_WORDS_EN = {
    // Articles
    "a", "an", "the", "this", "that", "these", "those",
    // Pronouns
    "i", "me", "my", "we", "our", "you", "your", "he", "she", "it", "they", "them",
    // Verbs
    "is", "are", "was", "were", "be", "been", "being",
    "have", "has", "had", "do", "does", "did",
    "will", "would", "could", "should", "can", "may", "might",
    // Prepositions
    "in", "on", "at", "to", "for", "of", "with", "by", "from",
    "about", "into", "through", "during", "before", "after",
    "above", "below", "between", "under", "over",
    // Conjunctions
    "and", "or", "but", "if", "then", "because", "as",
    "while", "when", "where", "what", "which", "who", "how", "why",
    // Time (vague)
    "yesterday", "today", "tomorrow", "earlier", "later",
    "recently", "ago", "just", "now",
    // Vague references
    "thing", "things", "stuff", "something", "anything", "everything", "nothing",
    // Request words
    "please", "help", "find", "show", "get", "tell", "give"
};

const std::unordered_set<std::string> STOP_WORDS_ZH = {
    // Pronouns
    "我", "我们", "你", "你们", "他", "她", "它", "他们",
    "这", "那", "这个", "那个", "这些", "那些",
    // Auxiliary
    "的", "了", "着", "过", "得", "地", "吗", "呢", "吧", "啊", "呀", "嘛", "啦",
    // Verbs (common)
    "是", "有", "在", "被", "把", "给", "让", "用",
    "到", "去", "来", "做", "说", "看", "找", "想", "要", "能", "会", "可以",
    // Conjunctions
    "和", "与", "或", "但", "但是", "因为", "所以",
    "如果", "虽然", "而", "也", "都", "就", "还", "又", "再", "才", "只",
    // Time (vague)
    "之前", "以前", "之后", "以后", "刚才", "现在",
    "昨天", "今天", "明天", "最近",
    // Vague
    "东西", "事情", "事", "什么", "哪个", "哪些", "怎么", "为什么", "多少",
    // Request
    "请", "帮", "帮忙", "告诉"
};

const std::unordered_set<std::string> STOP_WORDS_JA = {
    "これ", "それ", "あれ", "この", "その", "あの",
    "ここ", "そこ", "あそこ",
    "する", "した", "して", "です", "ます",
    "いる", "ある", "なる", "できる",
    "の", "こと", "もの", "ため",
    "そして", "しかし", "また", "でも",
    "から", "まで", "より", "だけ",
    "なぜ", "どう", "何", "いつ", "どこ", "誰", "どれ",
    "昨日", "今日", "明日", "最近", "今", "さっき", "前", "後"
};

const std::unordered_set<std::string> STOP_WORDS_KO = {
    // Particles (조사)
    "은", "는", "이", "가", "을", "를", "의", "에", "에서",
    "로", "으로", "와", "과", "도", "만", "까지", "부터",
    "한테", "에게", "께", "처럼", "같이", "보다", "마다", "밖에", "대로",
    // Pronouns (대명사)
    "나", "나는", "내가", "나를", "너", "우리", "저", "저희",
    "그", "그녀", "그들",
    "이것", "저것", "그것", "여기", "저기", "거기",
    // Common verbs / auxiliaries (일반 동사/보조 동사)
    "있다", "없다", "하다", "되다", "이다", "아니다",
    "보다", "주다", "오다", "가다",
    // Nouns (의존 명사 / vague)
    "것", "거", "등", "수", "때", "곳", "중", "분",
    // Adverbs
    "잘", "더", "또", "매우", "정말", "아주", "많이", "너무", "좀",
    // Conjunctions
    "그리고", "하지만", "그래서", "그런데", "그러나", "또는", "그러면",
    // Question words
    "왜", "어떻게", "뭐", "언제", "어디", "누구", "무엇", "어떤",
    // Time (vague)
    "어제", "오늘", "내일", "최근", "지금", "아까", "나중", "전에",
    // Request words
    "제발", "부탁"
};

const std::unordered_set<std::string> STOP_WORDS_AR = {
    // Articles and connectors
    "ال", "و", "أو", "لكن", "ثم", "بل",
    // Pronouns / references
    "أنا", "نحن", "هو", "هي", "هم",
    "هذا", "هذه", "ذلك", "تلك", "هنا", "هناك",
    // Common prepositions
    "من", "إلى", "الى", "في", "على", "عن", "مع", "بين",
    "ل", "ب", "ك",
    // Common auxiliaries / vague verbs
    "كان", "كانت", "يكون", "تكون", "صار", "أصبح", "يمكن", "ممكن",
    // Time references (vague)
    "بالأمس", "امس", "اليوم", "غدا", "الآن", "قبل", "بعد", "مؤخرا",
    // Question/request words
    "لماذا", "كيف", "ماذا", "متى", "أين", "هل",
    "من فضلك", "فضلا", "ساعد"
};

const std::unordered_set<std::string> STOP_WORDS_ES = {
    // Articles and determiners
    "el", "la", "los", "las", "un", "una", "unos", "unas",
    "este", "esta", "ese", "esa",
    // Pronouns
    "yo", "me", "mi", "nosotros", "nosotras",
    "tu", "tus", "usted", "ustedes", "ellos", "ellas",
    // Prepositions and conjunctions
    "de", "del", "a", "en", "con", "por", "para", "sobre", "entre",
    "y", "o", "pero", "si", "porque", "como",
    // Common verbs / auxiliaries
    "es", "son", "fue", "fueron", "ser", "estar", "haber", "tener", "hacer",
    // Time references (vague)
    "ayer", "hoy", "mañana", "antes", "despues", "después", "ahora", "recientemente",
    // Question/request words
    "que", "qué", "cómo", "cuando", "cuándo", "donde", "dónde", "porqué",
    "favor", "ayuda"
};

const std::unordered_set<std::string> STOP_WORDS_PT = {
    // Articles and determiners
    "o", "a", "os", "as", "um", "uma", "uns", "umas",
    "este", "esta", "esse", "essa",
    // Pronouns
    "eu", "me", "meu", "minha", "nos", "nós",
    "você", "vocês", "ele", "ela", "eles", "elas",
    // Prepositions and conjunctions
    "de", "do", "da", "em", "com", "por", "para", "sobre", "entre",
    "e", "ou", "mas", "se", "porque", "como",
    // Common verbs / auxiliaries
    "é", "são", "foi", "foram", "ser", "estar", "ter", "fazer",
    // Time references (vague)
    "ontem", "hoje", "amanhã", "antes", "depois", "agora", "recentemente",
    // Question/request words
    "que", "quê", "quando", "onde", "porquê",
    "favor", "ajuda"
};

// Korean trailing particles to strip (对齐 mmr.ts:KO_TRAILING_PARTICLES)
// Sorted by descending length so longest-match-first is guaranteed.
const std::vector<std::u32string> KO_TRAILING_PARTICLES = {
    U"에서", U"으로", U"에게", U"한테", U"처럼", U"같이", U"보다",
    U"까지", U"부터", U"마다", U"밖에", U"대로",
    U"은", U"는", U"이", U"가", U"을", U"를", U"의", U"에",
    U"로", U"와", U"과", U"도", U"만"
};

// Check if character is CJK
bool is_cjk(char32_t c) {
    return (c >= 0x4E00 && c <= 0x9FFF);  // CJK Unified Ideographs
}

// Check if character is Korean Hangul
bool is_hangul(char32_t c) {
    return (c >= 0xAC00 && c <= 0xD7AF) || (c >= 0x3131 && c <= 0x3163);
}

// Check if character is Japanese
bool is_japanese(char32_t c) {
    return (c >= 0x3040 && c <= 0x30FF) || (c >= 0x31F0 && c <= 0x31FF);
}

// UTF-8 to UTF-32 conversion
std::u32string utf8_to_utf32(const std::string& utf8) {
    std::u32string result;
    size_t i = 0;
    while (i < utf8.size()) {
        unsigned char c = utf8[i];
        char32_t codepoint = 0;
        
        if (c < 0x80) {
            codepoint = c;
            i += 1;
        } else if ((c & 0xE0) == 0xC0) {
            codepoint = ((c & 0x1F) << 6) | (utf8[i+1] & 0x3F);
            i += 2;
        } else if ((c & 0xF0) == 0xE0) {
            codepoint = ((c & 0x0F) << 12) | ((utf8[i+1] & 0x3F) << 6) | (utf8[i+2] & 0x3F);
            i += 3;
        } else if ((c & 0xF8) == 0xF0) {
            codepoint = ((c & 0x07) << 18) | ((utf8[i+1] & 0x3F) << 12) 
                      | ((utf8[i+2] & 0x3F) << 6) | (utf8[i+3] & 0x3F);
            i += 4;
        } else {
            i += 1;
            continue;
        }
        result.push_back(codepoint);
    }
    return result;
}

// UTF-32 to UTF-8 conversion
std::string utf32_to_utf8(const std::u32string& utf32) {
    std::string result;
    for (char32_t c : utf32) {
        if (c < 0x80) {
            result.push_back(static_cast<char>(c));
        } else if (c < 0x800) {
            result.push_back(static_cast<char>(0xC0 | (c >> 6)));
            result.push_back(static_cast<char>(0x80 | (c & 0x3F)));
        } else if (c < 0x10000) {
            result.push_back(static_cast<char>(0xE0 | (c >> 12)));
            result.push_back(static_cast<char>(0x80 | ((c >> 6) & 0x3F)));
            result.push_back(static_cast<char>(0x80 | (c & 0x3F)));
        } else {
            result.push_back(static_cast<char>(0xF0 | (c >> 18)));
            result.push_back(static_cast<char>(0x80 | ((c >> 12) & 0x3F)));
            result.push_back(static_cast<char>(0x80 | ((c >> 6) & 0x3F)));
            result.push_back(static_cast<char>(0x80 | (c & 0x3F)));
        }
    }
    return result;
}

// Convert string to lowercase
std::string to_lower(const std::string& s) {
    std::string result = s;
    std::transform(result.begin(), result.end(), result.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    return result;
}

// Check if token is valid keyword (对齐 query-expansion.ts:isValidKeyword)
bool is_valid_keyword(const std::string& token) {
    if (token.empty()) return false;

    // Skip very short English words (likely stop words or fragments)
    bool is_ascii_alpha = true;
    for (char c : token) {
        if (!std::isalpha(static_cast<unsigned char>(c))) {
            is_ascii_alpha = false;
            break;
        }
    }
    if (is_ascii_alpha && token.length() < 3) return false;

    // Skip pure numbers (not useful for semantic search)
    bool is_number = true;
    for (char c : token) {
        if (!std::isdigit(static_cast<unsigned char>(c))) {
            is_number = false;
            break;
        }
    }
    if (is_number) return false;

    // Skip tokens that are all punctuation (对齐 /^[\p{P}\p{S}]+$/u)
    bool all_punct = true;
    for (char c : token) {
        if (!std::ispunct(static_cast<unsigned char>(c))) {
            all_punct = false;
            break;
        }
    }
    if (all_punct) return false;

    return true;
}

} // anonymous namespace

bool is_query_stop_word_token(const std::string& token) {
    return STOP_WORDS_EN.count(token) > 0 ||
           STOP_WORDS_ES.count(token) > 0 ||
           STOP_WORDS_PT.count(token) > 0 ||
           STOP_WORDS_AR.count(token) > 0 ||
           STOP_WORDS_ZH.count(token) > 0 ||
           STOP_WORDS_KO.count(token) > 0 ||
           STOP_WORDS_JA.count(token) > 0;
}

// Strip Korean trailing particle from a UTF-32 token
// (对齐 query-expansion.ts:stripKoreanTrailingParticle)
std::optional<std::u32string> strip_korean_trailing_particle(const std::u32string& token) {
    if (token.empty()) return std::nullopt;
    // Try longest match first (particles are sorted by descending length)
    for (const auto& particle : KO_TRAILING_PARTICLES) {
        if (token.size() > particle.size() &&
            token.compare(token.size() - particle.size(), particle.size(), particle) == 0) {
            return token.substr(0, token.size() - particle.size());
        }
    }
    return std::nullopt;
}

// Check if a Korean stem is a useful keyword
// (对齐 query-expansion.ts:isUsefulKoreanStem)
bool is_useful_korean_stem(const std::u32string& stem) {
    if (stem.empty()) return false;
    // Must be at least 2 characters to be useful
    if (stem.size() < 2) return false;
    // Must contain at least one Hangul character
    for (char32_t c : stem) {
        if (is_hangul(c)) return true;
    }
    return false;
}

// Check if a UTF-32 codepoint is whitespace or ASCII punctuation
// (对齐 openclaw: split(/[\s\p{P}]+/u))
static bool is_segment_separator(char32_t c) {
    // ASCII whitespace
    if (c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v') return true;
    // ASCII punctuation
    if (c >= 0x21 && c <= 0x7E) {
        // Keep alphanumeric and underscore as non-separators
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '_') return false;
        return true;  // All other ASCII printable chars are punctuation/separators
    }
    // Unicode general punctuation block (U+2000–U+206F)
    if (c >= 0x2000 && c <= 0x206F) return true;
    // CJK punctuation (U+3000–U+303F)
    if (c >= 0x3000 && c <= 0x303F) return true;
    // Fullwidth punctuation (U+FF00–U+FFEF)
    if (c >= 0xFF00 && c <= 0xFFEF) return true;
    return false;
}

std::vector<std::string> tokenize_query(const std::string& text) {
    // 对齐 openclaw query-expansion.ts:tokenize
    // 1. normalized = text.toLowerCase().trim()
    // 2. segments = normalized.split(/[\s\p{P}]+/u).filter(Boolean)
    // 3. For each segment: detect script and process accordingly

    std::string normalized = to_lower(text);
    // trim
    {
        size_t s = normalized.find_first_not_of(" \t\n\r\f\v");
        if (s == std::string::npos) return {};
        size_t e = normalized.find_last_not_of(" \t\n\r\f\v");
        normalized = normalized.substr(s, e - s + 1);
    }

    // Split into segments by whitespace/punctuation
    std::u32string utf32_text = utf8_to_utf32(normalized);
    std::vector<std::u32string> segments;
    std::u32string current_seg;
    for (char32_t c : utf32_text) {
        if (is_segment_separator(c)) {
            if (!current_seg.empty()) {
                segments.push_back(current_seg);
                current_seg.clear();
            }
        } else {
            current_seg.push_back(c);
        }
    }
    if (!current_seg.empty()) segments.push_back(current_seg);

    std::vector<std::string> tokens;

    for (const auto& seg : segments) {
        if (seg.empty()) continue;

        // Detect script of segment
        bool has_japanese = false;
        bool has_cjk = false;
        bool has_hangul = false;
        for (char32_t c : seg) {
            if (is_japanese(c)) has_japanese = true;
            if (is_cjk(c)) has_cjk = true;
            if (is_hangul(c)) has_hangul = true;
        }

        if (has_japanese) {
            // 对齐 openclaw: Japanese text - extract script-specific chunks
            // match /[a-z0-9_]+|[\u30a0-\u30ffー]+|[\u4e00-\u9fff]+|[\u3040-\u309f]{2,}/g
            std::u32string jp_current;
            enum class JpMode { None, Ascii, Katakana, Kanji, Hiragana } mode = JpMode::None;

            auto flush_jp = [&]() {
                if (jp_current.empty()) return;
                std::string part = utf32_to_utf8(jp_current);
                // For CJK (kanji) parts: add unigrams + bigrams
                bool all_kanji = true;
                for (char32_t c : jp_current) {
                    if (!is_cjk(c)) { all_kanji = false; break; }
                }
                if (all_kanji && jp_current.size() >= 1) {
                    tokens.push_back(part);
                    for (size_t i = 0; i + 1 < jp_current.size(); ++i) {
                        std::u32string bigram;
                        bigram.push_back(jp_current[i]);
                        bigram.push_back(jp_current[i+1]);
                        tokens.push_back(utf32_to_utf8(bigram));
                    }
                } else {
                    tokens.push_back(part);
                }
                jp_current.clear();
            };

            for (char32_t c : seg) {
                JpMode new_mode;
                if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_') {
                    new_mode = JpMode::Ascii;
                } else if ((c >= 0x30A0 && c <= 0x30FF) || c == 0x30FC) {
                    new_mode = JpMode::Katakana;
                } else if (is_cjk(c)) {
                    new_mode = JpMode::Kanji;
                } else if (c >= 0x3040 && c <= 0x309F) {
                    new_mode = JpMode::Hiragana;
                } else {
                    flush_jp();
                    mode = JpMode::None;
                    continue;
                }
                // Hiragana: only keep sequences of 2+
                if (new_mode != mode) {
                    flush_jp();
                    mode = new_mode;
                }
                jp_current.push_back(c);
            }
            flush_jp();

        } else if (has_cjk) {
            // 对齐 openclaw: Chinese - unigrams + bigrams
            std::vector<char32_t> chars;
            for (char32_t c : seg) {
                if (is_cjk(c)) chars.push_back(c);
            }
            for (char32_t c : chars) {
                std::u32string s(1, c);
                tokens.push_back(utf32_to_utf8(s));
            }
            for (size_t i = 0; i + 1 < chars.size(); ++i) {
                std::u32string bigram;
                bigram.push_back(chars[i]);
                bigram.push_back(chars[i+1]);
                tokens.push_back(utf32_to_utf8(bigram));
            }

        } else if (has_hangul) {
            // 对齐 openclaw: Korean - keep word, strip trailing particles
            std::string seg_str = utf32_to_utf8(seg);
            auto stem = strip_korean_trailing_particle(seg);
            bool stem_is_stop = stem && STOP_WORDS_KO.count(utf32_to_utf8(*stem)) > 0;

            if (!STOP_WORDS_KO.count(seg_str) && !stem_is_stop) {
                tokens.push_back(seg_str);
            }
            if (stem && !STOP_WORDS_KO.count(utf32_to_utf8(*stem)) && is_useful_korean_stem(*stem)) {
                tokens.push_back(utf32_to_utf8(*stem));
            }

        } else {
            // Non-CJK: keep as single token
            tokens.push_back(utf32_to_utf8(seg));
        }
    }

    return tokens;
}

std::vector<std::string> extract_keywords(const std::string& query) {
    auto tokens = tokenize_query(query);
    std::vector<std::string> keywords;
    std::unordered_set<std::string> seen;
    
    for (const auto& token : tokens) {
        // Skip stop words
        if (is_query_stop_word_token(token)) {
            continue;
        }
        
        // Skip invalid keywords
        if (!is_valid_keyword(token)) {
            continue;
        }
        
        // Skip duplicates
        if (seen.count(token) > 0) {
            continue;
        }
        
        seen.insert(token);
        keywords.push_back(token);
    }
    
    return keywords;
}

ExpandedQuery expand_query_for_fts(const std::string& query) {
    ExpandedQuery result;
    result.original = query;
    result.keywords = extract_keywords(query);
    
    // Build expanded query: original OR keywords
    if (!result.keywords.empty()) {
        std::ostringstream oss;
        oss << result.original;
        for (const auto& kw : result.keywords) {
            oss << " OR " << kw;
        }
        result.expanded = oss.str();
    } else {
        result.expanded = result.original;
    }
    
    return result;
}

std::vector<std::string> expand_query_with_llm(
    const std::string& query,
    const LlmQueryExpander& llm_expander
) {
    // Try LLM expander first if provided
    if (llm_expander) {
        try {
            auto llm_keywords = llm_expander(query);
            if (!llm_keywords.empty()) {
                return llm_keywords;
            }
        } catch (...) {
            // LLM failed, fall back
        }
    }
    
    // Fall back to local extraction
    return extract_keywords(query);
}

} // namespace memory
} // namespace hunterclaw
