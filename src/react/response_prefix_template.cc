#include "hunterclaw/react/response_prefix_template.h"
#include <algorithm>
#include <regex>

namespace hunterclaw {
namespace react {

namespace {
std::string to_lower(const std::string& s) {
    std::string r = s;
    std::transform(r.begin(), r.end(), r.begin(), ::tolower);
    return r;
}
} // anon

// 对齐 response-prefix-template.ts:40-69
std::optional<std::string> resolve_response_prefix_template(
    const std::optional<std::string>& tmpl,
    const ResponsePrefixContext& context
) {
    if (!tmpl.has_value() || tmpl->empty()) return std::nullopt;

    // Match {variableName} or {variable.name}
    static const std::regex var_re(R"(\{([a-zA-Z][a-zA-Z0-9.]*)\})");
    std::string result;
    std::string input = *tmpl;
    std::sregex_iterator it(input.begin(), input.end(), var_re);
    std::sregex_iterator end;
    size_t last_pos = 0;

    for (; it != end; ++it) {
        auto& match = *it;
        result += input.substr(last_pos, match.position() - last_pos);
        std::string var_name = to_lower(match[1].str());
        std::string replacement = match[0].str(); // default: keep as-is

        if (var_name == "model") {
            replacement = context.model.value_or(match[0].str());
        } else if (var_name == "modelfull") {
            replacement = context.model_full.value_or(match[0].str());
        } else if (var_name == "provider") {
            replacement = context.provider.value_or(match[0].str());
        } else if (var_name == "thinkinglevel" || var_name == "think") {
            replacement = context.thinking_level.value_or(match[0].str());
        } else if (var_name == "identity.name" || var_name == "identityname") {
            replacement = context.identity_name.value_or(match[0].str());
        }

        result += replacement;
        last_pos = match.position() + match.length();
    }
    result += input.substr(last_pos);
    return result;
}

// 对齐 response-prefix-template.ts:84-91
std::string extract_short_model_name(const std::string& full_model) {
    // Strip provider prefix
    auto slash = full_model.rfind('/');
    std::string model_part = (slash != std::string::npos) ?
        full_model.substr(slash + 1) : full_model;
    // Strip date suffixes (YYYYMMDD)
    static const std::regex date_re("-\\d{8}$");
    model_part = std::regex_replace(model_part, date_re, "");
    // Strip -latest
    static const std::regex latest_re("-latest$");
    model_part = std::regex_replace(model_part, latest_re, "");
    return model_part;
}

// 对齐 response-prefix-template.ts:96-103
bool has_template_variables(const std::optional<std::string>& tmpl) {
    if (!tmpl.has_value() || tmpl->empty()) return false;
    static const std::regex var_re(R"(\{[a-zA-Z][a-zA-Z0-9.]*\})");
    return std::regex_search(*tmpl, var_re);
}

} // namespace react
} // namespace hunterclaw
