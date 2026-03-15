#pragma once

#include <string>
#include <vector>
#include <set>
#include <optional>

namespace hunterclaw {
namespace memory {

/**
 * Multimodal memory support (image/audio embeddings).
 * Aligned with OpenClaw multimodal.ts
 */

// Supported modalities
enum class MemoryMultimodalModality {
    IMAGE,
    AUDIO
};

/**
 * Multimodal settings configuration
 */
struct MemoryMultimodalSettings {
    bool enabled = false;
    std::set<MemoryMultimodalModality> modalities;
    size_t max_file_bytes = 10 * 1024 * 1024;  // 10MB default
};

constexpr size_t DEFAULT_MEMORY_MULTIMODAL_MAX_FILE_BYTES = 10 * 1024 * 1024;

/**
 * Get file extensions for a given modality
 */
std::vector<std::string> get_memory_multimodal_extensions(MemoryMultimodalModality modality);

/**
 * Get label prefix for a modality (e.g., "Image file", "Audio file")
 */
std::string get_memory_multimodal_label_prefix(MemoryMultimodalModality modality);

/**
 * Build a descriptive label for a multimodal file
 * E.g., "Image file: path/to/image.jpg"
 */
std::string build_memory_multimodal_label(
    MemoryMultimodalModality modality,
    const std::string& normalized_path
);

/**
 * Build case-insensitive extension glob
 * E.g., ".jpg" -> "*.jpg" with case variations
 */
std::string build_case_insensitive_extension_glob(const std::string& extension);

/**
 * Classify a file path to determine if it's a supported multimodal type
 * Returns modality if matched, otherwise returns nullopt
 */
std::optional<MemoryMultimodalModality> classify_memory_multimodal_path(
    const std::string& file_path,
    const MemoryMultimodalSettings& settings
);

/**
 * Check if multimodal is enabled (has enabled flag and non-empty modalities)
 */
bool is_memory_multimodal_enabled(const MemoryMultimodalSettings& settings);

/**
 * Normalize Gemini embedding model name for memory
 * E.g., "models/gemini-embedding-2-preview" -> "gemini-embedding-2-preview"
 */
std::string normalize_gemini_embedding_model_for_memory(const std::string& model);

/**
 * Check if a provider/model supports multimodal embeddings
 * Currently only Gemini gemini-embedding-2-preview is supported
 */
bool supports_memory_multimodal_embeddings(
    const std::string& provider,
    const std::string& model
);

/**
 * Normalize multimodal selection from config
 * "all" -> {IMAGE, AUDIO}
 */
std::set<MemoryMultimodalModality> normalize_memory_multimodal_modalities(
    const std::vector<std::string>& raw
);

/**
 * Normalize multimodal settings from config
 */
MemoryMultimodalSettings normalize_memory_multimodal_settings(
    bool enabled,
    const std::vector<std::string>& modalities,
    size_t max_file_bytes = DEFAULT_MEMORY_MULTIMODAL_MAX_FILE_BYTES
);

} // namespace memory
} // namespace hunterclaw
