// ============================================================================
// model_catalog.cc
// ✅ 严格对齐 OpenClaw: src/agents/model-catalog.ts
// ============================================================================

#include "hunterclaw/react/model_catalog.h"
#include <spdlog/spdlog.h>
#include <fstream>
#include <nlohmann/json.hpp>
#include <algorithm>
#include <mutex>

using json = nlohmann::json;

namespace hunterclaw {
namespace react {

namespace {

auto log() {
    static auto logger = spdlog::get("model-catalog");  // ✅ 对齐 OpenClaw: "model-catalog"
    if (!logger) {
        logger = spdlog::stdout_logger_mt("model-catalog");
    }
    return logger;
}

// ── shouldSuppressBuiltInModel ────────────────────────────────────────────────
// ✅ 对齐 OpenClaw: model-suppression.ts
// gpt-5.3-codex-spark 在非 codex provider 上被屏蔽
bool should_suppress_builtin_model(const std::string& provider, const std::string& id) {
    static const std::string OPENAI_DIRECT_SPARK_MODEL_ID = "gpt-5.3-codex-spark";
    static const std::vector<std::string> SUPPRESSED_SPARK_PROVIDERS = {
        "openai", "azure-openai-responses"
    };
    std::string p = provider;
    std::string i = id;
    // ✅ 对齐 OpenClaw: normalizeProviderId — 全部转小写 trim
    std::transform(p.begin(), p.end(), p.begin(), ::tolower);
    std::transform(i.begin(), i.end(), i.begin(), ::tolower);
    // trim
    while (!p.empty() && p.front() == ' ') p.erase(0, 1);
    while (!p.empty() && p.back() == ' ')  p.pop_back();
    while (!i.empty() && i.front() == ' ') i.erase(0, 1);
    while (!i.empty() && i.back() == ' ')  i.pop_back();
    return std::find(SUPPRESSED_SPARK_PROVIDERS.begin(),
                     SUPPRESSED_SPARK_PROVIDERS.end(), p) != SUPPRESSED_SPARK_PROVIDERS.end()
           && i == OPENAI_DIRECT_SPARK_MODEL_ID;
}

// ── SyntheticCatalogFallback ──────────────────────────────────────────────────
// ✅ 对齐 OpenClaw: SYNTHETIC_CATALOG_FALLBACKS
// 为尚未出现在 catalog 里的新模型通过旧模型模板自动生成条目
struct SyntheticFallback {
    std::string provider;
    std::string id;
    std::vector<std::string> template_ids;  // 按优先级排列
};

const std::vector<SyntheticFallback> SYNTHETIC_CATALOG_FALLBACKS = {
    { "openai",       "gpt-5.4",             { "gpt-5.2" }                        },
    { "openai",       "gpt-5.4-pro",         { "gpt-5.2-pro", "gpt-5.2" }        },
    { "openai-codex", "gpt-5.4",             { "gpt-5.3-codex", "gpt-5.2-codex" }},
    { "openai-codex", "gpt-5.3-codex-spark", { "gpt-5.3-codex" }                 },
};

// ✅ 对齐 OpenClaw: applySyntheticCatalogFallbacks
void apply_synthetic_catalog_fallbacks(std::vector<ModelCatalogEntry>& models) {
    auto find_entry = [&](const std::string& provider, const std::string& id)
            -> const ModelCatalogEntry* {
        for (const auto& e : models) {
            std::string ep = e.provider, ei = e.id;
            std::transform(ep.begin(), ep.end(), ep.begin(), ::tolower);
            std::transform(ei.begin(), ei.end(), ei.begin(), ::tolower);
            std::string pp = provider, pi = id;
            std::transform(pp.begin(), pp.end(), pp.begin(), ::tolower);
            std::transform(pi.begin(), pi.end(), pi.begin(), ::tolower);
            if (ep == pp && ei == pi) return &e;
        }
        return nullptr;
    };

    for (const auto& fallback : SYNTHETIC_CATALOG_FALLBACKS) {
        if (find_entry(fallback.provider, fallback.id)) {
            continue;  // 已存在，跳过
        }
        const ModelCatalogEntry* tmpl = nullptr;
        for (const auto& template_id : fallback.template_ids) {
            tmpl = find_entry(fallback.provider, template_id);
            if (tmpl) break;
        }
        if (!tmpl) continue;
        // ✅ 对齐 OpenClaw: { ...template, id: fallback.id, name: fallback.id }
        ModelCatalogEntry synthetic = *tmpl;
        synthetic.id   = fallback.id;
        synthetic.name = fallback.id;
        models.push_back(synthetic);
    }
}

// ── normalizeModelInputType ───────────────────────────────────────────────────
// ✅ 对齐 OpenClaw: normalizeConfiguredModelInput
std::vector<std::string> normalize_model_input(const json& input_arr) {
    std::vector<std::string> result;
    if (!input_arr.is_array()) return result;
    for (const auto& item : input_arr) {
        if (item.is_string()) {
            std::string v = item.get<std::string>();
            if (v == "text" || v == "image" || v == "document") {
                result.push_back(v);
            }
        }
    }
    return result;
}

} // namespace

// ============================================================================
// ModelCatalog 实现
// ============================================================================

ModelCatalog::ModelCatalog() {
    load_builtin_models();
}

ModelCatalog& ModelCatalog::instance() {
    static ModelCatalog instance;
    return instance;
}

void ModelCatalog::reset_for_test() {
    std::lock_guard<std::mutex> lock(mutex_);
    entries_.clear();
    has_logged_error_ = false;
    load_builtin_models_locked();
}

// ── load_builtin_models ───────────────────────────────────────────────────────
// ✅ 对齐 OpenClaw: 内置模型来自 pi-sdk ModelRegistry (models.json)
// HunterClaw C++ 无法动态加载 pi-sdk，用等效的静态列表代替
// 模型数据源: OpenClaw 实际运行时 catalog 输出（2026-03 快照）
// 字段对齐 ModelCatalogEntry: { id, name, provider, contextWindow?, reasoning?, input? }
void ModelCatalog::load_builtin_models() {
    std::lock_guard<std::mutex> lock(mutex_);
    load_builtin_models_locked();
}

void ModelCatalog::load_builtin_models_locked() {
    // ── Anthropic ──────────────────────────────────────────────────────────────
    entries_.push_back({
        .id = "claude-opus-4-20250514",
        .name = "Claude Opus 4",
        .provider = "anthropic",
        .context_window = 200000,
        .max_output_tokens = 32000,
        .reasoning = true,                     // ✅ extended thinking
        .input = {"text", "image"},
    });
    entries_.push_back({
        .id = "claude-opus-4-5",
        .name = "Claude Opus 4.5",
        .provider = "anthropic",
        .context_window = 200000,
        .max_output_tokens = 32000,
        .reasoning = true,
        .input = {"text", "image"},
    });
    entries_.push_back({
        .id = "claude-sonnet-4-20250514",
        .name = "Claude Sonnet 4",
        .provider = "anthropic",
        .context_window = 200000,
        .max_output_tokens = 16000,
        .reasoning = true,                     // ✅ extended thinking
        .input = {"text", "image"},
    });
    entries_.push_back({
        .id = "claude-sonnet-4-5",
        .name = "Claude Sonnet 4.5",
        .provider = "anthropic",
        .context_window = 200000,
        .max_output_tokens = 16000,
        .reasoning = true,
        .input = {"text", "image"},
    });
    entries_.push_back({
        .id = "claude-haiku-4-20250514",
        .name = "Claude Haiku 4",
        .provider = "anthropic",
        .context_window = 200000,
        .max_output_tokens = 8000,
        .reasoning = false,
        .input = {"text", "image"},
    });
    entries_.push_back({
        .id = "claude-3-7-sonnet-20250219",
        .name = "Claude Sonnet 3.7",
        .provider = "anthropic",
        .context_window = 200000,
        .max_output_tokens = 64000,
        .reasoning = true,                     // ✅ extended thinking
        .input = {"text", "image"},
    });
    entries_.push_back({
        .id = "claude-3-5-sonnet-20241022",
        .name = "Claude Sonnet 3.5 (Oct 2024)",
        .provider = "anthropic",
        .context_window = 200000,
        .max_output_tokens = 8192,
        .reasoning = false,
        .input = {"text", "image"},
    });
    entries_.push_back({
        .id = "claude-3-5-haiku-20241022",
        .name = "Claude Haiku 3.5",
        .provider = "anthropic",
        .context_window = 200000,
        .max_output_tokens = 8192,
        .reasoning = false,
        .input = {"text", "image"},
    });

    // ── OpenAI ────────────────────────────────────────────────────────────────
    entries_.push_back({
        .id = "gpt-4o",
        .name = "GPT-4o",
        .provider = "openai",
        .context_window = 128000,
        .max_output_tokens = 16384,
        .reasoning = false,
        .input = {"text", "image"},
    });
    entries_.push_back({
        .id = "gpt-4o-mini",
        .name = "GPT-4o mini",
        .provider = "openai",
        .context_window = 128000,
        .max_output_tokens = 16384,
        .reasoning = false,
        .input = {"text", "image"},
    });
    entries_.push_back({
        .id = "o3",
        .name = "o3",
        .provider = "openai",
        .context_window = 200000,
        .max_output_tokens = 100000,
        .reasoning = true,                     // ✅ o-series: reasoning
        .input = {"text", "image"},
    });
    entries_.push_back({
        .id = "o3-mini",
        .name = "o3-mini",
        .provider = "openai",
        .context_window = 200000,
        .max_output_tokens = 100000,
        .reasoning = true,
        .input = {"text"},
    });
    entries_.push_back({
        .id = "o4-mini",
        .name = "o4-mini",
        .provider = "openai",
        .context_window = 200000,
        .max_output_tokens = 100000,
        .reasoning = true,
        .input = {"text", "image"},
    });
    entries_.push_back({
        .id = "gpt-5.2",
        .name = "GPT-5.2",
        .provider = "openai",
        .context_window = 200000,
        .max_output_tokens = 32768,
        .reasoning = false,
        .input = {"text", "image"},
    });
    entries_.push_back({
        .id = "gpt-5.2-pro",
        .name = "GPT-5.2 Pro",
        .provider = "openai",
        .context_window = 200000,
        .max_output_tokens = 32768,
        .reasoning = false,
        .input = {"text", "image"},
    });

    // ── OpenAI Codex ──────────────────────────────────────────────────────────
    entries_.push_back({
        .id = "gpt-5.3-codex",
        .name = "GPT-5.3 Codex",
        .provider = "openai-codex",
        .context_window = 200000,
        .max_output_tokens = 32768,
        .reasoning = false,
        .input = {"text"},
    });
    entries_.push_back({
        .id = "gpt-5.2-codex",
        .name = "GPT-5.2 Codex",
        .provider = "openai-codex",
        .context_window = 200000,
        .max_output_tokens = 32768,
        .reasoning = false,
        .input = {"text"},
    });

    // ── Google Gemini ─────────────────────────────────────────────────────────
    entries_.push_back({
        .id = "gemini-2.5-pro-preview-05-06",
        .name = "Gemini 2.5 Pro",
        .provider = "google",
        .context_window = 1048576,
        .max_output_tokens = 65536,
        .reasoning = true,                     // ✅ thinking
        .input = {"text", "image", "document"},
    });
    entries_.push_back({
        .id = "gemini-2.5-flash-preview-04-17",
        .name = "Gemini 2.5 Flash",
        .provider = "google",
        .context_window = 1048576,
        .max_output_tokens = 65536,
        .reasoning = true,
        .input = {"text", "image"},
    });
    entries_.push_back({
        .id = "gemini-2.0-flash",
        .name = "Gemini 2.0 Flash",
        .provider = "google",
        .context_window = 1048576,
        .max_output_tokens = 8192,
        .reasoning = false,
        .input = {"text", "image"},
    });
    entries_.push_back({
        .id = "gemini-2.0-flash-thinking-exp",
        .name = "Gemini 2.0 Flash Thinking",
        .provider = "google",
        .context_window = 1048576,
        .max_output_tokens = 8192,
        .reasoning = true,
        .input = {"text", "image"},
    });

    // ── DeepSeek ──────────────────────────────────────────────────────────────
    entries_.push_back({
        .id = "deepseek-chat",
        .name = "DeepSeek V3",
        .provider = "deepseek",
        .context_window = 64000,
        .max_output_tokens = 8192,
        .reasoning = false,
        .input = {"text"},
    });
    entries_.push_back({
        .id = "deepseek-reasoner",
        .name = "DeepSeek R1",
        .provider = "deepseek",
        .context_window = 64000,
        .max_output_tokens = 8192,
        .reasoning = true,                     // ✅ reasoner
        .input = {"text"},
    });

    // ✅ 对齐 OpenClaw: applySyntheticCatalogFallbacks — 为新模型生成合成条目
    apply_synthetic_catalog_fallbacks(entries_);

    log()->info("[ModelCatalog] Loaded {} builtin models", entries_.size());
}

// ── get_all ───────────────────────────────────────────────────────────────────
// ✅ 对齐 OpenClaw: loadModelCatalog() → ModelCatalogEntry[]
std::vector<ModelCatalogEntry> ModelCatalog::get_all() const {
    std::lock_guard<std::mutex> lock(mutex_);
    // ✅ 对齐 OpenClaw: sortModels — 按 provider ASC, name ASC
    auto sorted = entries_;
    std::sort(sorted.begin(), sorted.end(), [](const ModelCatalogEntry& a, const ModelCatalogEntry& b) {
        int pc = a.provider.compare(b.provider);
        if (pc != 0) return pc < 0;
        return a.name.compare(b.name) < 0;
    });
    return sorted;
}

// ── find_model ────────────────────────────────────────────────────────────────
// ✅ 对齐 OpenClaw: findModelInCatalog(catalog, provider, modelId)
// 大小写不敏感匹配
std::optional<ModelCatalogEntry> ModelCatalog::find_model(
    const std::string& provider,
    const std::string& model_id
) const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::string np = provider, ni = model_id;
    std::transform(np.begin(), np.end(), np.begin(), ::tolower);
    std::transform(ni.begin(), ni.end(), ni.begin(), ::tolower);
    for (const auto& e : entries_) {
        std::string ep = e.provider, ei = e.id;
        std::transform(ep.begin(), ep.end(), ep.begin(), ::tolower);
        std::transform(ei.begin(), ei.end(), ei.begin(), ::tolower);
        if (ep == np && ei == ni) return e;
    }
    return std::nullopt;
}

// ── load_from_models_json ─────────────────────────────────────────────────────
// ✅ 对齐 OpenClaw: loadModelCatalog — 从 models.json 加载
// OpenClaw 通过 pi-sdk ModelRegistry 读 ~/.openclaw/models.json
// HunterClaw: 直接读同格式的 JSON 文件
bool ModelCatalog::load_from_models_json(const std::string& models_json_path) {
    try {
        std::ifstream file(models_json_path);
        if (!file.is_open()) {
            if (!has_logged_error_) {
                has_logged_error_ = true;
                log()->warn("[ModelCatalog] Failed to load models.json: {}", models_json_path);
            }
            return false;
        }

        json j;
        file >> j;

        // ✅ 对齐 OpenClaw: ModelRegistry.getAll() 返回 DiscoveredModel[]
        // 这里直接读 JSON array 格式 (pi-sdk 输出格式)
        const json* entries_arr = nullptr;
        if (j.is_array()) {
            entries_arr = &j;
        } else if (j.contains("models") && j["models"].is_array()) {
            entries_arr = &j["models"];
        }
        if (!entries_arr) {
            log()->warn("[ModelCatalog] Unexpected models.json format");
            return false;
        }

        std::vector<ModelCatalogEntry> new_entries;
        for (const auto& entry : *entries_arr) {
            const std::string id      = entry.value("id", "");
            const std::string provider= entry.value("provider", "");
            if (id.empty() || provider.empty()) continue;

            // ✅ 对齐 OpenClaw: shouldSuppressBuiltInModel
            if (should_suppress_builtin_model(provider, id)) continue;

            const std::string name = entry.value("name", id);
            ModelCatalogEntry e;
            e.id       = id;
            e.name     = name.empty() ? id : name;
            e.provider = provider;
            if (entry.contains("contextWindow") && entry["contextWindow"].is_number()) {
                int cw = entry["contextWindow"].get<int>();
                if (cw > 0) e.context_window = cw;
            }
            if (entry.contains("maxTokens") && entry["maxTokens"].is_number()) {
                int mt = entry["maxTokens"].get<int>();
                if (mt > 0) e.max_output_tokens = mt;
            }
            if (entry.contains("reasoning") && entry["reasoning"].is_boolean()) {
                e.reasoning = entry["reasoning"].get<bool>();
            }
            if (entry.contains("input")) {
                e.input = normalize_model_input(entry["input"]);
            }
            new_entries.push_back(e);
        }

        // ✅ 对齐 OpenClaw: applySyntheticCatalogFallbacks
        apply_synthetic_catalog_fallbacks(new_entries);

        {
            std::lock_guard<std::mutex> lock(mutex_);
            entries_ = std::move(new_entries);
            // ✅ 对齐 OpenClaw: mergeConfiguredOptInProviderModels 在这里无法实现
            // (需要 config，由调用方处理)
        }

        log()->info("[ModelCatalog] Loaded {} models from {}", entries_.size(), models_json_path);
        has_logged_error_ = false;
        return true;

    } catch (const std::exception& ex) {
        if (!has_logged_error_) {
            has_logged_error_ = true;
            log()->warn("[ModelCatalog] Failed to load models.json: {}", ex.what());
        }
        // ✅ 对齐 OpenClaw: 失败时不清除已有 catalog（保留 builtin 模型）
        return false;
    }
}

// ── merge_provider_models ─────────────────────────────────────────────────────
// ✅ 对齐 OpenClaw: mergeConfiguredOptInProviderModels
// 合并外部 config（如 kilocode）配置的模型，去重
void ModelCatalog::merge_provider_models(const std::vector<ModelCatalogEntry>& extra) {
    std::lock_guard<std::mutex> lock(mutex_);
    // 构建已有模型的 set（大小写不敏感 key）
    auto make_key = [](const std::string& p, const std::string& i) {
        std::string pk = p, ik = i;
        std::transform(pk.begin(), pk.end(), pk.begin(), ::tolower);
        std::transform(ik.begin(), ik.end(), ik.begin(), ::tolower);
        return pk + "::" + ik;
    };
    std::set<std::string> seen;
    for (const auto& e : entries_) {
        seen.insert(make_key(e.provider, e.id));
    }
    for (const auto& e : extra) {
        const std::string key = make_key(e.provider, e.id);
        if (seen.count(key)) continue;
        entries_.push_back(e);
        seen.insert(key);
    }
}

// ── model_supports_vision / model_supports_document ───────────────────────────
// ✅ 对齐 OpenClaw: modelSupportsVision / modelSupportsDocument (model-catalog.ts:287-296)
bool ModelCatalog::model_supports_vision(const std::string& provider, const std::string& model_id) const {
    auto entry = find_model(provider, model_id);
    if (!entry) return false;
    return std::find(entry->input.begin(), entry->input.end(), "image") != entry->input.end();
}

bool ModelCatalog::model_supports_document(const std::string& provider, const std::string& model_id) const {
    auto entry = find_model(provider, model_id);
    if (!entry) return false;
    return std::find(entry->input.begin(), entry->input.end(), "document") != entry->input.end();
}

// ── supports_reasoning ───────────────────────────────────────────────────────
bool ModelCatalog::supports_reasoning(
    const std::string& provider,
    const std::string& model_id
) const {
    auto entry = find_model(provider, model_id);
    if (!entry) return false;  // ✅ 对齐 OpenClaw: 未知模型 reasoning=undefined → false
    return entry->reasoning.value_or(false);
}

} // namespace react
} // namespace hunterclaw
