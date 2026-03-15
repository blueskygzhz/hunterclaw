#include "hunterclaw/memory/http_client.h"
#include <curl/curl.h>
#include <sstream>
#include <cstring>

namespace hunterclaw {
namespace memory {

// ============================================================================
// HTTP Client Implementation (using libcurl)
// ============================================================================

size_t HttpClient::write_callback(void* contents, size_t size, size_t nmemb, void* userp) {
    size_t total_size = size * nmemb;
    auto* str = static_cast<std::string*>(userp);
    str->append(static_cast<char*>(contents), total_size);
    return total_size;
}

size_t HttpClient::header_callback(char* buffer, size_t size, size_t nitems, void* userdata) {
    size_t total_size = size * nitems;
    auto* headers = static_cast<std::map<std::string, std::string>*>(userdata);
    
    std::string header_line(buffer, total_size);
    
    // Parse "Key: Value\r\n"
    size_t colon_pos = header_line.find(':');
    if (colon_pos != std::string::npos) {
        std::string key = header_line.substr(0, colon_pos);
        std::string value = header_line.substr(colon_pos + 1);
        
        // Trim whitespace
        value.erase(0, value.find_first_not_of(" \t\r\n"));
        value.erase(value.find_last_not_of(" \t\r\n") + 1);
        
        (*headers)[key] = value;
    }
    
    return total_size;
}

core::Result<HttpResponse> HttpClient::request(const HttpRequest& req) {
    CURL* curl = curl_easy_init();
    if (!curl) {
        return core::Result<HttpResponse>::Err("Failed to initialize CURL");
    }
    
    HttpResponse response;
    std::string response_body;
    
    // Set URL
    curl_easy_setopt(curl, CURLOPT_URL, req.url.c_str());
    
    // Set method
    if (req.method == "POST") {
        curl_easy_setopt(curl, CURLOPT_POST, 1L);
        if (!req.body.empty()) {
            curl_easy_setopt(curl, CURLOPT_POSTFIELDS, req.body.c_str());
            curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, req.body.size());
        }
    } else if (req.method == "GET") {
        curl_easy_setopt(curl, CURLOPT_HTTPGET, 1L);
    }
    
    // Set headers
    struct curl_slist* headers = nullptr;
    for (const auto& [key, value] : req.headers) {
        std::string header = key + ": " + value;
        headers = curl_slist_append(headers, header.c_str());
    }
    if (headers) {
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    }
    
    // Set callbacks
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_callback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response_body);
    curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, header_callback);
    curl_easy_setopt(curl, CURLOPT_HEADERDATA, &response.headers);
    
    // Set timeout
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, req.timeout_ms);
    
    // Follow redirects
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    
    // Perform request
    CURLcode res = curl_easy_perform(curl);
    
    if (res != CURLE_OK) {
        std::string error_msg = curl_easy_strerror(res);
        curl_slist_free_all(headers);
        curl_easy_cleanup(curl);
        return core::Result<HttpResponse>::Err("CURL error: " + error_msg);
    }
    
    // Get status code
    long status_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status_code);
    response.status_code = static_cast<int>(status_code);
    response.body = response_body;
    
    // Cleanup
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    
    return core::Result<HttpResponse>::Ok(response);
}

core::Result<nlohmann::json> HttpClient::post_json(
    const std::string& url,
    const nlohmann::json& body,
    const std::map<std::string, std::string>& headers,
    int timeout_ms
) {
    HttpRequest req;
    req.url = url;
    req.method = "POST";
    req.headers = headers;
    req.headers["Content-Type"] = "application/json";
    req.body = body.dump();
    req.timeout_ms = timeout_ms;
    
    auto result = request(req);
    if (!result.is_ok()) {
        return core::Result<nlohmann::json>::Err(result.error);
    }
    
    const auto& resp = result.value;
    if (!resp.is_success()) {
        std::stringstream ss;
        ss << "HTTP error " << resp.status_code << ": " << resp.body;
        return core::Result<nlohmann::json>::Err(ss.str());
    }
    
    try {
        nlohmann::json json_response = nlohmann::json::parse(resp.body);
        return core::Result<nlohmann::json>::Ok(json_response);
    } catch (const std::exception& e) {
        return core::Result<nlohmann::json>::Err(
            std::string("JSON parse error: ") + e.what()
        );
    }
}

core::Result<HttpResponse> HttpClient::get(
    const std::string& url,
    const std::map<std::string, std::string>& headers,
    int timeout_ms
) {
    HttpRequest req;
    req.url = url;
    req.method = "GET";
    req.headers = headers;
    req.timeout_ms = timeout_ms;
    
    return request(req);
}

}  // namespace memory
}  // namespace hunterclaw
