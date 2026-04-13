// ============================================================================
// merge_patch_arrays.cc
// ✅ 严格对齐 OpenClaw: src/config/merge-patch.ts (98行)
//
// JSON Merge-Patch 增强：按 ID 数组合并
//   - mergeObjectArraysById: 按 `id` 字段进行数组元素级合并
//     * base 数组按 id 索引
//     * patch 中已有 id → 合并到对应元素
//     * patch 中新 id → 追加
//     * patch 中无 id 项 → 追加（避免破坏性替换）
//     * base 非全 id-keyed → 返回 nullopt（退化为整体替换）
// ============================================================================

#include <string>
#include <vector>
#include <map>
#include <optional>
#include <nlohmann/json.hpp>

using json = nlohmann::json;

namespace hunterclaw {
namespace config {

// ============================================================================
// 对齐 merge-patch.ts:10-15: isObjectWithStringId
// ============================================================================
static bool is_object_with_string_id(const json& value) {
    if (!value.is_object()) return false;
    if (!value.contains("id") || !value["id"].is_string()) return false;
    return !value["id"].get<std::string>().empty();
}

// Forward declaration
json apply_merge_patch_with_array_support(const json& base, const json& patch, bool merge_arrays_by_id);

// ============================================================================
// 对齐 merge-patch.ts:25-60: mergeObjectArraysById
// ============================================================================
std::optional<json> merge_object_arrays_by_id(
    const json& base,
    const json& patch,
    bool merge_arrays_by_id
) {
    if (!base.is_array() || !patch.is_array()) return std::nullopt;

    // Check if base is fully id-keyed
    for (const auto& entry : base) {
        if (!is_object_with_string_id(entry)) return std::nullopt;
    }

    json merged = base;

    // Build index by id
    std::map<std::string, size_t> index_by_id;
    for (size_t i = 0; i < merged.size(); ++i) {
        if (is_object_with_string_id(merged[i])) {
            index_by_id[merged[i]["id"].get<std::string>()] = i;
        }
    }

    // Process patch entries
    for (const auto& patch_entry : patch) {
        if (!is_object_with_string_id(patch_entry)) {
            // No id → append as-is
            merged.push_back(patch_entry);
            continue;
        }

        std::string id = patch_entry["id"].get<std::string>();
        auto it = index_by_id.find(id);
        if (it == index_by_id.end()) {
            // New id → append
            merged.push_back(patch_entry);
            index_by_id[id] = merged.size() - 1;
            continue;
        }

        // Existing id → recursive merge
        merged[it->second] = apply_merge_patch_with_array_support(
            merged[it->second], patch_entry, merge_arrays_by_id);
    }

    return merged;
}

// ============================================================================
// Blocked prototype keys (对齐 prototype-keys.ts)
// ============================================================================
static bool is_blocked_object_key(const std::string& key) {
    return key == "__proto__" || key == "constructor" || key == "prototype";
}

// ============================================================================
// 对齐 merge-patch.ts:62-97: applyMergePatch (with array support)
// ============================================================================
json apply_merge_patch_with_array_support(
    const json& base,
    const json& patch,
    bool merge_arrays_by_id
) {
    if (!patch.is_object()) return patch;

    json result = base.is_object() ? base : json::object();

    for (auto& [key, value] : patch.items()) {
        if (is_blocked_object_key(key)) continue;

        if (value.is_null()) {
            result.erase(key);
            continue;
        }

        // Array merge by id
        if (merge_arrays_by_id &&
            result.contains(key) && result[key].is_array() && value.is_array()) {
            auto merged_array = merge_object_arrays_by_id(result[key], value, merge_arrays_by_id);
            if (merged_array.has_value()) {
                result[key] = *merged_array;
                continue;
            }
        }

        // Recursive object merge
        if (value.is_object()) {
            json base_value = (result.contains(key) && result[key].is_object())
                ? result[key] : json::object();
            result[key] = apply_merge_patch_with_array_support(base_value, value, merge_arrays_by_id);
            continue;
        }

        result[key] = value;
    }

    return result;
}

} // namespace config
} // namespace hunterclaw
