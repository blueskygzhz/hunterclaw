// ============================================================================
// test_stub_tool_manager.cc
// 测试用最小 ToolManager 实现。
//
// 只实现 remote_a2a 的 register_tools() 实际用到的四个接口：
//   create / register_tool / get_tool / list_tools / context
// 其余方法给空实现以满足链接。
//
// ⚠ 真实 src/tools/tool_manager.cc 会拖入 MCP/hook/policy/approval/
//   loop_detector 整条依赖链（15 个 include，跨 8 个库），
//   单测因此需要几乎整个二进制。此 stub 仅用于隔离测试。
// ============================================================================

#include "hunterclaw/tools/tool_manager.h"

namespace hunterclaw {
namespace tools {

std::shared_ptr<ToolManager> ToolManager::create(const ToolContext& context) {
    auto m = std::make_shared<ToolManager>();
    m->set_context(context);
    return m;
}

void ToolManager::register_tool(std::shared_ptr<Tool> tool) {
    if (!tool) return;
    // ⚠ 与真实实现一致：同名覆盖。remote_a2a 的去重测试依赖这一行为
    //   （若这里改成"拒绝覆盖"，撞名测试会失去意义）。
    tools_[tool->name()] = std::move(tool);
}

std::shared_ptr<Tool> ToolManager::get_tool(const std::string& name) {
    auto it = tools_.find(name);
    return it == tools_.end() ? nullptr : it->second;
}

std::vector<std::string> ToolManager::list_tools() const {
    std::vector<std::string> out;
    out.reserve(tools_.size());
    for (const auto& kv : tools_) out.push_back(kv.first);
    return out;
}

ToolResult ToolManager::execute_tool(const std::string& name,
                                    const core::JsonValue& params) {
    auto it = tools_.find(name);
    if (it == tools_.end()) return tool_result::error("tool not found: " + name);
    return it->second->execute(params);
}

ToolResult ToolManager::execute_tool(const std::string& name,
                                    const core::JsonValue& params,
                                    ToolUpdateCallback) {
    return execute_tool(name, params);
}

void ToolManager::set_policy_enabled(bool enabled) { policy_enabled_ = enabled; }
void ToolManager::set_policy_pipeline(std::shared_ptr<PolicyPipeline> p) {
    policy_pipeline_ = std::move(p);
}
void ToolManager::set_approval_manager(std::shared_ptr<ApprovalManager> m) {
    approval_manager_ = std::move(m);
}
void ToolManager::set_hook_manager(std::shared_ptr<ToolHookManager> m) {
    hook_manager_ = std::move(m);
}
void ToolManager::set_loop_detector(std::shared_ptr<ToolLoopDetector> d) {
    loop_detector_ = std::move(d);
}
void ToolManager::set_result_processor(std::shared_ptr<ResultProcessingPipeline> p) {
    result_processor_ = std::move(p);
}

core::Result<int> ToolManager::enable_mcp(const mcp::McporterConfig&) {
    return core::Result<int>::Ok(0);
}
core::Result<int> ToolManager::refresh_mcp_tools(const std::vector<std::string>&) {
    return core::Result<int>::Ok(0);
}
std::shared_ptr<mcp::McporterClient> ToolManager::get_mcp_client() const {
    return mcp_client_;
}
std::shared_ptr<mcp::McpToolRegistry> ToolManager::get_mcp_registry() const {
    return mcp_registry_typed_;
}
bool ToolManager::is_mcp_enabled() const { return false; }
void ToolManager::set_mcp_registry(std::shared_ptr<void> r) {
    mcp_registry_ = std::move(r);
}

}  // namespace tools
}  // namespace hunterclaw
