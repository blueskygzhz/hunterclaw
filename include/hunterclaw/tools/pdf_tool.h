#pragma once

#include <string>
#include <vector>
#include <memory>
#include <optional>
#include "hunterclaw/tools/tool.h"
#include "hunterclaw/core/json.h"

namespace hunterclaw {
namespace tools {

/**
 * @brief PDF extraction result
 * 
 * Contains extracted text and images from a PDF document.
 */
struct PdfExtractionResult {
    std::string text;                         // Extracted text content
    std::vector<std::string> images_base64;   // Extracted images (base64)
    int page_count;                           // Total pages
    int extracted_pages;                      // Pages extracted
    std::optional<std::string> metadata_title;
    std::optional<std::string> metadata_author;
    bool truncated;                           // Content truncated?
};

/**
 * @brief PDF analysis result
 * 
 * Result from AI model analyzing PDF content.
 */
struct PdfAnalysisResult {
    std::string analysis;                     // AI analysis text
    std::string model;                        // Model used
    int input_tokens;                         // Input tokens used
    int output_tokens;                        // Output tokens used
    bool used_native_pdf;                     // Used native PDF support?
    bool used_extraction_fallback;            // Used extraction fallback?
};

/**
 * @brief PDFTool - Comprehensive PDF Processing Tool
 * 
 * Provides advanced PDF processing capabilities:
 * 1. pdf_read - Read and extract PDF content (text + images)
 * 2. pdf_analyze - AI-powered PDF analysis (vision models)
 * 3. pdf_merge - Combine multiple PDFs into one
 * 4. pdf_split - Split PDF into multiple files
 * 
 * Design Philosophy:
 * - Native PDF support (Anthropic Claude, Google Gemini)
 * - Extraction fallback (poppler, pdfminer, tesseract OCR)
 * - Multi-modal analysis (text + images)
 * - Performance optimization (caching, streaming)
 * 
 * OpenClaw Alignment:
 * - Full compatibility with OpenClaw pdf_tool
 * - Native PDF providers (Anthropic Claude Opus 4, Gemini 2.5 Pro)
 * - Extraction fallback with OCR support
 * - Page range selection and filtering
 * 
 * AI Models (Native PDF Support):
 * - Anthropic Claude Opus 4.6/4.5 (Best quality)
 * - Google Gemini 2.5 Pro (High quality, 2M context)
 * - OpenAI GPT-5 Mini (Fast, cost-effective)
 * 
 * Extraction Libraries:
 * - poppler-utils (pdftotext, pdfimages)
 * - pdfminer.six (Python fallback)
 * - tesseract-ocr (OCR for scanned PDFs)
 * - pdf2image (PDF to image conversion)
 * 
 * Security Features:
 * - Path validation (prevent directory traversal)
 * - File size limits (default: 10MB per PDF)
 * - Page count limits (default: 20 pages)
 * - Max PDFs per operation (default: 10)
 * 
 * Performance Features:
 * - Efficient extraction (native libraries)
 * - Caching (extracted content)
 * - Streaming (large PDFs)
 * - Parallel processing (multiple PDFs)
 * 
 * Usage Example:
 * ```cpp
 * // Read PDF and extract content
 * json read_params = {
 *     {"path", "/path/to/document.pdf"},
 *     {"pages", "1-5"},
 *     {"extract_images", true}
 * };
 * auto result = pdf_tool.execute("pdf_read", read_params);
 * 
 * // AI-powered PDF analysis
 * json analyze_params = {
 *     {"path", "/path/to/document.pdf"},
 *     {"prompt", "Summarize the key findings"},
 *     {"model", "anthropic/claude-opus-4-6"},
 *     {"pages", "1-10"}
 * };
 * auto analysis = pdf_tool.execute("pdf_analyze", analyze_params);
 * 
 * // Merge multiple PDFs
 * json merge_params = {
 *     {"input_paths", json::array({
 *         "/path/to/doc1.pdf",
 *         "/path/to/doc2.pdf"
 *     })},
 *     {"output_path", "/path/to/merged.pdf"}
 * };
 * auto merge_result = pdf_tool.execute("pdf_merge", merge_params);
 * 
 * // Split PDF into multiple files
 * json split_params = {
 *     {"input_path", "/path/to/document.pdf"},
 *     {"output_dir", "/path/to/output/"},
 *     {"pages_per_file", 5}
 * };
 * auto split_result = pdf_tool.execute("pdf_split", split_params);
 * ```
 * 
 * Supported Actions:
 * - pdf_read: Extract text/images (path, pages, extract_images)
 * - pdf_analyze: AI analysis (path, prompt, model, pages, max_tokens)
 * - pdf_merge: Combine PDFs (input_paths, output_path)
 * - pdf_split: Split PDF (input_path, output_dir, pages_per_file)
 * 
 * Page Range Syntax:
 * - Single page: "5"
 * - Range: "1-10"
 * - Multiple ranges: "1-5,10-15,20"
 * - All pages: "" (empty string or omit parameter)
 * 
 * Model Selection (for pdf_analyze):
 * - Primary: "anthropic/claude-opus-4-6" (best quality)
 * - Fallback 1: "anthropic/claude-opus-4-5"
 * - Fallback 2: "google/gemini-2.5-pro"
 * - Fallback 3: "openai/gpt-5-mini"
 * 
 * Configuration:
 * - Max file size: 10MB (configurable)
 * - Max pages: 20 (configurable)
 * - Max PDFs: 10 (configurable)
 * - OCR enabled: true (configurable)
 * - Cache enabled: true (configurable)
 * 
 * @note Requires poppler-utils and tesseract-ocr installed
 * @note Native PDF support requires API keys for respective providers
 */
class PDFTool : public Tool {
public:
    /**
     * @brief Configuration for PDFTool
     */
    struct Config {
        int max_file_size_mb;                 // Max PDF file size in MB
        int max_pages;                        // Max pages to process
        int max_pdfs;                         // Max PDFs per operation
        bool ocr_enabled;                     // Enable OCR for scanned PDFs
        bool cache_enabled;                   // Enable extraction caching
        std::string cache_dir;                // Cache directory
        std::string temp_dir;                 // Temporary directory
        std::string allowed_dir;              // Allowed directory (sandbox)
        
