// pdf_tool.cc
// Strictly aligned with OpenClaw: src/agents/tools/pdf-tool.ts
//
// Tool: pdf
// Actions: pdf_read | pdf_analyze | pdf_merge | pdf_split
// Providers: poppler | pdfminer | tesseract (extraction)
//            anthropic | gemini | openai (analysis)

#include "hunterclaw/tools/pdf_tool.h"
#include "hunterclaw/core/json.h"
#include <nlohmann/json.hpp>
#include <string>
#include <vector>
#include <optional>
#include <sstream>
#include <fstream>
#include <filesystem>
#include <cstdlib>
#include <cstdio>
#include <regex>
#include <algorithm>
#include <chrono>
#include <unordered_map>

namespace hunterclaw {
namespace tools {

using json = nlohmann::json;
namespace fs = std::filesystem;

// ============================================================================
// Constants (aligned with OpenClaw pdf-tool.ts)
// ============================================================================

// 🎯 对齐 OpenClaw: DEFAULT_MAX_FILE_SIZE_MB = 10
static constexpr int DEFAULT_MAX_FILE_SIZE_MB = 10;
// 🎯 对齐 OpenClaw: DEFAULT_MAX_PAGES = 20
static constexpr int DEFAULT_MAX_PAGES = 20;
// 🎯 对齐 OpenClaw: DEFAULT_MAX_PDFS = 10
static constexpr int DEFAULT_MAX_PDFS = 10;
// 🎯 对齐 OpenClaw: DEFAULT_MAX_TOKENS = 4096
static constexpr int DEFAULT_MAX_TOKENS = 4096;

// ============================================================================
// Helpers
// ============================================================================

static std::string trim(const std::string& s) {
    size_t start = s.find_first_not_of(" \t\r\n");
    if (start == std::string::npos) return "";
    size_t end = s.find_last_not_of(" \t\r\n");
    return s.substr(start, end - start + 1);
}

static std::string exec_command(const std::string& cmd) {
    std::string result;
    FILE* pipe = popen(cmd.c_str(), "r");
    if (!pipe) return "";
    char buf[4096];
    while (fgets(buf, sizeof(buf), pipe)) result += buf;
    pclose(pipe);
    return result;
}

// ============================================================================
// PDFTool implementation
// ============================================================================

PDFTool::PDFTool(const Config& config) : config_(config) {
    // Apply defaults
    if (config_.max_file_size_mb <= 0) config_.max_file_size_mb = DEFAULT_MAX_FILE_SIZE_MB;
    if (config_.max_pages <= 0)        config_.max_pages        = DEFAULT_MAX_PAGES;
    if (config_.max_pdfs <= 0)         config_.max_pdfs         = DEFAULT_MAX_PDFS;
    if (config_.max_tokens <= 0)       config_.max_tokens       = DEFAULT_MAX_TOKENS;
    if (config_.temp_dir.empty())      config_.temp_dir         = "/tmp";
    if (config_.primary_model.empty()) config_.primary_model    = "anthropic/claude-opus-4-6";
}

std::string PDFTool::description() const {
    return "Read, analyze, merge, and split PDF documents. "
           "Supports native PDF analysis (Anthropic/Gemini) and text extraction (poppler/OCR). "
           "Actions: pdf_read, pdf_analyze, pdf_merge, pdf_split.";
}

Tool::Schema PDFTool::parameters() const {
    Schema schema;
    schema.type = "object";
    schema.description = description();
    schema.properties["action"] = core::JsonValue(json{{"type","string"},{"enum",json::array({"pdf_read","pdf_analyze","pdf_merge","pdf_split"})},{"description","Action to perform"}});
    schema.properties["path"]   = core::JsonValue(json{{"type","string"},{"description","Path to PDF file"}});
    schema.properties["pages"]  = core::JsonValue(json{{"type","string"},{"description","Page range (e.g. '1-5', '1,3,5-10')"}});
    schema.properties["extractImages"] = core::JsonValue(json{{"type","boolean"},{"description","Extract images from PDF"}});
    schema.properties["prompt"] = core::JsonValue(json{{"type","string"},{"description","Analysis prompt for pdf_analyze"}});
    schema.properties["model"]  = core::JsonValue(json{{"type","string"},{"description","AI model for pdf_analyze"}});
    schema.properties["maxTokens"] = core::JsonValue(json{{"type","number"},{"description","Max output tokens for pdf_analyze"}});
    schema.properties["inputPaths"] = core::JsonValue(json{{"type","array"},{"items",json{{"type","string"}}},{"description","Input PDF paths for pdf_merge"}});
    schema.properties["outputPath"] = core::JsonValue(json{{"type","string"},{"description","Output PDF path for pdf_merge/pdf_split"}});
    schema.properties["outputDir"]  = core::JsonValue(json{{"type","string"},{"description","Output directory for pdf_split"}});
    schema.properties["pagesPerFile"] = core::JsonValue(json{{"type","number"},{"description","Pages per file for pdf_split"}});
    schema.required = {"action"};
    return schema;
}

ToolResult PDFTool::execute(const core::JsonValue& params_val) {
    json params = params_val.to_json();
    if (!params.contains("action") || !params["action"].is_string())
        return tool_result::error("action is required");
    std::string action = params["action"].get<std::string>();
    json result;
    if (action == "pdf_read")    result = pdf_read(params);
    else if (action == "pdf_analyze") result = pdf_analyze(params);
    else if (action == "pdf_merge")   result = pdf_merge(params);
    else if (action == "pdf_split")   result = pdf_split(params);
    else return tool_result::error("Unknown action: " + action);
    if (result.contains("error"))
        return tool_result::error(result["error"].get<std::string>());
    return tool_result::json(core::JsonValue(result));
}

// ============================================================================
// pdf_read
// ============================================================================

json PDFTool::pdf_read(const json& params) {
    // 🎯 对齐 OpenClaw: pdf_read action
    if (!params.contains("path") || !params["path"].is_string())
        return json{{"error", "path is required"}};
    std::string path = trim(params["path"].get<std::string>());
    if (!validate_path(path))
        return json{{"error", "Invalid or disallowed path: " + path}};
    if (!check_file_size(path))
        return json{{"error", "File too large (max " + std::to_string(config_.max_file_size_mb) + "MB)"}};

    std::optional<std::string> page_range;
    if (params.contains("pages") && params["pages"].is_string()) {
        std::string pr = trim(params["pages"].get<std::string>());
        if (!pr.empty()) page_range = pr;
    }

    bool extract_images = params.value("extractImages", false);

    // Check cache
    std::string cache_key_str = compute_cache_key(path, page_range.value_or(""));
    if (config_.cache_enabled) {
        auto cached = get_from_cache(path, page_range.value_or(""));
        if (cached.has_value()) {
            const auto& r = cached.value();
            json result = {
                {"text",           r.text},
                {"pageCount",      r.page_count},
                {"extractedPages", r.extracted_pages},
                {"truncated",      r.truncated},
                {"cached",         true},
            };
            if (r.metadata_title.has_value())  result["title"]  = r.metadata_title.value();
            if (r.metadata_author.has_value()) result["author"] = r.metadata_author.value();
            if (!r.images_base64.empty())      result["imageCount"] = (int)r.images_base64.size();
            return result;
        }
    }

    auto extraction = extract_pdf_content(path, page_range, extract_images);

    if (config_.cache_enabled) {
        save_to_cache(path, page_range.value_or(""), extraction);
    }

    json result = {
        {"text",           extraction.text},
        {"pageCount",      extraction.page_count},
        {"extractedPages", extraction.extracted_pages},
        {"truncated",      extraction.truncated},
    };
    if (extraction.metadata_title.has_value())  result["title"]  = extraction.metadata_title.value();
    if (extraction.metadata_author.has_value()) result["author"] = extraction.metadata_author.value();
    if (!extraction.images_base64.empty())      result["imageCount"] = (int)extraction.images_base64.size();
    return result;
}

// ============================================================================
// pdf_analyze
// ============================================================================

json PDFTool::pdf_analyze(const json& params) {
    // 🎯 对齐 OpenClaw: pdf_analyze action (AI-powered)
    if (!params.contains("path") || !params["path"].is_string())
        return json{{"error", "path is required"}};
    std::string path = trim(params["path"].get<std::string>());
    if (!validate_path(path))
        return json{{"error", "Invalid or disallowed path: " + path}};
    if (!check_file_size(path))
        return json{{"error", "File too large (max " + std::to_string(config_.max_file_size_mb) + "MB)"}};

    std::string prompt = "Analyze this PDF document and provide a comprehensive summary.";
    if (params.contains("prompt") && params["prompt"].is_string())
        prompt = trim(params["prompt"].get<std::string>());

    std::string model = config_.primary_model;
    if (params.contains("model") && params["model"].is_string()) {
        std::string m = trim(params["model"].get<std::string>());
        if (!m.empty()) model = m;
    }

    int max_tokens = config_.max_tokens;
    if (params.contains("maxTokens") && params["maxTokens"].is_number())
        max_tokens = std::max(1, (int)params["maxTokens"].get<double>());

    std::optional<std::string> page_range;
    if (params.contains("pages") && params["pages"].is_string()) {
        std::string pr = trim(params["pages"].get<std::string>());
        if (!pr.empty()) page_range = pr;
    }

    auto analysis = analyze_pdf_with_model(path, prompt, model, page_range, max_tokens);

    return json{
        {"analysis",              analysis.analysis},
        {"model",                 analysis.model},
        {"inputTokens",           analysis.input_tokens},
        {"outputTokens",          analysis.output_tokens},
        {"usedNativePdf",         analysis.used_native_pdf},
        {"usedExtractionFallback",analysis.used_extraction_fallback},
    };
}

// ============================================================================
// pdf_merge
// ============================================================================

json PDFTool::pdf_merge(const json& params) {
    // 🎯 对齐 OpenClaw: pdf_merge action
    if (!params.contains("inputPaths") || !params["inputPaths"].is_array())
        return json{{"error", "inputPaths is required"}};
    if (!params.contains("outputPath") || !params["outputPath"].is_string())
        return json{{"error", "outputPath is required"}};

    std::vector<std::string> input_paths;
    for (const auto& p : params["inputPaths"]) {
        if (!p.is_string()) continue;
        std::string path = trim(p.get<std::string>());
        if (!validate_path(path)) return json{{"error", "Invalid path: " + path}};
        input_paths.push_back(path);
    }
    if (input_paths.empty()) return json{{"error", "inputPaths must not be empty"}};
    if ((int)input_paths.size() > config_.max_pdfs)
        return json{{"error", "Too many PDFs (max " + std::to_string(config_.max_pdfs) + ")"}};

    std::string output_path = trim(params["outputPath"].get<std::string>());
    if (!validate_path(output_path)) return json{{"error", "Invalid outputPath"}};

    bool ok = merge_pdfs_with_pypdf(input_paths, output_path);
    if (!ok) return json{{"error", "PDF merge failed"}};

    return json{
        {"ok",         true},
        {"outputPath", output_path},
        {"inputCount", (int)input_paths.size()},
    };
}

// ============================================================================
// pdf_split
// ============================================================================

json PDFTool::pdf_split(const json& params) {
    // 🎯 对齐 OpenClaw: pdf_split action
    if (!params.contains("path") || !params["path"].is_string())
        return json{{"error", "path is required"}};
    std::string input_path = trim(params["path"].get<std::string>());
    if (!validate_path(input_path)) return json{{"error", "Invalid path"}};

    std::string output_dir = config_.temp_dir;
    if (params.contains("outputDir") && params["outputDir"].is_string()) {
        std::string d = trim(params["outputDir"].get<std::string>());
        if (!d.empty()) output_dir = d;
    }

    int pages_per_file = 1;
    if (params.contains("pagesPerFile") && params["pagesPerFile"].is_number())
        pages_per_file = std::max(1, (int)params["pagesPerFile"].get<double>());

    bool ok = split_pdf_with_pypdf(input_path, output_dir, pages_per_file);
    if (!ok) return json{{"error", "PDF split failed"}};

    // List output files
    json output_files = json::array();
    try {
        for (const auto& entry : fs::directory_iterator(output_dir)) {
            if (entry.path().extension() == ".pdf") {
                output_files.push_back(entry.path().string());
            }
        }
    } catch (...) {}

    return json{
        {"ok",          true},
        {"outputDir",   output_dir},
        {"outputFiles", output_files},
    };
}

// ============================================================================
// Extraction
// ============================================================================

PdfExtractionResult PDFTool::extract_pdf_content(
    const std::string& path,
    const std::optional<std::string>& page_range,
    bool extract_images
) {
    // 🎯 对齐 OpenClaw: extraction fallback chain
    // Try poppler first, then pdfminer, then OCR

    // Get total page count
    int total_pages = 0;
    {
        std::string cmd = "pdfinfo \"" + path + "\" 2>/dev/null | grep 'Pages:' | awk '{print $2}'";
        std::string out = exec_command(cmd);
        try { total_pages = std::stoi(trim(out)); } catch (...) { total_pages = 1; }
    }

    std::vector<int> pages;
    if (page_range.has_value()) {
        pages = parse_page_range(page_range.value(), total_pages);
    } else {
        int limit = std::min(total_pages, config_.max_pages);
        for (int i = 1; i <= limit; ++i) pages.push_back(i);
    }

    // Try poppler
    auto result = extract_with_poppler(path, pages, extract_images);
    if (!result.text.empty()) return result;

    // Try pdfminer
    result = extract_with_pdfminer(path, pages);
    if (!result.text.empty()) return result;

    // Try OCR
    if (config_.ocr_enabled) {
        result = extract_with_ocr(path, pages);
    }

    result.page_count = total_pages;
    result.extracted_pages = (int)pages.size();
    return result;
}

PdfExtractionResult PDFTool::extract_with_poppler(
    const std::string& path,
    const std::vector<int>& pages,
    bool extract_images
) {
    PdfExtractionResult result;
    result.page_count = 0;
    result.extracted_pages = 0;
    result.truncated = false;

    if (pages.empty()) return result;

    // Build page range for pdftotext
    int first = pages.front();
    int last  = pages.back();

    std::string cmd = "pdftotext -f " + std::to_string(first) +
                      " -l " + std::to_string(last) +
                      " \"" + path + "\" - 2>/dev/null";
    result.text = exec_command(cmd);
    result.extracted_pages = (int)pages.size();

    // Extract images if requested
    if (extract_images && !pages.empty()) {
        std::string img_dir = config_.temp_dir + "/pdf_images_" + std::to_string(
            std::chrono::system_clock::now().time_since_epoch().count());
        fs::create_directories(img_dir);
        std::string img_cmd = "pdfimages -f " + std::to_string(first) +
                              " -l " + std::to_string(last) +
                              " -png \"" + path + "\" \"" + img_dir + "/img\" 2>/dev/null";
        exec_command(img_cmd);
        // Read extracted images
        try {
            for (const auto& entry : fs::directory_iterator(img_dir)) {
                if (entry.path().extension() == ".png") {
                    std::ifstream f(entry.path(), std::ios::binary);
                    if (!f.is_open()) continue;
                    std::string data((std::istreambuf_iterator<char>(f)),
                                      std::istreambuf_iterator<char>());
                    // Base64 encode
                    // Simple base64 encoding
                    static const char* b64 =
                        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
                    std::string encoded;
                    for (size_t i = 0; i < data.size(); i += 3) {
                        unsigned char a = (unsigned char)data[i];
                        unsigned char b = (i+1 < data.size()) ? (unsigned char)data[i+1] : 0;
                        unsigned char c = (i+2 < data.size()) ? (unsigned char)data[i+2] : 0;
                        encoded += b64[a >> 2];
                        encoded += b64[((a & 3) << 4) | (b >> 4)];
                        encoded += (i+1 < data.size()) ? b64[((b & 15) << 2) | (c >> 6)] : '=';
                        encoded += (i+2 < data.size()) ? b64[c & 63] : '=';
                    }
                    result.images_base64.push_back(encoded);
                }
            }
        } catch (...) {}
    }

    return result;
}

PdfExtractionResult PDFTool::extract_with_pdfminer(
    const std::string& path,
    const std::vector<int>& pages
) {
    PdfExtractionResult result;
    result.page_count = 0;
    result.extracted_pages = 0;
    result.truncated = false;

    // Build page list for pdfminer
    std::string page_list;
    for (size_t i = 0; i < pages.size(); ++i) {
        if (i > 0) page_list += ",";
        page_list += std::to_string(pages[i]);
    }

    std::string cmd = "python3 -c \""
        "from pdfminer.high_level import extract_text; "
        "import sys; "
        "print(extract_text(sys.argv[1]))\" \"" + path + "\" 2>/dev/null";
    result.text = exec_command(cmd);
    result.extracted_pages = (int)pages.size();
    return result;
}

PdfExtractionResult PDFTool::extract_with_ocr(
    const std::string& path,
    const std::vector<int>& pages
) {
    PdfExtractionResult result;
    result.page_count = 0;
    result.extracted_pages = 0;
    result.truncated = false;

    // Convert PDF pages to images, then OCR
    std::string img_dir = config_.temp_dir + "/pdf_ocr_" + std::to_string(
        std::chrono::system_clock::now().time_since_epoch().count());
    fs::create_directories(img_dir);

    int first = pages.empty() ? 1 : pages.front();
    int last  = pages.empty() ? 1 : pages.back();

    // Convert to images using pdftoppm
    std::string cmd = "pdftoppm -f " + std::to_string(first) +
                      " -l " + std::to_string(last) +
                      " -r 150 \"" + path + "\" \"" + img_dir + "/page\" 2>/dev/null";
    exec_command(cmd);

    // OCR each image
    std::string text;
    try {
        std::vector<fs::path> img_files;
        for (const auto& entry : fs::directory_iterator(img_dir)) {
            if (entry.path().extension() == ".ppm" ||
                entry.path().extension() == ".png" ||
                entry.path().extension() == ".jpg") {
                img_files.push_back(entry.path());
            }
        }
        std::sort(img_files.begin(), img_files.end());
        for (const auto& img : img_files) {
            std::string ocr_cmd = "tesseract \"" + img.string() + "\" stdout 2>/dev/null";
            text += exec_command(ocr_cmd) + "\n";
        }
    } catch (...) {}

    result.text = trim(text);
    result.extracted_pages = (int)pages.size();
    return result;
}

// ============================================================================
// AI Analysis
// ============================================================================

PdfAnalysisResult PDFTool::analyze_pdf_with_model(
    const std::string& path,
    const std::string& prompt,
    const std::string& model,
    const std::optional<std::string>& page_range,
    int max_tokens
) {
    // 🎯 对齐 OpenClaw: try native PDF, fallback to extraction
    std::string model_lower = model;
    std::transform(model_lower.begin(), model_lower.end(), model_lower.begin(), ::tolower);

    if (model_lower.find("anthropic") != std::string::npos ||
        model_lower.find("claude") != std::string::npos) {
        if (config_.anthropic_api_key.has_value()) {
            return analyze_with_anthropic(path, prompt, model, max_tokens);
        }
    }
    if (model_lower.find("gemini") != std::string::npos ||
        model_lower.find("google") != std::string::npos) {
        if (config_.google_api_key.has_value()) {
            return analyze_with_gemini(path, prompt, max_tokens);
        }
    }
    if (model_lower.find("openai") != std::string::npos ||
        model_lower.find("gpt") != std::string::npos) {
        if (config_.openai_api_key.has_value()) {
            return analyze_with_openai(path, prompt, max_tokens);
        }
    }

    // Fallback: extract text and return
    auto extraction = extract_pdf_content(path, page_range, false);
    PdfAnalysisResult result;
    result.analysis = "Extracted text (no AI model available):\n\n" + extraction.text;
    result.model = "extraction-fallback";
    result.input_tokens = 0;
    result.output_tokens = 0;
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
    // 🎯 对齐 OpenClaw: Anthropic native PDF support
    // Read PDF as base64
    std::ifstream f(path, std::ios::binary);
    if (!f.is_open()) {
        PdfAnalysisResult r;
        r.analysis = "Failed to open PDF";
        r.model = model;
        r.used_native_pdf = false;
        r.used_extraction_fallback = true;
        return r;
    }
    std::string pdf_data((std::istreambuf_iterator<char>(f)),
                          std::istreambuf_iterator<char>());

    // TODO: call Anthropic API with native PDF support
    // For now, return stub
    PdfAnalysisResult result;
    result.analysis = "[Anthropic native PDF analysis - stub]";
    result.model = model;
    result.input_tokens = 0;
    result.output_tokens = 0;
    result.used_native_pdf = true;
    result.used_extraction_fallback = false;
    return result;
}

PdfAnalysisResult PDFTool::analyze_with_gemini(
    const std::string& path,
    const std::string& prompt,
    int max_tokens
) {
    // TODO: call Gemini API with native PDF support
    PdfAnalysisResult result;
    result.analysis = "[Gemini native PDF analysis - stub]";
    result.model = "google/gemini-2.5-pro";
    result.input_tokens = 0;
    result.output_tokens = 0;
    result.used_native_pdf = true;
    result.used_extraction_fallback = false;
    return result;
}

PdfAnalysisResult PDFTool::analyze_with_openai(
    const std::string& path,
    const std::string& prompt,
    int max_tokens
) {
    // 🎯 对齐 OpenClaw: OpenAI PDF analysis (text extraction + chat)
    (void)max_tokens;

    // OpenAI doesn't support native PDF — use text extraction fallback
    auto extracted = extract_text_from_pdf(path);
    std::string text_content = extracted.is_ok() ? extracted.get() : "[extraction failed]";

    const char* api_key = std::getenv("OPENAI_API_KEY");
    infra::HttpClient client;
    infra::HttpRequestOptions opts;
    json request_body = {
        {"model", "gpt-4o"},
        {"messages", json::array({
            {{"role", "user"}, {"content", prompt + "\n\n---\n\nDocument content:\n" + text_content.substr(0, 100000)}}
        })},
        {"max_tokens", 4096}
    };
    opts.body = request_body.dump();
    opts.headers = {
        {"Content-Type", "application/json"},
        {"Authorization", std::string("Bearer ") + (api_key ? api_key : "")}
    };
    opts.timeout_ms = 60000;

    auto http_result = client.post("https://api.openai.com/v1/chat/completions", opts);
    PdfAnalysisResult result;
    result.model = "openai/gpt-4o";
    result.used_native_pdf = false;
    result.used_extraction_fallback = true;

    if (http_result.is_ok()) {
        try {
            auto resp = json::parse(http_result.get().body);
            if (resp.contains("choices") && !resp["choices"].empty()) {
                result.analysis = resp["choices"][0]["message"].value("content", "");
            }
            if (resp.contains("usage")) {
                result.input_tokens = resp["usage"].value("prompt_tokens", 0);
                result.output_tokens = resp["usage"].value("completion_tokens", 0);
            }
        } catch (...) {
            result.analysis = "[OpenAI PDF analysis failed: parse error]";
        }
    } else {
        result.analysis = "[OpenAI PDF analysis failed: " + http_result.error + "]";
    }
    return result;
}

// ============================================================================
// PDF manipulation
// ============================================================================

bool PDFTool::merge_pdfs_with_pypdf(
    const std::vector<std::string>& input_paths,
    const std::string& output_path
) {
    // Build Python script
    std::string script = "from pypdf import PdfWriter; w = PdfWriter();\n";
    for (const auto& p : input_paths) {
        script += "w.append('" + p + "');\n";
    }
    script += "w.write('" + output_path + "');\n";

    std::string cmd = "python3 -c \"" + script + "\" 2>/dev/null";
    int ret = std::system(cmd.c_str());
    return ret == 0 && fs::exists(output_path);
}

bool PDFTool::split_pdf_with_pypdf(
    const std::string& input_path,
    const std::string& output_dir,
    int pages_per_file
) {
    std::string script =
        "from pypdf import PdfReader, PdfWriter; import os; "
        "r = PdfReader('" + input_path + "'); "
        "n = len(r.pages); "
        "ppf = " + std::to_string(pages_per_file) + "; "
        "for i in range(0, n, ppf): "
        "  w = PdfWriter(); "
        "  [w.add_page(r.pages[j]) for j in range(i, min(i+ppf, n))]; "
        "  w.write(os.path.join('" + output_dir + "', f'part_{i//ppf+1:04d}.pdf'))";

    std::string cmd = "python3 -c \"" + script + "\" 2>/dev/null";
    int ret = std::system(cmd.c_str());
    return ret == 0;
}

// ============================================================================
// Page range parsing
// ============================================================================

std::vector<int> PDFTool::parse_page_range(
    const std::string& range_str,
    int total_pages
) {
    // 🎯 对齐 OpenClaw: page range syntax: "1-5", "1,3,5-10", "all"
    std::vector<int> pages;
    if (range_str.empty() || range_str == "all") {
        int limit = std::min(total_pages, config_.max_pages);
        for (int i = 1; i <= limit; ++i) pages.push_back(i);
        return pages;
    }

    std::istringstream ss(range_str);
    std::string token;
    while (std::getline(ss, token, ',')) {
        token = trim(token);
        if (token.empty()) continue;
        auto dash = token.find('-');
        if (dash != std::string::npos) {
            int start = std::stoi(token.substr(0, dash));
            int end   = std::stoi(token.substr(dash + 1));
            for (int i = start; i <= end && i <= total_pages; ++i)
                pages.push_back(i);
        } else {
            int p = std::stoi(token);
            if (p >= 1 && p <= total_pages) pages.push_back(p);
        }
    }

    // Deduplicate and sort
    std::sort(pages.begin(), pages.end());
    pages.erase(std::unique(pages.begin(), pages.end()), pages.end());

    // Limit to max_pages
    if ((int)pages.size() > config_.max_pages)
        pages.resize(config_.max_pages);

    return pages;
}

// ============================================================================
// Path validation
// ============================================================================

bool PDFTool::validate_path(const std::string& path) {
    if (path.empty()) return false;
    // Prevent directory traversal
    if (path.find("..") != std::string::npos) return false;
    // Check allowed directory
    if (!config_.allowed_dir.empty()) {
        std::string abs = resolve_absolute_path(path);
        if (abs.substr(0, config_.allowed_dir.size()) != config_.allowed_dir) return false;
    }
    return true;
}

bool PDFTool::check_file_size(const std::string& path) {
    try {
        auto size = fs::file_size(path);
        return (int)(size / (1024 * 1024)) <= config_.max_file_size_mb;
    } catch (...) {
        return false;
    }
}

std::string PDFTool::resolve_absolute_path(const std::string& path) {
    try {
        return fs::absolute(path).string();
    } catch (...) {
        return path;
    }
}

// ============================================================================
// Cache
// ============================================================================

namespace {
struct PdfCacheEntry {
    PdfExtractionResult result;
    std::chrono::system_clock::time_point expires_at;
};
static std::unordered_map<std::string, PdfCacheEntry> g_pdf_cache;
}

std::optional<PdfExtractionResult> PDFTool::get_from_cache(
    const std::string& path,
    const std::string& page_range_key
) {
    std::string key = compute_cache_key(path, page_range_key);
    auto it = g_pdf_cache.find(key);
    if (it == g_pdf_cache.end()) return std::nullopt;
    if (std::chrono::system_clock::now() > it->second.expires_at) {
        g_pdf_cache.erase(it);
        return std::nullopt;
    }
    return it->second.result;
}

void PDFTool::save_to_cache(
    const std::string& path,
    const std::string& page_range_key,
    const PdfExtractionResult& result
) {
    std::string key = compute_cache_key(path, page_range_key);
    PdfCacheEntry entry;
    entry.result = result;
    entry.expires_at = std::chrono::system_clock::now() + std::chrono::minutes(30);
    g_pdf_cache[key] = entry;
}

std::string PDFTool::compute_cache_key(
    const std::string& path,
    const std::string& page_range
) {
    return path + "|" + page_range;
}

// ============================================================================
// Factory
// ============================================================================

std::unique_ptr<PDFTool> create_pdf_tool(const PDFTool::Config& config) {
    return std::make_unique<PDFTool>(config);
}

} // namespace tools
} // namespace hunterclaw
