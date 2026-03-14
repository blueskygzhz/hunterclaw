#pragma once

#include "hunterclaw/memory/provider.h"
#include "hunterclaw/memory/plugin.h"
#include "hunterclaw/core/result.h"
#include <memory>

namespace hunterclaw {
namespace memory {

/**
 * 🎯 MemOS Cloud Memory Provider
 * 
 * 对齐 OpenClaw MemOS Cloud Plugin:
 * https://github.com/MemTensor/MemOS-Cloud-OpenClaw-Plugin
 * 
 * MemOS Cloud 是企业级托管记忆服务,提供:
 * - 分布式向量搜索 (Pinecone/Qdrant/Weaviate)
 * - 多租户隔离
 * - 企业级安全和加密
 * - 自动备份和灾难恢复
 * - 实时同步
 * - 跨设备记忆共享
 * 
 * 配置示例 (.plugin.json):
 * {
 *   "id": "memos-cloud",
 *   "name": "MemOS Cloud",
 *   "version": "1.0.0",
 *   "type": "memory",
 *   "config": {
 *     "api_key": {"type": "string", "required": true},
 *     "workspace_id": {"type": "string", "required": true},
 *     "base_url": {"type": "string", "default": "https://api.memos.cloud"}
 *   }
 * }
 * 
 * 使用示例:
 * 
 * // 创建 MemOS Cloud Provider
 * auto provider = std::make_shared<MemOSCloudProvider>(
 *     "your-api-key",
 *     "workspace-id",
 *     "https://api.memos.cloud"
 * );
 * 
 * // 添加记忆
 * auto result = provider->add_memory("Important fact", {{"tag", "work"}});
 * 
 * // 搜索记忆
 * auto search_result = provider->search_memories("fact", SearchOptions{});
 */
class MemOSCloudProvider : public MemoryProvider {
public:
    /**
     * 构造函数
     * @param api_key MemOS Cloud API Key
     * @param workspace_id Workspace ID
     * @param base_url API Base URL (默认: https://api.memos.cloud)
     */
    MemOSCloudProvider(
        const std::string& api_key,
        const std::string& workspace_id,
        const std::string& base_url = "https://api.memos.cloud"
    );
    
    ~MemOSCloudProvider() override;
    
    // ========================================
    // MemoryProvider 接口实现
    // ========================================
    
    core::Result<std::string> add_memory(
        const std::string& content,
        const std::map<std::string, std::string>& metadata = {}
    ) override;
    
    core::Result<std::vector<Memory>> search_memories(
        const std::string& query,
        const SearchOptions& options = SearchOptions()
    ) override;
    
    core::Result<bool> remove_memory(const std::string& id) override;
    
    core::Result<bool> update_memory(
        const std::string& id,
        const std::string& content,
        const std::map<std::string, std::string>& metadata = {}
    ) override;
    
    core::Result<bool> clear_memories() override;
    
    core::Result<MemoryStats> get_stats() override;
    
    // ========================================
    // Provider 元信息
    // ========================================
    
    std::string provider_id() const override { return "memos-cloud"; }
    std::string provider_name() const override { return "MemOS Cloud"; }
    std::string provider_version() const override { return "1.0.0"; }
    
private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

/**
 * 🎯 MemOS Cloud Memory Plugin
 * 
 * Plugin 包装类,用于插件系统集成
 */
class MemOSCloudPlugin : public MemoryPlugin {
public:
    MemOSCloudPlugin() = default;
    ~MemOSCloudPlugin() override = default;
    
    // ========================================
    // Plugin 生命周期
    // ========================================
    
    core::Result<bool> init(const plugins::PluginContext& ctx) override;
    core::Result<bool> shutdown() override;
    core::Result<bool> on_load() override;
    core::Result<bool> on_unload() override;
    
    // ========================================
    // MemoryPlugin 接口
    // ========================================
    
    std::shared_ptr<MemoryProvider> get_provider() override {
        return provider_;
    }
    
    // ========================================
    // Plugin 元信息
    // ========================================
    
    std::string id() const override { return "memos-cloud"; }
    std::string name() const override { return "MemOS Cloud"; }
    std::string version() const override { return "1.0.0"; }
    
private:
    std::shared_ptr<MemOSCloudProvider> provider_;
};

} // namespace memory
} // namespace hunterclaw

// ============================================================================
// Plugin 导出 (C ABI)
// ============================================================================

extern "C" {
    hunterclaw::memory::MemoryPlugin* create_plugin();
    void destroy_plugin(hunterclaw::memory::MemoryPlugin* plugin);
}
