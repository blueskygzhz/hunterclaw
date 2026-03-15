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
    "的", "了", "着", "过", "得", "地", "吗", "呢", "吧", "啊",
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
    // Particles
    "은", "는", "이", "가", "을", "를", "의", "에", "에서",
    "로", "으로", "와", "과", "도", "만", "까지", "부터",
    // Pronouns
    "나", "나는", "내가", "너", "우리", "저", "그", "그녀", "그들",
    "이것", "저것", "그것", "여기", "저기", "거기",
    // Verbs
    "있다", "없다", "하다", "되다", "이다", "아니다",
    // Nouns (vague)
    "것", "거", "등", "수", "때", "곳", "중", "분",
    // Conjunctions
    "그리고", "하지만", "그래서", "그런데", "그러나", "또는",
    // Question
    "왜", "어떻게", "뭐", "언제", "어디", "누구", "무엇", "어떤",
    // Time
    "어제", "오늘", "내일", "최근", "지금", "아까", "전에"
};

const std::unordered_set<std::string> STOP_WORDS_AR = {
    "ال", "و", "أو", "لكن", "من", "إلى", "في", "على", "عن",
    "أنا", "نحن", "هو", "هي", "هم", "هذا", "هذه", "ذلك", "تلك",
    "كان", "كانت", "يكون", "يمكن", "ممكن",
    "لماذا", "كيف", "ماذا", "متى", "أين", "هل"
};

const std::unordered_set<std::string> STOP_WORDS_ES = {
    "el", "la", "los", "las", "un", "una", "este", "esta", "ese", "esa",
    "yo", "me", "mi", "tu", "usted", "nosotros", "ellos",
    "de", "del", "a", "en", "con", "por", "para", "sobre", "y", "o", "pero",
    "es", "son", "fue", "ser", "estar", "haber", "tener", "hacer",
    "que", "qué", "cómo", "cuando", "donde", "porqué"
};

const std::unordered_set<std::string> STOP_WORDS_PT = {
    "o", "a", "os", "as", "um", "uma", "este", "esta", "esse", "essa",
    "eu", "me", "meu", "você", "ele", "ela", "eles",
    "de", "do", "da", "em", "com", "por", "para", "sobre", "e", "ou", "mas",
    "é", "são", "foi", "ser", "estar", "ter", "fazer",
    "que", "quando", "onde", "porquê"
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

// Check if token is valid keyword
bool is_valid_keyword(const std::string& token) {
    if (token.empty()) return false;
    
    // Skip very short English words
    bool is_ascii = true;
    for (char c : token) {
        if (!std::isalpha(static_cast<unsigned char>(c))) {
            is_ascii = false;
            break;
        }
    }
    if (is_ascii && token.length() < 3) return false;
    
    // Skip pure numbers
    bool is_number = true;
    for (char c : token) {
        if (!std::isdigit(static_cast<unsigned char>(c))) {
            is_number = false;
            break;
        }
    }
    if (is_number) return false;
    
    return true;
}

} // anonymous namespace

bool is_query_stop_word_token(const std::string& token) {
    return STOP_WORDS_EN.count(token) > 0 ||
           STOP_WORDS_ZH.count(token) > 0 ||
           STOP_WORDS_JA.count(token) > 0 ||
           STOP_WORDS_KO.count(token) > 0 ||
           STOP_WORDS_AR.count(token) > 0 ||
           STOP_WORDS_ES.count(token) > 0 ||
           STOP_WORDS_PT.count(token) > 0;
}

std::vector<std::string> tokenize_query(const std::string& text) {
    std::vector<std::string> tokens;
    std::string normalized = to_lower(text);
    
    // Convert to UTF-32 for proper Unicode handling
    std::u32string utf32_text = utf8_to_utf32(normalized);
    
    std::u32string current_token;
    for (size_t i = 0; i < utf32_text.size(); ++i) {
        char32_t c = utf32_text[i];
        
        // Handle CJK characters (Chinese)
        if (is_cjk(c)) {
            // Flush current token
            if (!current_token.empty()) {
                tokens.push_back(utf32_to_utf8(current_token));
                current_token.clear();
            }
            
            // Add single character
            std::u32string single_char(1, c);
            tokens.push_back(utf32_to_utf8(single_char));
            
            // Add bigram if possible
            if (i + 1 < utf32_text.size() && is_cjk(utf32_text[i + 1])) {
                std::u32string bigram;
                bigram.push_back(c);
                bigram.push_back(utf32_text[i + 1]);
                tokens.push_back(utf32_to_utf8(bigram));
            }
        }
        // Handle Hangul/Japanese - keep as continuous segments
        else if (is_hangul(c) || is_japanese(c)) {
            current_token.push_back(c);
        }
        // Handle ASCII alphanumeric
        else if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_') {
            current_token.push_back(c);
        }
        // Whitespace or punctuation - token boundary
        else {
            if (!current_token.empty()) {
                tokens.push_back(utf32_to_utf8(current_token));
                current_token.clear();
            }
        }
    }
    
    // Flush remaining token
    if (!current_token.empty()) {
        tokens.push_back(utf32_to_utf8(current_token));
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
