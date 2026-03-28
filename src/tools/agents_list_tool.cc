// agents_list_tool.cc
// Strictly aligned with OpenClaw: src/agents/tools/agents-list-tool.ts
//
// Tool: agents_list
// Description: List OpenClaw agent ids you can target with sessions_spawn
//              when runtime="subagent" (based on subagent allowlists).

#include "hunterclaw/tools/agents_list_tool.h"
#include "hunterclaw/tools/tool.h"
#include "hunterclaw/agents/agent_scope.h"
#include "hunterclaw/sessions/session_key_utils.h"
#include "hunterclaw/core/types.h"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace hunterclaw {
namespace tools {

using json = nlohmann::json;

namespace {

// ============================================================================
// 🎯 对齐 OpenClaw: normalizeAgentId
// ============================================================================
static std::string normalize_agent_id(const std::string& id) {
    std::string s = id;
    // trim
    size_t start = s.find_first_not_of(" \t\r\n");
    if (start == std::string::npos) return "";
    size_t end = s.find_last_not_of(" \t\r\n");
    s = s.substr(start, end - start + 1);
    // lowercase
    std::transform(s.begin(), s.end(), s.begin(), ::tolower);
    return s;
}

// ============================================================================
// AgentsListTool
// ============================================================================
class AgentsListTool : public Tool {
public:
    explicit AgentsListTool(AgentsListToolOptions opts)
        : opts_(std::move(opts)) {}

    // 🎯 对齐 OpenClaw: name: "agents_list"
    std::string name() const override { return "agents_list"; }

    // 🎯 对齐 OpenClaw: description
    std::string description() const override {
        return "List OpenClaw agent ids you can target with `sessions_spawn` when "
               "`runtime=\"subagent\"` (based on subagent allowlists).";
    }

    // 🎯 对齐 OpenClaw: AgentsListToolSchema = Type.Object({})
    Schema parameters() const override {
        Schema schema;
        schema.type = "object";
        // No parameters required
        return schema;
    }

