#include "hunterclaw/gateway/gateway.h"
#include "hunterclaw/gateway/method_guard.h"
#include "hunterclaw/skills/skill.h"
#include "hunterclaw/core/utils.h"
#include <nlohmann/json.hpp>

using json = nlohmann::json;

namespace hunterclaw {
namespace gateway {
namespace methods {

// ============================================================================
// Skill Methods
// ============================================================================

// 11. skill.list - List all skills
core::Result<json> method_skill_list(
    std::shared_ptr<GatewayServer> server,
    std::shared_ptr<MethodGuard> method_guard,
    const ClientContext& client_ctx,
    const json& params
) {
    auto skill_mgr = server->skill_manager();
    auto skills = skill_mgr->list_skills();

    json result = json::array();
    for (const auto& skill_meta : skills) {
        json skill_json;
        skill_json["name"] = skill_meta.skill.name;
        skill_json["description"] = skill_meta.skill.description;
        skill_json["path"] = skill_meta.skill.file_path.string();
        result.push_back(skill_json);
    }

    return core::Result<json>::Ok(result);
}

// 11. skill.get - Get skill by name
core::Result<json> method_skill_get(
    std::shared_ptr<GatewayServer> server,
    std::shared_ptr<MethodGuard> method_guard,
    const ClientContext& client_ctx,
    const json& params
) {
    if (!params.contains("name")) {
        return core::Result<json>::Err("Missing name");
    }

    std::string name = params["name"];
    auto skill_mgr = server->skill_manager();
    auto skill_opt = skill_mgr->get_skill(name);

    if (!skill_opt.has_value()) {
        return core::Result<json>::Err("Skill not found: " + name);
    }

    const auto& skill_meta = skill_opt.value();
    json result;
    result["name"] = skill_meta.skill.name;
    result["description"] = skill_meta.skill.description;
    result["path"] = skill_meta.skill.file_path.string();
    result["content"] = skill_meta.skill.content;

    return core::Result<json>::Ok(result);
}

}  // namespace methods
}  // namespace gateway
}  // namespace hunterclaw
