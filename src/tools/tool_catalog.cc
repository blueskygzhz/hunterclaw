#include "hunterclaw/tools/tool_catalog.h"
#include <algorithm>
#include <sstream>

namespace hunterclaw {
namespace tools {

// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
// ToolCatalogEntry Implementation
// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━

core::JsonValue ToolCatalogEntry::to_json() const {
    core::JsonValue obj = core::JsonValue::object();
    obj["id"] = id;
    obj["label"] = label;
    obj["description"] = description;
    obj["source"] = source;
    
    if (!plugin_id.empty()) {
        obj["pluginId"] = plugin_id;
    }
    
    if (optional) {
        obj["optional"] = true;
    }
    
    if (!default_profiles.empty()) {
        core::JsonValue profiles_arr = core::JsonValue::array();
        for (const auto& p : default_profiles) {
            profiles_arr.push_back(p);
        }
        obj["defaultProfiles"] = profiles_arr;
    } else {
        obj["defaultProfiles"] = core::JsonValue::array();
    }
    
    return obj;
}

// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
// ToolCatalogGroup Implementation
// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━

core::JsonValue ToolCatalogGroup::to_json() const {
    core::JsonValue obj = core::JsonValue::object();
    obj["id"] = id;
    obj["label"] = label;
    obj["source"] = source;
    
    if (!plugin_id.empty()) {
        obj["pluginId"] = plugin_id;
    }
    
    core::JsonValue tools_arr = core::JsonValue::array();
    for (const auto& tool : tools) {
        tools_arr.push_back(tool.to_json());
    }
    obj["tools"] = tools_arr;
    
    return obj;
}

// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
// ToolCatalog Implementation
// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━

ToolCatalog::ToolCatalog() {
    init_core_tools();
    init_sections();
}

std::shared_ptr<ToolCatalog> ToolCatalog::create() {
    return std::shared_ptr<ToolCatalog>(new ToolCatalog());
}

void ToolCatalog::init_core_tools() {
    // Load all core tool definitions
    for (const auto& def : CORE_TOOL_DEFINITIONS) {
        core_tools_[def.id] = def;
    }
}

void ToolCatalog::init_sections() {
    // Initialize all sections
    for (const auto& [id, label] : CORE_TOOL_SECTION_ORDER) {
        CoreToolSection section;
        section.id = id;
        section.label = label;
        
        // Add tools to this section
        for (const auto& [tool_id, def] : core_tools_) {
            if (def.section_id == id) {
                section.tools.push_back(def);
            }
        }
        
        sections_[id] = section;
    }
}

std::vector<CoreToolSection> ToolCatalog::list_sections() const {
    std::vector<CoreToolSection> result;
    
    // Return sections in defined order
    for (const auto& [id, _] : CORE_TOOL_SECTION_ORDER) {
        auto it = sections_.find(id);
        if (it != sections_.end()) {
            result.push_back(it->second);
        }
    }
    
    return result;
}

std::vector<std::string> ToolCatalog::get_tools_for_profile(ToolProfileId profile_id) const {
    std::vector<std::string> result;
    
    for (const auto& [tool_id, def] : core_tools_) {
        auto it = std::find(def.profiles.begin(), def.profiles.end(), profile_id);
        if (it != def.profiles.end()) {
            result.push_back(tool_id);
        }
    }
    
    return result;
}

std::vector<ToolProfileOption> ToolCatalog::get_profile_options() const {
    return PROFILE_OPTIONS;
}

std::vector<std::string> ToolCatalog::resolve_tool_profiles(const std::string& tool_id) const {
    auto it = core_tools_.find(tool_id);
    if (it == core_tools_.end()) {
        return {};
    }
    
    std::vector<std::string> result;
    for (auto profile_id : it->second.profiles) {
        switch (profile_id) {
            case ToolProfileId::MINIMAL:
                result.push_back("minimal");
                break;
            case ToolProfileId::CODING:
                result.push_back("coding");
                break;
            case ToolProfileId::MESSAGING:
                result.push_back("messaging");
                break;
            case ToolProfileId::FULL:
                result.push_back("full");
                break;
        }
    }
    
    return result;
}

core::JsonValue ToolCatalog::build_catalog(
    const std::string& agent_id,
    bool include_plugins
) const {
    core::JsonValue response = core::JsonValue::object();
    response["agentId"] = agent_id;
    
    // Add profile options
    core::JsonValue profiles_arr = core::JsonValue::array();
    for (const auto& profile : PROFILE_OPTIONS) {
        core::JsonValue profile_obj = core::JsonValue::object();
        profile_obj["id"] = profile.id;
        profile_obj["label"] = profile.label;
        profiles_arr.push_back(profile_obj);
    }
    response["profiles"] = profiles_arr;
    
    // Build groups from sections
    core::JsonValue groups_arr = core::JsonValue::array();
    
    for (const auto& section : list_sections()) {
        ToolCatalogGroup group;
        group.id = section.id;
        group.label = section.label;
        group.source = "core";
        
        for (const auto& tool_def : section.tools) {
            ToolCatalogEntry entry;
            entry.id = tool_def.id;
            entry.label = tool_def.label;
            entry.description = tool_def.description;
            entry.source = "core";
            entry.default_profiles = resolve_tool_profiles(tool_def.id);
            
            group.tools.push_back(entry);
        }
        
        groups_arr.push_back(group.to_json());
    }
    
    // Add plugin tools if requested
    if (include_plugins && !plugin_tools_.empty()) {
        // Group plugins by plugin_id
        std::map<std::string, std::vector<ToolCatalogEntry>> plugin_groups;
        
        for (const auto& plugin_tool : plugin_tools_) {
            std::string pid = plugin_tool.plugin_id.empty() ? "plugin" : plugin_tool.plugin_id;
            plugin_groups[pid].push_back(plugin_tool);
        }
        
        // Create groups
        for (const auto& [plugin_id, tools] : plugin_groups) {
            ToolCatalogGroup group;
            group.id = "plugin:" + plugin_id;
            group.label = plugin_id;
            group.source = "plugin";
            group.plugin_id = plugin_id;
            group.tools = tools;
            
            groups_arr.push_back(group.to_json());
        }
    }
    
    response["groups"] = groups_arr;
    
    return response;
}

bool ToolCatalog::is_tool_in_profile(const std::string& tool_id, ToolProfileId profile_id) const {
    auto it = core_tools_.find(tool_id);
    if (it == core_tools_.end()) {
        return false;
    }
    
    auto& profiles = it->second.profiles;
    return std::find(profiles.begin(), profiles.end(), profile_id) != profiles.end();
}

core::Result<CoreToolSection> ToolCatalog::get_section(const std::string& section_id) const {
    auto it = sections_.find(section_id);
    if (it == sections_.end()) {
        return core::Result<CoreToolSection>::Err("Section not found: " + section_id);
    }
    return core::Result<CoreToolSection>::Ok(it->second);
}

void ToolCatalog::register_plugin_tool(const ToolCatalogEntry& entry) {
    plugin_tools_.push_back(entry);
}

} // namespace tools
} // namespace hunterclaw
