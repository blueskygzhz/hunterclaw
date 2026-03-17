#include "hunterclaw/tools/edge_tts_provider.h"
#include <sstream>
#include <cstdlib>
#include <cstdio>
#include <memory>
#include <array>
#include <fstream>

namespace hunterclaw {
namespace tools {

EdgeTTSProvider::EdgeTTSProvider(std::shared_ptr<infra::HttpClient> http_client)
    : http_client_(http_client) {
    // Check if edge-tts is installed
    if (!is_edge_tts_installed()) {
        // Try to install it
        install_edge_tts();
    }
}

core::JsonValue EdgeTTSProvider::synthesize(
    const std::string& text,
    const std::string& voice,
    const std::string& format,
    double rate,
    double pitch,
    double volume
) {
    // Validate parameters
    if (text.empty()) {
        core::JsonValue error = core::JsonValue::object();
        error["error"] = "Text cannot be empty";
        return error;
    }
    
    // Check if edge-tts is installed
    if (!is_edge_tts_installed()) {
        core::JsonValue error = core::JsonValue::object();
        error["error"] = "edge-tts not installed";
        error["hint"] = "Install with: pip install edge-tts";
        return error;
    }
    
    // Generate output filename
    std::string output_file = "/tmp/edge_tts_output." + format;
    
    // Build edge-tts command
    std::vector<std::string> args = {
        "edge-tts",
        "--voice", voice,
        "--text", text,
        "--write-media", output_file
    };
    
    // Add rate
    if (rate != 1.0) {
        std::stringstream rate_str;
        int rate_percent = static_cast<int>((rate - 1.0) * 100);
        if (rate_percent > 0) {
            rate_str << "+" << rate_percent << "%";
        } else {
            rate_str << rate_percent << "%";
        }
        args.push_back("--rate");
        args.push_back(rate_str.str());
    }
    
    // Add pitch
    if (pitch != 0.0) {
        std::stringstream pitch_str;
        int pitch_hz = static_cast<int>(pitch);
        if (pitch_hz > 0) {
            pitch_str << "+" << pitch_hz << "Hz";
        } else {
            pitch_str << pitch_hz << "Hz";
        }
        args.push_back("--pitch");
        args.push_back(pitch_str.str());
    }
    
    // Add volume
    if (volume != 1.0) {
        std::stringstream volume_str;
        int volume_percent = static_cast<int>((volume - 1.0) * 100);
        if (volume_percent > 0) {
            volume_str << "+" << volume_percent << "%";
        } else {
            volume_str << volume_percent << "%";
        }
        args.push_back("--volume");
        args.push_back(volume_str.str());
    }
    
    // Execute edge-tts
    std::string output = call_edge_tts_cli(args);
    
    // Check if output file exists
    std::ifstream file(output_file, std::ios::binary);
    if (!file.good()) {
        core::JsonValue error = core::JsonValue::object();
        error["error"] = "Failed to generate audio";
        error["details"] = output;
        return error;
    }
    
    // Read file to base64
    file.seekg(0, std::ios::end);
    size_t file_size = file.tellg();
    file.seekg(0, std::ios::beg);
    
    std::vector<char> buffer(file_size);
    file.read(buffer.data(), file_size);
    file.close();
    
    // Convert to base64 (simplified, use proper base64 encoding in production)
    std::string audio_base64;  // TODO: Implement base64 encoding
    
    // Build result
    core::JsonValue result = core::JsonValue::object();
    result["status"] = "success";
    result["provider"] = "edge";
    result["voice"] = voice;
    result["format"] = format;
    result["audio_path"] = output_file;
    result["file_size"] = static_cast<int64_t>(file_size);
    result["duration_estimate"] = static_cast<int64_t>(text.length() / 15);  // Rough estimate
    
    if (!audio_base64.empty()) {
        result["audio_base64"] = audio_base64;
    }
    
    return result;
}

std::vector<std::string> EdgeTTSProvider::list_voices(const std::string& language) {
    std::vector<std::string> voices;
    
    // Return popular voices if language filter is provided
    if (!language.empty()) {
        for (const auto& voice : POPULAR_EDGE_VOICES) {
            if (voice.language.find(language) == 0) {
                voices.push_back(voice.name);
            }
        }
    } else {
        // Return all popular voices
        for (const auto& voice : POPULAR_EDGE_VOICES) {
            voices.push_back(voice.name);
        }
    }
    
    return voices;
}

core::JsonValue EdgeTTSProvider::get_voice_info(const std::string& voice_name) {
    core::JsonValue info = core::JsonValue::object();
    
    // Find voice in popular list
    for (const auto& voice : POPULAR_EDGE_VOICES) {
        if (voice.name == voice_name) {
            info["name"] = voice.name;
            info["language"] = voice.language;
            info["gender"] = voice.gender;
            info["description"] = voice.description;
            info["provider"] = "edge";
            info["cost"] = "free";
            return info;
        }
    }
    
    // Not found
    info["error"] = "Voice not found: " + voice_name;
    return info;
}

std::string EdgeTTSProvider::build_ssml(
    const std::string& text,
    const std::string& voice,
    double rate,
    double pitch,
    double volume
) {
    std::stringstream ssml;
    ssml << "<speak version='1.0' xmlns='http://www.w3.org/2001/10/synthesis' xml:lang='en-US'>";
    ssml << "<voice name='" << voice << "'>";
    
    // Add prosody tags if needed
    bool need_prosody = (rate != 1.0 || pitch != 0.0 || volume != 1.0);
    if (need_prosody) {
        ssml << "<prosody";
        if (rate != 1.0) {
            ssml << " rate='" << (rate * 100) << "%'";
        }
        if (pitch != 0.0) {
            ssml << " pitch='" << (pitch > 0 ? "+" : "") << pitch << "Hz'";
        }
        if (volume != 1.0) {
            ssml << " volume='" << (volume * 100) << "%'";
        }
        ssml << ">";
    }
    
    ssml << text;
    
    if (need_prosody) {
        ssml << "</prosody>";
    }
    
    ssml << "</voice></speak>";
    return ssml.str();
}

std::string EdgeTTSProvider::call_edge_tts_cli(const std::vector<std::string>& args) {
    // Build command
    std::stringstream cmd;
    for (size_t i = 0; i < args.size(); ++i) {
        if (i > 0) cmd << " ";
        
        // Quote arguments with spaces
        if (args[i].find(' ') != std::string::npos) {
            cmd << "\"" << args[i] << "\"";
        } else {
            cmd << args[i];
        }
    }
    cmd << " 2>&1";  // Capture stderr
    
    // Execute command
    std::array<char, 128> buffer;
    std::string result;
    
    FILE* pipe = popen(cmd.str().c_str(), "r");
    if (!pipe) {
        return "Error: Failed to execute edge-tts";
    }
    
    while (fgets(buffer.data(), buffer.size(), pipe) != nullptr) {
        result += buffer.data();
    }
    
    int return_code = pclose(pipe);
    
    if (return_code != 0) {
        return "Error (code " + std::to_string(return_code) + "): " + result;
    }
    
    return result;
}

bool EdgeTTSProvider::is_edge_tts_installed() {
    // Try to run edge-tts --version
    FILE* pipe = popen("edge-tts --version 2>&1", "r");
    if (!pipe) {
        return false;
    }
    
    std::array<char, 128> buffer;
    std::string result;
    while (fgets(buffer.data(), buffer.size(), pipe) != nullptr) {
        result += buffer.data();
    }
    
    int return_code = pclose(pipe);
    return (return_code == 0 && result.find("edge-tts") != std::string::npos);
}

std::string EdgeTTSProvider::install_edge_tts() {
    // Try to install via pip
    FILE* pipe = popen("pip install edge-tts 2>&1", "r");
    if (!pipe) {
        return "Error: Failed to run pip";
    }
    
    std::array<char, 128> buffer;
    std::string result;
    while (fgets(buffer.data(), buffer.size(), pipe) != nullptr) {
        result += buffer.data();
    }
    
    pclose(pipe);
    return result;
}

} // namespace tools
} // namespace hunterclaw
