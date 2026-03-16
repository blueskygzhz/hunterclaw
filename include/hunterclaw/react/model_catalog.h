#pragma once

#include "hunterclaw/react/verbose_reasoning_config.h"
#include <string>
#include <vector>
#include <unordered_map>
#include <memory>

namespace hunterclaw {
namespace react {

/**
 * 模型目录管理器 (OpenClaw 对齐)
 * 
 * 对应 OpenClaw: ModelCatalogEntry[]
 * 参考: openclaw-analysis/src/agents/model-selection.ts
 * 
 * 管理模型能力信息,用于自动检测 reasoning 支持
 */
class ModelCatalog {
public:
    /**
     * 获取全局模型目录实例 (单例)
     */
    static ModelCatalog& instance();
    
    /**
     * 注册模型能力
     */
    void register_model(const ModelCapabilities& caps);
    
    /**
     * 查找模型能力
     * 
     * @param provider 提供商 (anthropic, openai, etc.)
     * @param model_id 模型 ID
     * @return 模型能力,如果不存在则返回默认能力
     */
    ModelCapabilities get_capabilities(
        const std::string& provider, 
        const std::string& model_id
    ) const;
    
    /**
     * 检查模型是否支持 reasoning
     * 
     * 对应 OpenClaw: resolveReasoningDefault()
     */
    bool supports_reasoning(
        const std::string& provider, 
        const std::string& model_id
    ) const;
    
    /**
     * 列出所有已注册的模型
     */
    std::vector<ModelCapabilities> list_all_models() const;
    
    /**
     * 清空目录
     */
    void clear();
    
    /**
     * 从配置文件加载模型目录
     */
    bool load_from_config(const std::string& config_path);
    
private:
    ModelCatalog();
    ~ModelCatalog() = default;
    
    // 禁止拷贝和赋值
    ModelCatalog(const ModelCatalog&) = delete;
    ModelCatalog& operator=(const ModelCatalog&) = delete;
    
    // 注册内置模型能力
    void register_builtin_models();
    
    // 生成模型键值 (provider/model_id)
    static std::string make_key(const std::string& provider, const std::string& model_id);
    
    // 模型能力映射
    std::unordered_map<std::string, ModelCapabilities> catalog_;
};

/**
 * 注册常见模型的能力信息
 * 
 * 默认包含:
 * - Anthropic Claude 系列
 * - OpenAI GPT 系列
 * - Google Gemini 系列
 * - Deepseek 系列
 */
void register_common_models();

/**
 * 创建默认模型能力 (当模型未在目录中时)
 */
ModelCapabilities create_default_capabilities(
    const std::string& provider,
    const std::string& model_id
);

} // namespace react
} // namespace hunterclaw
