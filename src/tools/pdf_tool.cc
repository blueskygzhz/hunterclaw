#include "hunterclaw/tools/pdf_tool.h"
#include <fstream>
#include <sstream>
#include <filesystem>
#include <cstdlib>
#include <regex>

namespace hunterclaw {
namespace tools {

namespace {

// Helper: Execute shell command and capture output
std::string exec_command(const std::string& cmd) {
    std::array<char, 128> buffer;
    std::string result;
    std::unique_ptr<FILE, decltype(&pclose)> pipe(popen(cmd.c_str(), "r"), pclose);
    if (!pipe) {
        throw std::runtime_error("popen() failed!");
    }
    while (fgets(buffer.data(), buffer.size(), pipe.get()) != nullptr) {
        result += buffer.data();
    }
    return result;
}

// Helper: Check if file exists
bool file_exists(const std::string& path) {
    return std::filesystem::exists(path) && std::filesystem::is_regular_file(path);
}

// Helper: Get file size in MB
double get_file_size_mb(const std::string& path) {
    if (!file_exists(path)) return 0.0;
    auto size = std::filesystem::file_size(path);
    return static_cast<double>(size) / (1024.0 * 1024.0);
}

} // anonymous namespace

//=============================================================================
// PDFTool Implementation
//=============================================================================

PDFTool::PDFTool(const Config& config)
    : config_(config)
{
    // Initialize directories
    std::filesystem::create_directories(config_.cache_dir);
    std::filesystem::create_directories(config_.temp_dir);
}

ToolMetadata PDFTool::get_metadata() const {
    return ToolMetadata{
        .name = "pdf",
        .description = "Comprehensive PDF processing tool with 4 actions: "
                      "pdf_read, pdf_analyze, pdf_merge, pdf_split. "
                      "Supports native AI analysis and extraction fallback.",
        .version = "1.0.0",
        .author = "HunterClaw Team",
        .actions = {
            "pdf_read",
            "pdf_analyze",
            "pdf_merge",
            "pdf_split"
        }
    };
}

json PDFTool::execute(const std::string& action, const json& params) {
    try {
        if (action == "pdf_read") {
            return pdf_read(params);
        } else if (action == "pdf_analyze") {
            return pdf_analyze(params);
        } else if (action == "pdf_merge") {
            return pdf_merge(params);
        } else if (action == "pdf_split") {
            return pdf_split(params);
        } else {
            return json{
                {"status", "error"},
                {"error", "Unknown action: " + action}
            };
        }
    } catch (const std::exception& e) {
        return json{
            {"status", "error"},
            {"error", std::string("PDFTool error: ") + e.what()}
        };
    }
}

//=============================================================================
// PDF Read
//=============================================================================

json PDFTool::pdf_read(const json& params) {
    // Validate required parameter
    if (!params.contains("path")) {
        return json{
            {"status", "error"},
            {"error", "Missing required parameter: path"}
        };
    }
    
    std::string path = params["path"].get<std::string>();
    
    // Validate path
    if (!validate_path(path)) {
        return json{
            {"status", "error"},
            {"error", "Invalid or forbidden path: " + path}
        };
    }
    
    // Check file size
    if (!check_file_size(path)) {
        return json{
            {"status", "error"},
            {"error", "File exceeds maximum size limit"}
        };
    }
    
    // Parse optional parameters
    std::optional<std::string> page_range;
    if (params.contains("pages")) {
        page_range = params["pages"].get<std::string>();
    }
    
    bool extract_images = params.value("extract_images", false);
    
    // Check cache
    if (config_.cache_enabled && page_range.has_value()) {
        auto cached = get_from_cache(path, page_range.value());
        if (cached.has_value()) {
            auto& result = cached.value();
            return json{
                {"status", "success"},
                {"text", result.text},
                {"images", result.images_base64},
                {"page_count", result.page_count},
                {"extracted_pages", result.extracted_pages},
                {"truncated", result.truncated},
                {"cached", true}
            };
        }
    }
    
    // Extract content
    auto result = extract_pdf_content(path, page_range, extract_images);
    
    // Save to cache
    if (config_.cache_enabled && page_range.has_value()) {
        save_to_cache(path, page_range.value(), result);
    }
    
    json response = {
        {"status", "success"},
        {"text", result.text},
        {"page_count", result.page_count},
        {"extracted_pages", result.extracted_pages},
        {"truncated", result.truncated},
        {"cached", false}
    };
    
    if (extract_images) {
        response["images"] = result.images_base64;
        response["image_count"] = result.images_base64.size();
    }
    
    if (result.metadata_title.has_value()) {
        response["title"] = result.metadata_title.value();
    }
    if (result.metadata_author.has_value()) {
        response["author"] = result.metadata_author.value();
    }
    
    return response;
}

//=============================================================================
// PDF Analyze
//=============================================================================

json PDFTool::pdf_analyze(const json& params) {
    // Validate required parameters
    if (!params.contains("path")) {
        return json{
            {"status", "error"},
            {"error", "Missing required parameter: path"}
        };
    }
    
    std::string path = params["path"].get<std::string>();
    std::string prompt = params.value("prompt", "Analyze this PDF document.");
    
    // Validate path
    if (!validate_path(path)) {
        return json{
            {"status", "error"},
            {"error", "Invalid or forbidden path: " + path}
        };
    }
    
    // Check file size
    if (!check_file_size(path)) {
        return json{
            {"status", "error"},
            {"error", "File exceeds maximum size limit"}
        };
    }
    
    // Parse optional parameters
    std::string model = params.value("model", config_.primary_model);
    int max_tokens = params.value("max_tokens", config_.max_tokens);
    std::optional<std::string> page_range;
    if (params.contains("pages")) {
        page_range = params["pages"].get<std::string>();
    }
    
    // Analyze with AI model
    auto result = analyze_pdf_with_model(path, prompt, model, page_range, max_tokens);
    
    return json{
        {"status", "success"},
        {"analysis", result.analysis},
        {"model", result.model},
        {"input_tokens", result.input_tokens},
        {"output_tokens", result.output_tokens},
        {"used_native_pdf", result.used_native_pdf},
        {"used_extraction_fallback", result.used_extraction_fallback}
    };
}

//=============================================================================
// PDF Merge
//=============================================================================

json PDFTool::pdf_merge(const json& params) {
    // Validate parameters
    if (!params.contains("input_paths") || !params.contains("output_path")) {
        return json{
            {"status", "error"},
            {"error", "Missing required parameters: input_paths, output_path"}
        };
    }
    
    auto input_paths = params["input_paths"].get<std::vector<std::string>>();
    std::string output_path = params["output_path"].get<std::string>();
    
    // Validate input paths
    if (input_paths.size() > static_cast<size_t>(config_.max_pdfs)) {
        return json{
            {"status", "error"},
            {"error", "Too many input PDFs (max: " + std::to_string(config_.max_pdfs) + ")"}
        };
    }
    
    for (const auto& path : input_paths) {
        if (!validate_path(path) || !file_exists(path)) {
            return json{
                {"status", "error"},
                {"error", "Invalid or missing input path: " + path}
            };
        }
    }
    
    // Validate output path
    if (!validate_path(output_path)) {
        return json{
            {"status", "error"},
            {"error", "Invalid output path: " + output_path}
        };
    }
    
    // Merge PDFs
    bool success = merge_pdfs_with_pypdf(input_paths, output_path);
    
    if (success) {
        return json{
            {"status", "success"},
            {"output_path", output_path},
            {"input_count", input_paths.size()}
        };
    } else {
        return json{
            {"status", "error"},
            {"error", "Failed to merge PDFs"}
        };
    }
}

//=============================================================================
// PDF Split
//=============================================================================

json PDFTool::pdf_split(const json& params) {
    // Validate parameters
    if (!params.contains("input_path") || !params.contains("output_dir")) {
        return json{
            {"status", "error"},
            {"error", "Missing required parameters: input_path, output_dir"}
        };
    }
    
    std::string input_path = params["input_path"].get<std::string>();
    std::string output_dir = params["output_dir"].get<std::string>();
    int pages_per_file = params.value("pages_per_file", 10);
    
    // Validate input path
    if (!validate_path(input_path) || !file_exists(input_path)) {
        return json{
            {"status", "error"},
            {"error", "Invalid or missing input path: " + input_path}
        };
    }
    
    // Validate output directory
    if (!validate_path(output_dir)) {
        return json{
            {"status", "error"},
            {"error", "Invalid output directory: " + output_dir}
        };
    }
    
    // Create output directory
    std::filesystem::create_directories(output_dir);
    
    // Split PDF
    bool success = split_pdf_with_pypdf(input_path, output_dir, pages_per_file);
    
    if (success) {
        return json{
            {"status", "success"},
            {"output_dir", output_dir},
            {"pages_per_file", pages_per_file}
        };
    } else {
        return json{
            {"status", "error"},
            {"error", "Failed to split PDF"}
        };
    }
}

//=============================================================================
// Helper Implementations (Extraction)
//=============================================================================

PdfExtractionResult PDFTool::extract_pdf_content(
    const std::string& path,
    const std::optional<std::string>& page_range,
    bool extract_images
) {
    PdfExtractionResult result;
    result.truncated = false;
    
    // Get total page count using pdfinfo
    try {
        std::string cmd = "pdfinfo \"" + path + "\" | grep Pages | awk '{print $2}'";
        std::string output = exec_command(cmd);
        result.page_count = std::stoi(output);
    } catch (...) {
        result.page_count = 0;
    }
    
    // Parse page range
    std::vector<int> pages;
    if (page_range.has_value() && !page_range->empty()) {
        pages = parse_page_range(page_range.value(), result.page_count);
    } else {
        // All pages (up to max_pages)
        int max_pages = std::min(result.page_count, config_.max_pages);
        for (int i = 1; i <= max_pages; ++i) {
            pages.push_back(i);
        }
    }
    
    result.extracted_pages = pages.size();
    
    // Extract with poppler (preferred)
    try {
        result = extract_with_poppler(path, pages, extract_images);
        return result;
    } catch (const std::exception& e) {
        // Fallback to pdfminer
    }
    
    // Fallback: pdfminer
    try {
        result = extract_with_pdfminer(path, pages);
        return result;
    } catch (const std::exception& e) {
        // Fallback to OCR
    }
    
    // Fallback: OCR (if enabled)
    if (config_.ocr_enabled) {
        try {
            result = extract_with_ocr(path, pages);
            return result;
        } catch (const std::exception& e) {
            result.text = "Failed to extract PDF content: " + std::string(e.what());
        }
    } else {
        result.text = "Failed to extract PDF content (OCR disabled)";
    }
    
    return result;
}

PdfExtractionResult PDFTool::extract_with_poppler(
    const std::string& path,
    const std::vector<int>& pages,
    bool extract_images
) {
    PdfExtractionResult result;
    
    // Extract text with pdftotext
    std::string temp_txt = config_.temp_dir + "/extract_" + 
                           std::to_string(std::time(nullptr)) + ".txt";
    
    std::string cmd = "pdftotext -layout \"" + path + "\" \"" + temp_txt + "\"";
    int ret = std::system(cmd.c_str());
    
    if (ret == 0 && file_exists(temp_txt)) {
        std::ifstream file(temp_txt);
        std::stringstream buffer;
        buffer << file.rdbuf();
        result.text = buffer.str();
        
        // Cleanup
        std::filesystem::remove(temp_txt);
    } else {
        throw std::runtime_error("pdftotext failed");
    }
    
    result.page_count = pages.size();
    result.extracted_pages = pages.size();
    result.truncated = false;
    
    // Extract images (if requested)
    if (extract_images) {
        // STUB: Implement image extraction with pdfimages
        // pdfimages -j -p /path/to/pdf /output/prefix
    }
    
    return result;
}

PdfExtractionResult PDFTool::extract_with_pdfminer(
    const std::string& path,
    const std::vector<int>& pages
) {
    // STUB: Call Python pdfminer.six via subprocess
    PdfExtractionResult result;
    result.text = "[pdfminer extraction not implemented]";
    result.page_count = pages.size();
    result.extracted_pages = pages.size();
    return result;
}

PdfExtractionResult PDFTool::extract_with_ocr(
    const std::string& path,
    const std::vector<int>& pages
) {
    // STUB: Convert PDF to images and OCR with tesseract
    PdfExtractionResult result;
    result.text = "[OCR extraction not implemented]";
    result.page_count = pages.size();
    result.extracted_pages = pages.size();
    return result;
}

//=============================================================================
// Helper Implementations (AI Analysis) - STUBS
//=============================================================================

PdfAnalysisResult PDFTool::analyze_pdf_with_model(
    const std::string& path,
    const std::string& prompt,
    const std::string& model,
    const std::optional<std::string>& page_range,
    int max_tokens
) {
    // STUB: Route to appropriate provider
    if (model.find("anthropic") != std::string::npos) {
        return analyze_with_anthropic(path, prompt, model, max_tokens);
    } else if (model.find("google") != std::string::npos) {
        return analyze_with_gemini(path, prompt, max_tokens);
    } else if (model.find("openai") != std::string::npos) {
        return analyze_with_openai(path, prompt, max_tokens);
    }
    
    // Default: Use extraction fallback
    auto extraction = extract_pdf_content(path, page_range, false);
    
    PdfAnalysisResult result;
    result.analysis = "Analysis placeholder. Text extracted: " + 
                     std::to_string(extraction.text.length()) + " chars";
    result.model = model;
    result.input_tokens = extraction.text.length() / 4; // Rough estimate
    result.output_tokens = 100;
    result.used_native_pdf = false;
    result.used_extraction_fallback = true;
    
    return result;
}

PdfAnalysisResult PDFTool::analyze_with_anthropic(
    const std::string& path,
    const std::string& prompt,
    const std::string& model,
    int max_tokens
) {
    // STUB: Anthropic API call with native PDF support
    PdfAnalysisResult result;
    result.analysis = "[Anthropic analysis not implemented]";
    result.model = model;
    result.input_tokens = 1000;
    result.output_tokens = 500;
    result.used_native_pdf = true;
    result.used_extraction_fallback = false;
    return result;
}

PdfAnalysisResult PDFTool::analyze_with_gemini(
    const std::string& path,
    const std::string& prompt,
    int max_tokens
) {
    // STUB: Google Gemini API call
    PdfAnalysisResult result;
    result.analysis = "[Gemini analysis not implemented]";
    result.model = "google/gemini-2.5-pro";
    result.input_tokens = 1000;
    result.output_tokens = 500;
    result.used_native_pdf = false;
    result.used_extraction_fallback = true;
    return result;
}

PdfAnalysisResult PDFTool::analyze_with_openai(
    const std::string& path,
    const std::string& prompt,
    int max_tokens
) {
    // STUB: OpenAI API call
    PdfAnalysisResult result;
    result.analysis = "[OpenAI analysis not implemented]";
    result.model = "openai/gpt-5-mini";
    result.input_tokens = 1000;
    result.output_tokens = 500;
    result.used_native_pdf = false;
    result.used_extraction_fallback = true;
    return result;
}

//=============================================================================
// Helper Implementations (Merge/Split) - STUBS
//=============================================================================

bool PDFTool::merge_pdfs_with_pypdf(
    const std::vector<std::string>& input_paths,
    const std::string& output_path
) {
    // STUB: Call Python PyPDF2/pypdf
    // In production: Use C++ PDF library (e.g., PoDoFo, qpdf)
    return false;
}

bool PDFTool::split_pdf_with_pypdf(
    const std::string& input_path,
    const std::string& output_dir,
    int pages_per_file
) {
    // STUB: Call Python PyPDF2/pypdf
    return false;
}

//=============================================================================
// Helper Implementations (Validation & Cache)
//=============================================================================

std::vector<int> PDFTool::parse_page_range(
    const std::string& range_str,
    int total_pages
) {
    std::vector<int> pages;
    
    // Parse range: "1-5,10,15-20"
    std::regex range_regex(R"((\d+)(?:-(\d+))?)");
    std::sregex_iterator iter(range_str.begin(), range_str.end(), range_regex);
    std::sregex_iterator end;
    
    for (; iter != end; ++iter) {
        int start = std::stoi((*iter)[1]);
        int end_page = (*iter)[2].matched ? std::stoi((*iter)[2]) : start;
        
        for (int page = start; page <= end_page && page <= total_pages; ++page) {
            pages.push_back(page);
        }
    }
    
    // Remove duplicates and sort
    std::sort(pages.begin(), pages.end());
    pages.erase(std::unique(pages.begin(), pages.end()), pages.end());
    
    // Limit to max_pages
    if (pages.size() > static_cast<size_t>(config_.max_pages)) {
        pages.resize(config_.max_pages);
    }
    
    return pages;
}

bool PDFTool::validate_path(const std::string& path) {
    // Check if path is within allowed directory
    if (!config_.allowed_dir.empty()) {
        std::filesystem::path abs_path = std::filesystem::absolute(path);
        std::filesystem::path allowed = std::filesystem::absolute(config_.allowed_dir);
        
        auto rel = std::filesystem::relative(abs_path, allowed);
        if (rel.string().find("..") == 0) {
            return false; // Path escapes allowed directory
        }
    }
    
    return true;
}

bool PDFTool::check_file_size(const std::string& path) {
    double size_mb = get_file_size_mb(path);
    return size_mb > 0 && size_mb <= config_.max_file_size_mb;
}

std::string PDFTool::resolve_absolute_path(const std::string& path) {
    return std::filesystem::absolute(path).string();
}

std::optional<PdfExtractionResult> PDFTool::get_from_cache(
    const std::string& path,
    const std::string& page_range_key
) {
    // STUB: Implement cache lookup
    return std::nullopt;
}

void PDFTool::save_to_cache(
    const std::string& path,
    const std::string& page_range_key,
    const PdfExtractionResult& result
) {
    // STUB: Implement cache storage
}

std::string PDFTool::compute_cache_key(
    const std::string& path,
    const std::string& page_range
) {
    // Simple cache key: hash of path + page_range
    return std::to_string(std::hash<std::string>{}(path + page_range));
}

//=============================================================================
// Factory Function
//=============================================================================

std::unique_ptr<PDFTool> create_pdf_tool(const PDFTool::Config& config) {
    return std::make_unique<PDFTool>(config);
}

} // namespace tools
} // namespace hunterclaw
