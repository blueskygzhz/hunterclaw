#pragma once
// ============================================================================
// model_catalog.h
// ✅ 严格对齐 OpenClaw: src/agents/model-catalog.ts
// ============================================================================

#include <string>
#include <vector>
#include <optional>
#include <set>
#include <mutex>
#include <algorithm>

namespace hunterclaw {
namespace react {

// ── ModelInputType ────────────────────────────────────────────────────────────
// ✅ 对齐 OpenClaw: ModelInputType = "text" | "image" | "document"
// 用字符串存储（对齐 JSON 格式，方便序列化/比较）

// ── ModelCatalogEntry ─────────────────────────────────────────────────────────
// ✅ 严格对齐 OpenClaw: ModelCatalogEntry (model-catalog.ts:11-18)
// {
//   id: string;
//   name: string;
//   provider: string;
//   contextWindow?: number;
//   reasoning?: boolean;
//   input?: ModelInputType[];
// }
//
// HunterClaw 扩展: max_output_tokens（对齐 ModelDefinitionConfig.maxTokens）
struct ModelCatalogEntry {
    std::string id;                          // ✅ OpenClaw: id
    std::string name;                        // ✅ OpenClaw: name
    std::string provider;                    // ✅ OpenClaw: provider
    std::optional<int> context_window;       // ✅ OpenClaw: contextWindow?
    std::optional<int> max_output_tokens;    // ✅ OpenClaw: ModelDefinitionConfig.maxTokens
    std::optional<bool> reasoning;           // ✅ OpenClaw: reasoning?
    std::vector<std::string> input;          // ✅ OpenClaw: input? ("text"|"image"|"document")
};

// ── ModelCatalog ──────────────────────────────────────────────────────────────
// ✅ 对齐 OpenClaw: loadModelCatalog() 的 C++ 实现
// OpenClaw: 动态加载（pi-sdk ModelRegistry + models.json）
// HunterClaw: 单例，内置静态列表 + 可选从 models.json 覆盖加载
class ModelCatalog {
public:
    // ✅ 对齐 OpenClaw: 进程级单例（模拟 module-scope promise cache）
    static ModelCatalog& instance();

    // ── 核心查询接口 ──────────────────────────────────────────────────────────

    // ✅ 对齐 OpenClaw: loadModelCatalog() → ModelCatalogEntry[]（已排序）
    // 按 provider ASC, name ASC 排序
    std::vector<ModelCatalogEntry> get_all() const;

    // ✅ 对齐 OpenClaw: findModelInCatalog(catalog, provider, modelId)
    // 大小写不敏感
    std::optional<ModelCatalogEntry> find_model(
        const std::string& provider,
        const std::string& model_id
    ) const;

    // ✅ 对齐 OpenClaw: entry?.reasoning ?? false
    bool supports_reasoning(
        const std::string& provider,
        const std::string& model_id
    ) const;

    // ✅ 对齐 OpenClaw: modelSupportsVision(entry) (model-catalog.ts:287)
    bool model_supports_vision(
        const std::string& provider,
        const std::string& model_id
    ) const;

    // ✅ 对齐 OpenClaw: modelSupportsDocument(entry) (model-catalog.ts:293)
    bool model_supports_document(
        const std::string& provider,
        const std::string& model_id
    ) const;

    // ── 加载接口 ──────────────────────────────────────────────────────────────

    // ✅ 对齐 OpenClaw: loadModelCatalog — 从 models.json 加载
    // 失败时不清除已有内置模型（对齐 OpenClaw: 不污染 cache）
    bool load_from_models_json(const std::string& models_json_path);

    // ✅ 对齐 OpenClaw: mergeConfiguredOptInProviderModels
    // 合并外部配置的模型（如 kilocode），去重
    void merge_provider_models(const std::vector<ModelCatalogEntry>& extra);

    // ── 测试接口 ──────────────────────────────────────────────────────────────
    // ✅ 对齐 OpenClaw: resetModelCatalogCacheForTest()
    void reset_for_test();

private:
    ModelCatalog();
    ~ModelCatalog() = default;
    ModelCatalog(const ModelCatalog&) = delete;
    ModelCatalog& operator=(const ModelCatalog&) = delete;

    void load_builtin_models();
    void load_builtin_models_locked();  // 调用方持锁时使用

    mutable std::mutex mutex_;
    std::vector<ModelCatalogEntry> entries_;
    bool has_logged_error_ = false;
};

// ── 自由函数（对齐 OpenClaw 导出的函数） ──────────────────────────────────────

// ✅ 对齐 OpenClaw: modelSupportsVision(entry)
inline bool model_supports_vision(const ModelCatalogEntry* entry) {
    if (!entry) return false;
    return std::find(entry->input.begin(), entry->input.end(), "image") != entry->input.end();
}

// ✅ 对齐 OpenClaw: modelSupportsDocument(entry)
inline bool model_supports_document(const ModelCatalogEntry* entry) {
    if (!entry) return false;
    return std::find(entry->input.begin(), entry->input.end(), "document") != entry->input.end();
}

} // namespace react
} // namespace hunterclaw
