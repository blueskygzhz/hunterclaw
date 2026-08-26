// ============================================================================
// remote_a2a/agent_card.cc
// AgentCard 解析。畸形输入一律安全默认，不抛异常（卡是不可信输入）。
// ============================================================================

#include "hunterclaw/remote_a2a/agent_card.h"

#include <algorithm>
#include <cctype>
#include <set>

namespace hunterclaw {
namespace remote_a2a {

namespace {

std::string jstr(const nlohmann::json& j, const char* key) {
    if (!j.is_object()) return {};
    auto it = j.find(key);
    if (it == j.end() || !it->is_string()) return {};
    return it->get<std::string>();
}

/// 兼容 camelCase / snake_case
std::string jstr2(const nlohmann::json& j, const char* camel, const char* snake) {
    std::string v = jstr(j, camel);
    return v.empty() ? jstr(j, snake) : v;
}

bool jbool(const nlohmann::json& j, const char* key) {
    if (!j.is_object()) return false;
    auto it = j.find(key);
    if (it == j.end() || !it->is_boolean()) return false;
    return it->get<bool>();
}

bool jbool2(const nlohmann::json& j, const char* camel, const char* snake) {
    if (!j.is_object()) return false;
    auto it = j.find(camel);
    if (it != j.end() && it->is_boolean()) return it->get<bool>();
    it = j.find(snake);
    if (it != j.end() && it->is_boolean()) return it->get<bool>();
    return false;
}

std::vector<std::string> jstr_array(const nlohmann::json& j, const char* key) {
    std::vector<std::string> out;
    if (!j.is_object()) return out;
    auto it = j.find(key);
    if (it == j.end() || !it->is_array()) return out;
    for (const auto& v : *it) {
        if (v.is_string()) out.push_back(v.get<std::string>());
    }
    return out;
}

std::vector<std::string> jstr_array2(const nlohmann::json& j, const char* camel,
                                    const char* snake) {
    auto v = jstr_array(j, camel);
    return v.empty() ? jstr_array(j, snake) : v;
}

std::string to_upper_copy(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
    return s;
}

}  // namespace

core::Result<AgentCard> parse_agent_card(const nlohmann::json& j) {
    if (!j.is_object()) {
        return core::Result<AgentCard>::Err("AgentCard 必须是 JSON object");
    }

    AgentCard c;
    c.protocol_version = jstr2(j, "protocolVersion", "protocol_version");
    c.name = jstr(j, "name");
    c.description = jstr(j, "description");
    c.version = jstr(j, "version");
    c.url = jstr(j, "url");

    // interfaces（A2A 1.x 多传输绑定）
    {
        auto it = j.find("interfaces");
        if (it == j.end()) it = j.find("additionalInterfaces");
        if (it != j.end() && it->is_array()) {
            for (const auto& e : *it) {
                if (!e.is_object()) continue;
                AgentInterface ai;
                ai.url = jstr(e, "url");
                ai.transport = to_upper_copy(jstr(e, "transport"));
                if (!ai.url.empty()) c.interfaces.push_back(std::move(ai));
            }
        }
    }

    // capabilities —— ★ 缺字段一律 false，不推导
    {
        auto it = j.find("capabilities");
        if (it != j.end() && it->is_object()) {
            c.capabilities.streaming = jbool(*it, "streaming");
            c.capabilities.push_notifications =
                jbool2(*it, "pushNotifications", "push_notifications");
            c.capabilities.state_transition_history =
                jbool2(*it, "stateTransitionHistory", "state_transition_history");
            auto ext = it->find("extensions");
            if (ext != it->end() && ext->is_array()) {
                for (const auto& e : *ext) {
                    if (e.is_string()) {
                        c.capabilities.extensions.push_back(e.get<std::string>());
                    } else if (e.is_object()) {
                        std::string uri = jstr(e, "uri");
                        if (!uri.empty()) c.capabilities.extensions.push_back(uri);
                    }
                }
            }
        }
    }

    c.default_input_modes = jstr_array2(j, "defaultInputModes", "default_input_modes");
    c.default_output_modes = jstr_array2(j, "defaultOutputModes", "default_output_modes");

    // skills
    {
        auto it = j.find("skills");
        if (it != j.end() && it->is_array()) {
            for (const auto& e : *it) {
                if (!e.is_object()) continue;
                AgentSkill s;
                s.id = jstr(e, "id");
                s.name = jstr(e, "name");
                s.description = jstr(e, "description");
                s.tags = jstr_array(e, "tags");
                s.examples = jstr_array(e, "examples");
                s.input_modes = jstr_array2(e, "inputModes", "input_modes");
                s.output_modes = jstr_array2(e, "outputModes", "output_modes");
                c.skills.push_back(std::move(s));
            }
        }
    }

    // 原样保留（不进 prompt，供鉴权一致性检查）
    {
        auto it = j.find("securitySchemes");
        if (it == j.end()) it = j.find("security_schemes");
        if (it != j.end() && it->is_object()) c.security_schemes = *it;

        auto it2 = j.find("security");
        if (it2 != j.end()) c.security = *it2;
    }

    // ★ 无任何 RPC URL 的卡不可用 —— 这是唯一的硬失败条件
    if (collect_all_rpc_urls(c).empty()) {
        return core::Result<AgentCard>::Err("AgentCard 未声明任何 RPC URL");
    }
    return core::Result<AgentCard>::Ok(std::move(c));
}

std::vector<std::string> collect_all_rpc_urls(const AgentCard& c) {
    std::vector<std::string> out;
    std::set<std::string> seen;

    auto add = [&](const std::string& u) {
        if (u.empty()) return;
        if (seen.insert(u).second) out.push_back(u);
    };

    add(c.url);
    // ★ 遍历全部 interface —— 只校验 c.url 等于没校验
    for (const auto& i : c.interfaces) add(i.url);
    return out;
}

std::string select_rpc_url(const AgentCard& c) {
    // 优先 JSONRPC
    for (const auto& i : c.interfaces) {
        if (i.transport == "JSONRPC" && !i.url.empty()) return i.url;
    }
    // 其次 HTTP+JSON
    for (const auto& i : c.interfaces) {
        if (i.transport == "HTTP+JSON" && !i.url.empty()) return i.url;
    }
    if (!c.url.empty()) return c.url;
    // 最后取任意一个
    for (const auto& i : c.interfaces) {
        if (!i.url.empty()) return i.url;
    }
    return {};
}

}  // namespace remote_a2a
}  // namespace hunterclaw
