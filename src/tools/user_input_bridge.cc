// ============================================================================
// user_input_bridge.cc
// 见 user_input_bridge.h 的对齐说明。
// ============================================================================

#include "hunterclaw/tools/user_input_bridge.h"

#include <algorithm>
#include <cctype>
#include <sstream>

namespace hunterclaw {
namespace tools {

namespace {

std::string trim_copy(const std::string& s) {
    size_t b = 0;
    size_t e = s.size();
    while (b < e && (s[b] == ' ' || s[b] == '\t' || s[b] == '\r' || s[b] == '\n')) ++b;
    while (e > b && (s[e - 1] == ' ' || s[e - 1] == '\t' || s[e - 1] == '\r' || s[e - 1] == '\n')) --e;
    return s.substr(b, e - b);
}

std::string to_lower_copy(const std::string& s) {
    std::string out = s;
    std::transform(out.begin(), out.end(), out.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}

bool all_digits(const std::string& s) {
    if (s.empty()) return false;
    for (unsigned char c : s) {
        if (!std::isdigit(c)) return false;
    }
    return true;
}

/// 按 \r?\n 切行，【保留空行】。
///
/// ⚠ 不能用"跳过空 token"的切法：位置回落依赖行序，
///   吃掉空行会让后面所有答案整体前移一格。
std::vector<std::string> split_lines_keep_empty(const std::string& text) {
    std::vector<std::string> lines;
    std::string cur;
    for (size_t i = 0; i < text.size(); ++i) {
        const char c = text[i];
        if (c == '\n') {
            lines.push_back(trim_copy(cur));
            cur.clear();
            continue;
        }
        if (c == '\r') {
            // \r\n 作为一个换行处理；孤立的 \r 也当换行
            if (i + 1 < text.size() && text[i + 1] == '\n') continue;
            lines.push_back(trim_copy(cur));
            cur.clear();
            continue;
        }
        cur.push_back(c);
    }
    lines.push_back(trim_copy(cur));
    return lines;
}

/// 解析 "key: value" / "key = value" / "key - value" 形式。
/// 逐条对齐 parseKeyedAnswers() 的正则 ^\s*([^:=-]+?)\s*[:=-]\s*(.+?)\s*$
///
/// ⚠ key 中不允许出现 : = -，这是 OpenClaw 的原始约束。
///   放宽会让 "2026-03-11: 见面" 这种正常文本被误当 keyed 答复。
std::map<std::string, std::string> parse_keyed_answers(const std::string& input_text) {
    std::map<std::string, std::string> out;
    for (const auto& raw_line : split_lines_keep_empty(input_text)) {
        // 找第一个分隔符，且其之前不含任何分隔符
        size_t sep = std::string::npos;
        for (size_t i = 0; i < raw_line.size(); ++i) {
            const char c = raw_line[i];
            if (c == ':' || c == '=' || c == '-') {
                sep = i;
                break;
            }
        }
        if (sep == std::string::npos || sep == 0) continue;
        const std::string key = trim_copy(raw_line.substr(0, sep));
        const std::string value = trim_copy(raw_line.substr(sep + 1));
        if (key.empty() || value.empty()) continue;
        // 后出现的同名 key 覆盖先前的（与 JS Map.set 一致）
        out[to_lower_copy(key)] = value;
    }
    return out;
}

}  // namespace

// ── is_valid_question_id ────────────────────────────────────────────────────

bool is_valid_question_id(const std::string& id) {
    if (id.empty()) return false;
    if (!(id[0] >= 'a' && id[0] <= 'z')) return false;
    for (size_t i = 1; i < id.size(); ++i) {
        const char c = id[i];
        const bool ok = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_';
        if (!ok) return false;
    }
    return true;
}

// ── validate_user_input_questions ───────────────────────────────────────────

std::string validate_user_input_questions(const std::vector<UserInputQuestion>& questions) {
    if (questions.size() < kQuestionsMinItems) {
        return "至少需要 1 个 question";
    }
    if (questions.size() > kQuestionsMaxItems) {
        return "最多支持 " + std::to_string(kQuestionsMaxItems) + " 个 question";
    }

    std::vector<std::string> seen;
    for (const auto& q : questions) {
        if (!is_valid_question_id(q.id)) {
            return "question id '" + q.id + "' 不合法（要求 ^[a-z][a-z0-9_]*$）";
        }
        // ⚠ id 必须唯一：answers 是以 id 为键的 map，重复 id 会让
        //   后一个答案静默覆盖前一个，而调用方看到的是"少了一个答案"。
        if (std::find(seen.begin(), seen.end(), q.id) != seen.end()) {
            return "question id 重复: " + q.id;
        }
        seen.push_back(q.id);

        if (q.header.empty()) {
            return "question '" + q.id + "' 的 header 不能为空";
        }
        if (q.header.size() > kQuestionHeaderMaxLength) {
            return "question '" + q.id + "' 的 header 超过 " +
                   std::to_string(kQuestionHeaderMaxLength) + " 字符";
        }
        if (trim_copy(q.question).empty()) {
            return "question '" + q.id + "' 的正文不能为空";
        }
        if (q.options.size() > kQuestionOptionsMaxItems) {
            return "question '" + q.id + "' 的选项超过 " +
                   std::to_string(kQuestionOptionsMaxItems) + " 个";
        }
        for (const auto& o : q.options) {
            if (trim_copy(o.label).empty()) {
                return "question '" + q.id + "' 存在空选项 label";
            }
        }
        // ⚠ 选项 label 必须互不相同（大小写不敏感）：
        //   答复是按 label 文本匹配的，重名会让"用户点了第 2 个"
        //   被解析成第 1 个。
        for (size_t i = 0; i < q.options.size(); ++i) {
            for (size_t j = i + 1; j < q.options.size(); ++j) {
                if (to_lower_copy(trim_copy(q.options[i].label)) ==
                    to_lower_copy(trim_copy(q.options[j].label))) {
                    return "question '" + q.id + "' 的选项 label 重复: " + q.options[i].label;
                }
            }
        }
    }
    return {};
}

// ── empty_user_input_answers ────────────────────────────────────────────────

UserInputAnswers empty_user_input_answers() { return UserInputAnswers{}; }

// ── question_reply_guidance ─────────────────────────────────────────────────

std::string question_reply_guidance(const std::vector<UserInputQuestion>& questions) {
    if (questions.size() != 1) {
        return "回复时用序号或 question id 标注。选项固定的题目请使用已给出的选项。";
    }
    const UserInputQuestion& q = questions.front();
    if (q.options.empty()) {
        return "直接回复你的答案。";
    }
    return q.is_other ? "回复序号、选项文字，或你自己的答案。" : "回复序号或选项文字。";
}

// ── format_user_input_prompt ────────────────────────────────────────────────

std::string format_user_input_prompt(const std::vector<UserInputQuestion>& questions,
                                     const UserInputPromptOptions& options) {
    std::vector<std::string> lines;
    lines.push_back(options.intro.value_or("需要你补充信息:"));

    for (size_t idx = 0; idx < questions.size(); ++idx) {
        const UserInputQuestion& q = questions[idx];
        lines.push_back("");
        if (questions.size() > 1) {
            // ★ 1-based 序号 —— 与 build_user_input_answers 的 keyed
            //   回落（keyed.find(std::to_string(index + 1))）同一基准。
            lines.push_back(std::to_string(idx + 1) + ". " + q.header);
            lines.push_back(q.question);
        } else {
            lines.push_back(q.header);
            lines.push_back(q.question);
        }
        if (q.is_secret) {
            lines.push_back(options.secret_warning.value_or(
                "此频道可能会把你的回复展示给其他参与者。"));
        }
        for (size_t oi = 0; oi < q.options.size(); ++oi) {
            // ★ 1-based —— normalize_user_input_answer 按同一基准解析数字
            std::string line = std::to_string(oi + 1) + ". " + q.options[oi].label;
            if (q.options[oi].description.has_value() &&
                !q.options[oi].description->empty()) {
                line += " - " + *q.options[oi].description;
            }
            lines.push_back(line);
        }
        if (q.is_other) {
            lines.push_back(options.other_label.value_or("其他: 直接回复你自己的答案。"));
        }
    }

    std::ostringstream ss;
    for (size_t i = 0; i < lines.size(); ++i) {
        if (i > 0) ss << "\n";
        ss << lines[i];
    }
    return ss.str();
}

// ── normalize_user_input_answer ─────────────────────────────────────────────

std::optional<std::string> normalize_user_input_answer(const std::string& answer,
                                                       const UserInputQuestion& question) {
    const std::string trimmed = trim_copy(answer);
    const auto& options = question.options;

    // ① 纯数字 → 1-based 选项。
    //    ★ 只在取数组这一刻转 0-based（与 OpenClaw 注释同一处理）。
    if (all_digits(trimmed)) {
        // 用 long long 解析避免 "999999999999" 溢出 int
        long long n = 0;
        bool overflow = false;
        for (unsigned char c : trimmed) {
            n = n * 10 + (c - '0');
            if (n > 1000000) { overflow = true; break; }
        }
        if (!overflow && n >= 1 && static_cast<size_t>(n) <= options.size()) {
            return options[static_cast<size_t>(n) - 1].label;
        }
        // ⚠ 数字越界时【不 return】，继续往下走 ——
        //   选项里可能真有一个 label 就叫 "2026"。
    }

    // ② 全等 label（大小写不敏感）→ 返回【原始大小写】的 label
    const std::string lowered = to_lower_copy(trimmed);
    for (const auto& o : options) {
        if (to_lower_copy(trim_copy(o.label)) == lowered) {
            return o.label;
        }
    }

    // ③ 有固定选项且不允许 other → 拒绝
    //    ★★ 这条是安全边界，不是体验优化：放行会把"随便""都行"
    //      当成有效枚举值发给远端。
    if (!options.empty() && !question.is_other) {
        return std::nullopt;
    }

    // ④ 自由文本
    if (trimmed.empty()) return std::nullopt;
    return trimmed;
}

// ── build_user_input_answers ────────────────────────────────────────────────

UserInputAnswers build_user_input_answers(const std::vector<UserInputQuestion>& questions,
                                          const std::string& input_text) {
    UserInputAnswers out;

    if (questions.size() == 1) {
        const UserInputQuestion& q = questions.front();
        auto a = normalize_user_input_answer(input_text, q);
        out.answers[q.id] = a.has_value() ? std::vector<std::string>{*a}
                                          : std::vector<std::string>{};
        return out;
    }

    const auto keyed = parse_keyed_answers(input_text);
    // ⚠ 保留空行 —— 见 split_lines_keep_empty 的说明
    const auto fallback_lines = split_lines_keep_empty(input_text);

    for (size_t idx = 0; idx < questions.size(); ++idx) {
        const UserInputQuestion& q = questions[idx];

        // key 查找顺序对齐 OpenClaw: id → header → question 原文 → 1-based 序号
        std::string picked;
        bool found = false;
        const std::string candidates[4] = {
            to_lower_copy(q.id),
            to_lower_copy(q.header),
            to_lower_copy(q.question),
            std::to_string(idx + 1),
        };
        for (const auto& k : candidates) {
            auto it = keyed.find(k);
            if (it != keyed.end()) {
                picked = it->second;
                found = true;
                break;
            }
        }
        if (!found) {
            picked = (idx < fallback_lines.size()) ? fallback_lines[idx] : std::string();
        }

        std::optional<std::string> normalized;
        if (!picked.empty()) {
            normalized = normalize_user_input_answer(picked, q);
        }
        out.answers[q.id] = normalized.has_value() ? std::vector<std::string>{*normalized}
                                                   : std::vector<std::string>{};
    }
    return out;
}

}  // namespace tools
}  // namespace hunterclaw
