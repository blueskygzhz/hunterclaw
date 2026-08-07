#include "hunterclaw/memory/active_memory_escalation.h"

#include <regex>
#include <vector>

// 🎯 严格对齐 OpenClaw extensions/active-memory/escalation.ts

namespace hunterclaw {
namespace memory {

namespace {

// 归一化空白：/\s+/g → " "，再 trim（对齐 hasRecallIntent 首行）。
std::string normalize_whitespace(const std::string& message) {
    static const std::regex ws_re(R"(\s+)");
    std::string collapsed = std::regex_replace(message, ws_re, " ");
    size_t b = collapsed.find_first_not_of(" ");
    if (b == std::string::npos) return "";
    size_t e = collapsed.find_last_not_of(" ");
    return collapsed.substr(b, e - b + 1);
}

// 对齐 RECALL_INTENT_PATTERNS（英文 + 西语）。
//   注：std::regex(ECMAScript) 不支持 lookbehind，西语 "última vez" 一条
//   去掉环视，改为字面包含匹配（见has_recall_intent 特判）。
const std::vector<std::regex>& recall_intent_patterns() {
    static const std::vector<std::regex> patterns = [] {
        auto f = std::regex::ECMAScript | std::regex::icase;
        std::vector<std::regex> v;
        v.emplace_back(R"(\b(?:previously|earlier|last time|used to)\b)", f);
        v.emplace_back(R"(\b(?:do|can|could|would)\s+you\s+(?:remember|recall)\b)", f);
        v.emplace_back(R"(\b(?:remember|recall)\s+(?:when|what|which|who|where|why|how)\b)", f);
        v.emplace_back(R"(\b(?:we|you|i)\s+(?:discussed|decided|agreed|said|talked about|chose)\b)", f);
        v.emplace_back(R"(\b(?:previous|earlier|past)\s+(?:decision|conversation|chat|discussion)\b)", f);
        v.emplace_back(
            R"(\b(?:yesterday|the other day|last (?:week|month|year)|(?:\d+|one|two|three|four|five|six|seven|eight|nine|ten)\s+(?:days?|weeks?|months?|years?)\s+ago)\b)", f);
        v.emplace_back(R"(\bwhat did (?:we|you|i)\b)", f);
        v.emplace_back(R"(\bwhat (?:do|did) i usually\b)", f);
        v.emplace_back(
            R"(\b(?:what|which|when|where|why|how)\s+(?:did|have|had)\s+(?:we|you|i)\s+(?:decide|choose|discuss|agree|say|mention|talk|use|do)\b)", f);
        v.emplace_back(
            R"(\b(?:did|have|had)\s+(?:we|you|i)\s+(?:decide|choose|discuss|agree|say|mention|talk)\b)", f);
        v.emplace_back(
            R"(\b(?:conversation|chat|discussion)s?\s+(?:from|in|during)\s+(?:january|february|march|april|may|june|july|august|september|october|november|december|\d{4})\b)", f);
        v.emplace_back(
            R"(\b(?:summarize|review|find|search)\s+(?:my|our|the)?\s*(?:past|previous|earlier)?\s*(?:conversation|chat|discussion)s?\b)", f);
        // 西语（去 última vez 的环视部分，交由字面包含特判）：
        v.emplace_back(R"(¿?qué\s+(?:decidimos|hablamos)|\b(?:recuerdas|recordar|anteriormente|ayer)\b)", f);
        v.emplace_back(R"(\bremind\s+(?:me|us)\s+(?:what|which|who|where|why|how)\b)", f);
        return v;
    }();
    return patterns;
}

// 对齐 LOCALIZED_RECALL_INTENT_PATTERNS（中/日/韩三元组）。
struct LocalizedIntent {
    std::regex intent;
    std::regex future;
    std::regex retrospective;
};

const std::vector<LocalizedIntent>& localized_recall_intent_patterns() {
    static const std::vector<LocalizedIntent> patterns = [] {
        auto f = std::regex::ECMAScript;
        std::vector<LocalizedIntent> v;
        // 中文
        v.push_back(LocalizedIntent{
            std::regex(
                R"([还還][记記]得|[记記]得(?=.{0,24}(?:[吗嗎]|[?？]))|(?:[之以]前|上(?:次|回)|曾[经經])[\s,，、:：]{0,3}(?:(?:我们|我們|我|你|您)?(?:(?:聊|[说說]|[谈談]|[讨討][论論]|提)(?:[过過]|[了的])|[决決]定(?:[过過]|了)|[记記][录錄](?:[过過]|了))|(?:的)?(?:聊天|[对對][话話])[记記][录錄]|的[资資]料))", f),
            std::regex(
                R"(明(?:天|日|晚|年|早)|今(?:晚|夜|后|後)|[后後](?:天|日)|下(?:周|週|星期|(?:个|個)?月|次|回|礼拜|禮拜)|以(?:后|後)|之(?:后|後)|将来|將來|未来|未來|稍(?:后|後|晚)|待(?:会|會)|等(?:会|會|下))", f),
            std::regex(
                R"((?:上(?:次|回|周|週|星期|(?:个|個)?月)|曾[经經]|昨(?:天|日))[\s,，、:：]{0,3}(?:我们|我們|我|你|您)?[\s,，、:：]{0,3}(?:聊|[说說]|[谈談]|[讨討][论論]|提|[决決]定|[记記][录錄]))", f),
        });
        // 日文
        v.push_back(LocalizedIntent{
            std::regex(
                R"(覚えて(?:る|ます|い(?:る|ます))(?!ように|状態に|ことに|ままに)|(?:前回|この前|この間|以前)(?!\s*より).{0,24}(?:話(?:した|していた|してた)|言った|伝えた|相談した|決めた))", f),
            std::regex(
                R"(明(?:日|晩)|今(?:晩|夜)|明後日|来(?:週|月|年)|次(?:回|の)|あとで|後で|後ほど|後日|あした|あす|今後|これから|将来)", f),
            std::regex(
                R"((?:前回|この前|この間|以前|先週|先月|昨日)(?!\s*より).{0,24}(?:話(?:した|していた|してた)|言った|伝えた|相談した|決めた))", f),
        });
        // 韩文
        v.push_back(LocalizedIntent{
            std::regex(
                R"(기억나(?:요|지(?:요)?|죠)?|(?:지난\s*번|저번|예전).{0,24}(?:논의했|(?:이야기|얘기|대화)했|말했|언급했|결정했))", f),
            std::regex(R"(내(?:일|년)|오늘\s*(?:밤|저녁|오후)|모레|글피|차주|다음|나중|앞으로|미래|이따|곧)", f),
            std::regex(
                R"((?:지난\s*(?:번|주|달)|저번|예전|어제).{0,24}(?:논의했|(?:이야기|얘기|대화)했|말했|언급했|결정했))", f),
        });
        return v;
    }();
    return patterns;
}

} // namespace

ActiveMemoryMode parse_active_memory_mode(const std::string& value) {
    if (value == "off") return ActiveMemoryMode::Off;
    if (value == "always") return ActiveMemoryMode::Always;
    // 对齐 OC 默认：escalate
    return ActiveMemoryMode::Escalate;
}

bool has_recall_intent(const std::string& message) {
    std::string normalized = normalize_whitespace(message);
    if (normalized.empty()) {
        return false;
    }
    // 英文/西语正则集
    for (const auto& re : recall_intent_patterns()) {
        if (std::regex_search(normalized, re)) {
            return true;
        }
    }
    // 西语 "última vez" 环视特判（对齐去掉的 lookbehind/lookahead 语义：
    //   前后非字母数字下划线。简化为字面包含即可覆盖常见场景）。
    if (normalized.find("última vez") != std::string::npos ||
        normalized.find("Última vez") != std::string::npos) {
        return true;
    }
    // 多语言三元组：intent 命中且（future 不命中 或 retrospective 命中）。
    for (const auto& li : localized_recall_intent_patterns()) {
        if (std::regex_search(normalized, li.intent) &&
            (!std::regex_search(normalized, li.future) ||
             std::regex_search(normalized, li.retrospective))) {
            return true;
        }
    }
    return false;
}

bool should_escalate_recall(
    ActiveMemoryMode mode,
    const std::string& message,
    bool has_strong_lane_one_hit) {
    if (mode == ActiveMemoryMode::Off) {
        return false;
    }
    if (mode == ActiveMemoryMode::Always) {
        return true;
    }
    // escalate: !hasStrongLaneOneHit && hasRecallIntent(message)
    return !has_strong_lane_one_hit && has_recall_intent(message);
}

} // namespace memory
} // namespace hunterclaw
