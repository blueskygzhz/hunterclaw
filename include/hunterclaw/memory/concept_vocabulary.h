#pragma once

// ============================================================================
// Concept Vocabulary — 严格对齐 OpenClaw concept-vocabulary.ts (474 行)
//
// 概念标签提取引擎。支持多语言（Latin/CJK/混合），使用分词器进行分词，
// 包含停用词过滤（英/西/法/德/日/中/韩）和保护词表。
//
// 主要导出:
//   - deriveConceptTags({path, snippet, limit?}) -> vector<string>
//   - classifyConceptTagScript(tag) -> ConceptTagScriptFamily
//   - summarizeConceptTagScriptCoverage(tags[][]) -> ConceptTagScriptCoverage
//   - MAX_CONCEPT_TAGS = 8
// ============================================================================

#include <string>
#include <vector>
#include <set>

namespace hunterclaw {
namespace memory {

constexpr int MAX_CONCEPT_TAGS = 8;

// Script family classification (对齐 concept-vocabulary.ts:6-7)
enum class ConceptTagScriptFamily {
    Latin,   // "latin"
    Cjk,     // "cjk"
    Mixed,   // "mixed"
    Other    // "other"
};

// Script coverage stats (对齐 concept-vocabulary.ts:8-13)
struct ConceptTagScriptCoverage {
    int latin_entry_count = 0;
    int cjk_entry_count = 0;
    int mixed_entry_count = 0;
    int other_entry_count = 0;
};

// ============================================================================
// Core API
// ============================================================================

/**
 * Derive concept tags from a memory path + snippet.
 * (对齐 concept-vocabulary.ts:395-419 deriveConceptTags)
 *
 * Extracts meaningful concept tokens from the source text using:
 * 1. Protected glossary matches (domain-specific terms)
 * 2. Compound tokens (e.g., "node.js", "api-key")
 * 3. Segmented word tokens (using Intl.Segmenter equivalent)
 *
 * Applies:
 * - Multi-language stop word filtering (EN/ES/FR/DE/JA/ZH/KO)
 * - Minimum length per script family (Latin: 3, CJK: 2)
 * - Pure-number and date rejection
 * - Normalization (NFKC, lowercase, trim punctuation)
 *
 * @param path  Relative memory path (e.g., "memory/2024-03-15.md")
 * @param snippet  Text snippet from the memory chunk
 * @param limit  Maximum number of tags (default: MAX_CONCEPT_TAGS = 8)
 * @return Vector of normalized concept tag strings
 */
std::vector<std::string> derive_concept_tags(
    const std::string& path,
    const std::string& snippet,
    int limit = MAX_CONCEPT_TAGS
);

/**
 * Classify a concept tag's script family.
 * (对齐 concept-vocabulary.ts:294-312 classifyConceptTagScript)
 */
ConceptTagScriptFamily classify_concept_tag_script(const std::string& tag);

/**
 * Summarize script coverage across multiple entry tag sets.
 * (对齐 concept-vocabulary.ts:422-466 summarizeConceptTagScriptCoverage)
 */
ConceptTagScriptCoverage summarize_concept_tag_script_coverage(
    const std::vector<std::vector<std::string>>& concept_tags_by_entry
);

// ============================================================================
// Internal helpers (exposed for testing)
// ============================================================================

/**
 * Normalize a single concept token.
 * Returns empty string if token should be rejected.
 * (对齐 concept-vocabulary.ts:329-357 normalizeConceptToken)
 */
std::string normalize_concept_token(const std::string& raw_token);

/**
 * Normalize a string to lowercase (简化的 normalizeLowercaseStringOrEmpty)
 */
std::string normalize_lowercase(const std::string& input);

}  // namespace memory
}  // namespace hunterclaw
