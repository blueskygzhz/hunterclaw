// ============================================================================
// mcporter_client.cc
// ----------------------------------------------------------------------------
// McporterClient 接口层 —— 工厂入口与跨实现调度
// ----------------------------------------------------------------------------
//
// 职责:
//   1. 实现 McporterClient 接口的静态工厂 McporterClient::create()
//   2. 提供兼容性工厂 create_mcporter_client()
//   3. 提供直接工厂 create_native_mcp_client()
//
// 设计原则:
//   - 本文件只持有"接口 → 实现"的映射关系,不包含任何具体协议实现。
//   - 当前唯一后端: NativeMcpClient (定义于 native_mcp_client.cc)
//   - 将来要新增 / 替换后端,只需:
//       a) 在此处新增对应 create_xxx_client() 并分发
//       b) McporterClient::create() 根据配置路由到新后端
//
// 依赖:
//   - mcporter_client.h  : McporterClient 接口声明
//   - native_mcp_client.h: NativeMcpClient 的完整类声明(实例化需要)
//
// ============================================================================

#include "hunterclaw/mcp/mcporter_client.h"
#include "hunterclaw/mcp/native_mcp_client.h"
#include <spdlog/spdlog.h>
#include <spdlog/sinks/stdout_sinks.h>

namespace hunterclaw {
namespace mcp {

namespace {

auto porter_log() {
    static auto logger = spdlog::get("mcp-porter");
    if (!logger) logger = spdlog::stdout_logger_mt("mcp-porter");
    return logger;
}

} // namespace

// ----------------------------------------------------------------------------
// 直接工厂: 构造 NativeMcpClient 实例
// ----------------------------------------------------------------------------
// 作为底层可选的"具体工厂",由接口层工厂 McporterClient::create() 调用。
// 调用方通常不直接使用此函数。
// ----------------------------------------------------------------------------

std::shared_ptr<McporterClient> create_native_mcp_client(const McporterConfig& config) {
    return std::make_shared<NativeMcpClient>(config);
}

// ----------------------------------------------------------------------------
// McporterClient 接口统一工厂入口
// ----------------------------------------------------------------------------
// 所有上层代码 (ToolManager::enable_mcp / AgentInstance 等) 都通过这个入口
// 获取 MCP 客户端实例。
//
// 当前路由策略: 总是返回 NativeMcpClient (纯 C++,无外部依赖)。
// ----------------------------------------------------------------------------

std::shared_ptr<McporterClient> McporterClient::create(const McporterConfig& config) {
    porter_log()->info(
        "[MCP] McporterClient::create → NativeMcpClient (in-process, zero external deps)"
    );
    return create_native_mcp_client(config);
}

// ----------------------------------------------------------------------------
// 兼容性工厂
// ----------------------------------------------------------------------------
// 与 McporterClient::create() 行为等价,保留用于历史调用点的向后兼容。
// ----------------------------------------------------------------------------

std::shared_ptr<McporterClient> create_mcporter_client(const McporterConfig& config) {
    return McporterClient::create(config);
}

} // namespace mcp
} // namespace hunterclaw
