#include "hunterclaw/memory/multimodal.h"
#include <algorithm>
#include <cctype>
#include <optional>
#include <map>

namespace hunterclaw {
namespace memory {

namespace {

// Multimodal specs aligned with OpenClaw
const std::map<MemoryMultimodalModality, std::pair<std::string, std::vector<std::string>>> MULTIMODAL_SPECS = {
    {MemoryMultimodalModality::IMAGE, {
        "Image file",
        {".jpg", ".jpeg", ".png", ".webp", ".gif", ".heic", ".heif"}
    }},
    {MemoryMultimodalModality::AUDIO, {
        "Audio file",
        {".mp3", ".wav", ".ogg", ".opus", ".m4a", ".aac", ".flac"}
    }}
};

std::string to_lower(const std::string& s) {
    std::string result = s;
    std::transform(result.begin(), result.end(), result.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    return result;
}

} // anonymous namespace

std::vector<std::string> get_memory_multimodal_extensions(MemoryMultimodalModality modality) {
    auto it = MULTIMODAL_SPECS.find(modality);
    if (it != MULTIMODAL_SPECS.end()) {
        return it->second.second;
    }
    return {};
}

std::string get_memory_multimodal_label_prefix(MemoryMultimodalModality modality) {
    auto it = MULTIMODAL_SPECS.find(modality);
    if (it != MULTIMODAL_SPECS.end()) {
        return it->second.first;
    }
    return "Unknown file";
}

std::string build_memory_multimodal_label(
    MemoryMultimodalModality modality,
    const std::string& normalized_path
) {
    return get_memory_multimodal_label_prefix(modality) + ": " + normalized_path;
}

std::string build_case_insensitive_extension_glob(const std::string& extension) {
    std::string normalized = extension;
    
    // Trim whitespace
    normalized.erase(0, normalized.find_first_not_of(" \t\n\r"));
    normalized.erase(normalized.find_last_not_of(" \t\n\r") + 1);
    
    // Remove leading dot
    if (!normalized.empty() && normalized[0] == '.') {
        normalized = normalized.substr(1);
    }
    
    normalized = to_lower(normalized);
    
    if (normalized.empty()) {
        return "*";
    }
    
    // Build case-insensitive pattern
    std::string result = "*.";
    for (char c : normalized) {
        if (std::isalpha(static_cast<unsigned char>(c))) {
            result += '[';
            result += std::tolower(c);
            result += std::toupper(c);
            result += ']';
        } else {
            result += c;
        }
    }
    
    return result;
}

std::optional<MemoryMultimodalModality> classify_memory_multimodal_path(
    const std::string& file_path,
    const MemoryMultimodalSettings& settings
) {
    if (!is_memory_multimodal_enabled(settings)) {
        return std::nullopt;
    }
    
    std::string lower_path = to_lower(file_path);
    
    for (const auto& modality : settings.modalities) {
        auto extensions = get_memory_multimodal_extensions(modality);
        for (const auto& ext : extensions) {
            std::string lower_ext = to_lower(ext);
            if (lower_path.size() >= lower_ext.size() &&
                lower_path.substr(lower_path.size() - lower_ext.size()) == lower_ext) {
                return modality;
            }
        }
    }
    
    return std::nullopt;
}

bool is_memory_multimodal_enabled(const MemoryMultimodalSettings& settings) {
    return settings.enabled && !settings.modalities.empty();
}

std::string normalize_gemini_embedding_model_for_memory(const std::string& model) {
    std::string trimmed = model;
    
    // Trim whitespace
    trimmed.erase(0, trimmed.find_first_not_of(" \t\n\r"));
    trimmed.erase(trimmed.find_last_not_of(" \t\n\r") + 1);
    
    if (trimmed.empty()) {
        return "";
    }
    
    // Remove "models/" prefix
    if (trimmed.rfind("models/", 0) == 0) {
        trimmed = trimmed.substr(7);
    }
    
    // Remove "gemini/" or "google/" prefix
    if (trimmed.rfind("gemini/", 0) == 0) {
        trimmed = trimmed.substr(7);
    } else if (trimmed.rfind("google/", 0) == 0) {
        trimmed = trimmed.substr(7);
    }
    
    return trimmed;
}

bool supports_memory_multimodal_embeddings(
    const std::string& provider,
    const std::string& model
) {
    if (provider != "gemini") {
        return false;
    }
    
    std::string normalized = normalize_gemini_embedding_model_for_memory(model);
    return normalized == "gemini-embedding-2-preview";
}

std::set<MemoryMultimodalModality> normalize_memory_multimodal_modalities(
    const std::vector<std::string>& raw
) {
    std::set<MemoryMultimodalModality> result;
    
    // Check for "all"
    for (const auto& item : raw) {
        std::string lower = to_lower(item);
        if (lower == "all") {
            result.insert(MemoryMultimodalModality::IMAGE);
            result.insert(MemoryMultimodalModality::AUDIO);
            return result;
        }
    }
    
    // Parse individual modalities
    for (const auto& item : raw) {
        std::string lower = to_lower(item);
        if (lower == "image") {
            result.insert(MemoryMultimodalModality::IMAGE);
        } else if (lower == "audio") {
            result.insert(MemoryMultimodalModality::AUDIO);
        }
    }
    
    return result;
}

MemoryMultimodalSettings normalize_memory_multimodal_settings(
    bool enabled,
    const std::vector<std::string>& modalities,
    size_t max_file_bytes
) {
    MemoryMultimodalSettings settings;
    settings.enabled = enabled;
    
    // Validate max_file_bytes
    if (max_file_bytes == 0 || max_file_bytes > 1024ULL * 1024 * 1024) {
        settings.max_file_bytes = DEFAULT_MEMORY_MULTIMODAL_MAX_FILE_BYTES;
    } else {
        settings.max_file_bytes = max_file_bytes;
    }
    
    // Parse modalities only if enabled
    if (enabled) {
        settings.modalities = normalize_memory_multimodal_modalities(modalities);
    }
    
    return settings;
}

} // namespace memory
} // namespace hunterclaw