        // AI model configuration
        std::string primary_model;            // Primary model (e.g., claude-opus-4-6)
        std::vector<std::string> fallback_models; // Fallback models
        int max_tokens;                       // Max output tokens (default: 4096)
        
        // API keys (for AI analysis)
        std::optional<std::string> anthropic_api_key;
        std::optional<std::string> google_api_key;
        std::optional<std::string> openai_api_key;
    };

    /**
     * @brief Construct PDFTool with configuration
     * 
     * @param config Tool configuration
     */
    explicit PDFTool(const Config& config);

    /**
     * @brief Get tool metadata
     */
    ToolMetadata get_metadata() const override;

    /**
     * @brief Execute tool action
     * 
     * @param action Action name (pdf_read, pdf_analyze, etc.)
     * @param params Action parameters
     * @return Execution result
     */
    json execute(const std::string& action, const json& params) override;

private:
    // PDF Read
    json pdf_read(const json& params);
    
    // PDF Analyze (AI-powered)
    json pdf_analyze(const json& params);
    
    // PDF Merge
    json pdf_merge(const json& params);
    
    // PDF Split
    json pdf_split(const json& params);
    
    // Helper methods
    PdfExtractionResult extract_pdf_content(
        const std::string& path,
        const std::optional<std::string>& page_range,
        bool extract_images
    );
    
    PdfAnalysisResult analyze_pdf_with_model(
        const std::string& path,
        const std::string& prompt,
        const std::string& model,
        const std::optional<std::string>& page_range,
        int max_tokens
    );
    
    std::vector<int> parse_page_range(
        const std::string& range_str,
        int total_pages
    );
    
    bool validate_path(const std::string& path);
    bool check_file_size(const std::string& path);
    std::string resolve_absolute_path(const std::string& path);
    
    // Extraction backends
    PdfExtractionResult extract_with_poppler(
        const std::string& path,
        const std::vector<int>& pages,
        bool extract_images
    );
    
    PdfExtractionResult extract_with_pdfminer(
        const std::string& path,
        const std::vector<int>& pages
    );
    
    PdfExtractionResult extract_with_ocr(
        const std::string& path,
        const std::vector<int>& pages
    );
    
    // AI analysis backends
    PdfAnalysisResult analyze_with_anthropic(
        const std::string& path,
        const std::string& prompt,
        const std::string& model,
        int max_tokens
    );
    
    PdfAnalysisResult analyze_with_gemini(
        const std::string& path,
        const std::string& prompt,
        int max_tokens
    );
    
    PdfAnalysisResult analyze_with_openai(
        const std::string& path,
        const std::string& prompt,
        int max_tokens
    );
    
    // PDF manipulation backends
    bool merge_pdfs_with_pypdf(
        const std::vector<std::string>& input_paths,
        const std::string& output_path
    );
    
    bool split_pdf_with_pypdf(
        const std::string& input_path,
        const std::string& output_dir,
        int pages_per_file
    );
    
    // Cache management
    std::optional<PdfExtractionResult> get_from_cache(
        const std::string& path,
        const std::string& page_range_key
    );
    
    void save_to_cache(
        const std::string& path,
        const std::string& page_range_key,
        const PdfExtractionResult& result
    );
    
    std::string compute_cache_key(
        const std::string& path,
        const std::string& page_range
    );

    // Configuration
    Config config_;
};

/**
 * @brief Factory function to create PDFTool
 * 
 * @param config Tool configuration
 * @return Unique pointer to PDFTool
 */
std::unique_ptr<PDFTool> create_pdf_tool(const PDFTool::Config& config);

} // namespace tools
} // namespace hunterclaw