    // 🎯 对齐 OpenClaw: execute: async () => { ... }
    ToolResult execute(const core::JsonValue& /*params*/) override {
        try {
            // 🎯 对齐 OpenClaw: const cfg = loadConfig()
            // 🎯 对齐 OpenClaw: const { mainKey, alias } = resolveMainSessionAlias(cfg)
            // 🎯 对齐 OpenClaw: const requesterInternalKey = resolveInternalSessionKey(...)
            // 🎯 对齐 OpenClaw: const requesterAgentId = normalizeAgentId(...)

            std::string requester_agent_id;

            if (!opts_.requester_agent_id_override.empty()) {
                requester_agent_id = normalize_agent_id(opts_.requester_agent_id_override);
            } else if (!opts_.agent_session_key.empty()) {
                // 🎯 对齐 OpenClaw: parseAgentSessionKey(requesterInternalKey)?.agentId
                auto parsed = sessions::parse_agent_session_key(opts_.agent_session_key);
                if (parsed && !parsed->agent_id.empty()) {
                    requester_agent_id = normalize_agent_id(parsed->agent_id);
                }
            }

            // 🎯 对齐 OpenClaw: DEFAULT_AGENT_ID fallback
            if (requester_agent_id.empty()) {
                requester_agent_id = "default";
            }

            // 🎯 对齐 OpenClaw: resolveAgentConfig(cfg, requesterAgentId)?.subagents?.allowAgents ?? []
            std::vector<std::string> allow_agents;
            std::vector<std::string> configured_agents_list;
            std::map<std::string, std::string> configured_name_map;

            if (opts_.config) {
                // 🎯 对齐 OpenClaw: allowAgents from cfg.agents.list[requesterAgentId].subagents.allowAgents
                // core::Config.data is nlohmann::json with the raw config
                const auto& data = opts_.config->data;
                if (data.contains("agents") && data["agents"].contains("list") &&
                    data["agents"]["list"].is_array()) {
                    for (const auto& entry : data["agents"]["list"]) {
                        if (!entry.contains("id") || !entry["id"].is_string()) continue;
                        std::string entry_id = normalize_agent_id(entry["id"].get<std::string>());

                        // Build configured agents list
                        configured_agents_list.push_back(entry_id);
                        if (entry.contains("name") && entry["name"].is_string()) {
                            std::string name = entry["name"].get<std::string>();
                            // trim
                            size_t s = name.find_first_not_of(" \t\r\n");
                            if (s != std::string::npos) {
                                size_t e = name.find_last_not_of(" \t\r\n");
                                name = name.substr(s, e - s + 1);
                                if (!name.empty()) configured_name_map[entry_id] = name;
                            }
                        }

                        // Get allowAgents for requester
                        if (entry_id == requester_agent_id) {
                            if (entry.contains("subagents") && entry["subagents"].contains("allowAgents") &&
                                entry["subagents"]["allowAgents"].is_array()) {
                                for (const auto& v : entry["subagents"]["allowAgents"]) {
                                    if (v.is_string()) {
                                        allow_agents.push_back(v.get<std::string>());
                                    }
                                }
                            }
                        }
                    }
                }
            }

            // 🎯 对齐 OpenClaw: const allowAny = allowAgents.some((value) => value.trim() === "*")
            bool allow_any = false;
            for (const auto& v : allow_agents) {
                std::string trimmed = v;
                size_t s = trimmed.find_first_not_of(" \t\r\n");
                if (s == std::string::npos) continue;
                size_t e = trimmed.find_last_not_of(" \t\r\n");
                trimmed = trimmed.substr(s, e - s + 1);
                if (trimmed == "*") { allow_any = true; break; }
            }

            // 🎯 对齐 OpenClaw: const allowSet = new Set(allowAgents.filter(...).map(normalizeAgentId))
            std::set<std::string> allow_set;
            for (const auto& v : allow_agents) {
                std::string trimmed = v;
                size_t s = trimmed.find_first_not_of(" \t\r\n");
                if (s == std::string::npos) continue;
                size_t e = trimmed.find_last_not_of(" \t\r\n");
                trimmed = trimmed.substr(s, e - s + 1);
                if (!trimmed.empty() && trimmed != "*") {
                    allow_set.insert(normalize_agent_id(trimmed));
                }
            }

            // 🎯 对齐 OpenClaw: build allowed set
            std::set<std::string> allowed;
            allowed.insert(requester_agent_id);
            if (allow_any) {
                for (const auto& id : configured_agents_list) {
                    allowed.insert(id);
                }
            } else {
                for (const auto& id : allow_set) {
                    allowed.insert(id);
                }
            }

            // 🎯 对齐 OpenClaw: ordered = [requesterAgentId, ...rest.toSorted()]
            std::vector<std::string> rest;
            for (const auto& id : allowed) {
                if (id != requester_agent_id) rest.push_back(id);
            }
            std::sort(rest.begin(), rest.end());

            std::vector<std::string> ordered;
            ordered.push_back(requester_agent_id);
            for (const auto& id : rest) ordered.push_back(id);

            // 🎯 对齐 OpenClaw: agents: AgentListEntry[] = ordered.map(...)
            json agents_arr = json::array();
            for (const auto& id : ordered) {
                json entry = {{"id", id}};
                auto it = configured_name_map.find(id);
                if (it != configured_name_map.end()) {
                    entry["name"] = it->second;
                }
                bool configured = std::find(configured_agents_list.begin(),
                                            configured_agents_list.end(), id)
                                  != configured_agents_list.end();
                entry["configured"] = configured;
                agents_arr.push_back(entry);
            }

            // 🎯 对齐 OpenClaw: jsonResult({ requester, allowAny, agents })
            json result = {
                {"requester", requester_agent_id},
                {"allowAny",  allow_any},
                {"agents",    agents_arr},
            };

            ToolResult tool_result;
            tool_result.success = true;
            ToolResultContent content;
            content.type = ToolResultContent::Type::Json;
            content.text = result.dump();
            tool_result.content.push_back(content);
            return tool_result;

        } catch (const std::exception& e) {
            ToolResult err;
            err.success = false;
            err.error = std::string("agents_list failed: ") + e.what();
            return err;
        }
    }

private:
    AgentsListToolOptions opts_;
};

} // namespace

// ============================================================================
// Factory
// ============================================================================

std::shared_ptr<Tool> create_agents_list_tool(const AgentsListToolOptions& opts) {
    return std::make_shared<AgentsListTool>(opts);
}

} // namespace tools
} // namespace hunterclaw
