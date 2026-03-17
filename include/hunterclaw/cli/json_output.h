#pragma once

#include <string>
#include <map>
#include <iostream>
#include <nlohmann/json.hpp>

namespace hunterclaw {
namespace cli {

/**
 * @brief JSON output helper for CLI commands
 * 
 * Provides structured JSON output for HunterClaw CLI commands:
 * {
 *   "success": true/false,
 *   "code": "success" | "error" | "not_found" | ...,
 *   "message": "...",
 *   "data": { ... }
 * }
 */
struct JsonOutput {
    bool success;
    std::string code;
    std::string message;
    nlohmann::json data = nlohmann::json::object();
    
    JsonOutput(bool s, const std::string& c, const std::string& msg)
        : success(s), code(c), message(msg) {}
    
    void print() const {
        nlohmann::json output;
        output["success"] = success;
        output["code"] = code;
        output["message"] = message;
        output["data"] = data;
        
        std::cout << output.dump(2) << std::endl;
    }
    
    void print_compact() const {
        nlohmann::json output;
        output["success"] = success;
        output["code"] = code;
        output["message"] = message;
        output["data"] = data;
        
        std::cout << output.dump() << std::endl;
    }
};

}  // namespace cli
}  // namespace hunterclaw
