/**
 * Simple Standalone ReAct Example
 * Demonstrates ReAct pattern without full HunterClaw dependencies
 */

#include <iostream>
#include <string>
#include <vector>
#include <map>
#include <functional>
#include <sstream>
#include <regex>

// Simplified types for standalone compilation
struct ToolCall {
    std::string tool_name;
    std::string input;
};

struct ReActStep {
    std::string thought;
    ToolCall action;
    std::string observation;
};

class SimpleLLM {
public:
    std::string call(const std::string& prompt) {
        // Stub implementation - in real world this would call OpenAI/Anthropic API
        std::cout << "\n[LLM Call]\n" << prompt << "\n";
        return "Thought: I should use the search tool to find information.\n"
               "Action: search\n"
               "Action Input: example query";
    }
};

class SimpleToolManager {
private:
    std::map<std::string, std::function<std::string(const std::string&)>> tools_;
    
public:
    void register_tool(const std::string& name, 
                      std::function<std::string(const std::string&)> func) {
        tools_[name] = func;
    }
    
    std::string execute(const std::string& tool_name, const std::string& input) {
        auto it = tools_.find(tool_name);
        if (it == tools_.end()) {
            return "Error: Tool not found: " + tool_name;
        }
        return it->second(input);
    }
    
    std::vector<std::string> list_tools() {
        std::vector<std::string> names;
        for (const auto& pair : tools_) {
            names.push_back(pair.first);
        }
        return names;
    }
};

class SimpleReActExecutor {
private:
    SimpleLLM& llm_;
    SimpleToolManager& tools_;
    int max_iterations_;
    
public:
    SimpleReActExecutor(SimpleLLM& llm, SimpleToolManager& tools, int max_iter = 10)
        : llm_(llm), tools_(tools), max_iterations_(max_iter) {}
    
    std::pair<ToolCall, bool> parse_action(const std::string& llm_response) {
        // Parse: Action: <tool>\nAction Input: <input>
        std::regex action_regex(R"(Action\s*:\s*(.*?)\s*Action\s*Input\s*:\s*([\s\S]*))");
        std::smatch matches;
        
        if (std::regex_search(llm_response, matches, action_regex)) {
            std::string tool = matches[1].str();
            std::string input = matches[2].str();
            
            // Trim whitespace
            tool.erase(0, tool.find_first_not_of(" \t\n\r"));
            tool.erase(tool.find_last_not_of(" \t\n\r") + 1);
            input.erase(0, input.find_first_not_of(" \t\n\r"));
            input.erase(input.find_last_not_of(" \t\n\r") + 1);
            
            return {{tool, input}, true};
        }
        
        // Check for finish
        if (llm_response.find("Final Answer:") != std::string::npos) {
            return {{"", ""}, false};
        }
        
        return {{"", ""}, true};
    }
    
    std::string run(const std::string& task, const std::string& context = "") {
        std::cout << "\n=== ReAct Executor Started ===\n";
        std::cout << "Task: " << task << "\n";
        
        std::string scratchpad = "Question: " + task + "\n\n";
        if (!context.empty()) {
            scratchpad += "Context:\n" + context + "\n\n";
        }
        
        // Add tool descriptions
        scratchpad += "You have access to the following tools:\n";
        for (const auto& tool_name : tools_.list_tools()) {
            scratchpad += "- " + tool_name + "\n";
        }
        scratchpad += "\n";
        
        scratchpad += "Use the following format:\n"
                     "Thought: [your reasoning]\n"
                     "Action: [tool name]\n"
                     "Action Input: [input to the tool]\n"
                     "Observation: [result from tool]\n"
                     "... (repeat Thought/Action/Observation as needed)\n"
                     "Thought: I now know the final answer\n"
                     "Final Answer: [your answer]\n\n";
        
        for (int iteration = 0; iteration < max_iterations_; ++iteration) {
            std::cout << "\n--- Iteration " << (iteration + 1) << " ---\n";
            
            // Call LLM
            std::string llm_response = llm_.call(scratchpad);
            scratchpad += llm_response + "\n";
            
            // Parse action
            auto [action, should_continue] = parse_action(llm_response);
            
            if (!should_continue) {
                // Extract final answer
                size_t pos = llm_response.find("Final Answer:");
                if (pos != std::string::npos) {
                    std::string answer = llm_response.substr(pos + 13);
                    answer.erase(0, answer.find_first_not_of(" \t\n\r"));
                    std::cout << "\n=== Final Answer ===\n" << answer << "\n";
                    return answer;
                }
                break;
            }
            
            // Execute tool
            std::cout << "[Tool] " << action.tool_name << "(" << action.input << ")\n";
            std::string observation = tools_.execute(action.tool_name, action.input);
            std::cout << "[Observation] " << observation << "\n";
            
            scratchpad += "Observation: " + observation + "\n";
        }
        
        std::cout << "\n=== Max iterations reached ===\n";
        return "Failed to complete task within " + std::to_string(max_iterations_) + " iterations";
    }
};

// Example tools
std::string search_tool(const std::string& query) {
    return "Search results for '" + query + "': [Mock search result]";
}

std::string calculator_tool(const std::string& expression) {
    return "Result of '" + expression + "': 42 (mock)";
}

std::string filesystem_tool(const std::string& path) {
    return "File contents of '" + path + "': [Mock file content]";
}

int main() {
    std::cout << "=================================\n";
    std::cout << "Simple ReAct Pattern Demo\n";
    std::cout << "=================================\n";
    
    // Setup
    SimpleLLM llm;
    SimpleToolManager tools;
    
    // Register tools
    tools.register_tool("search", search_tool);
    tools.register_tool("calculator", calculator_tool);
    tools.register_tool("read_file", filesystem_tool);
    
    // Create ReAct executor
    SimpleReActExecutor executor(llm, tools, 5);
    
    // Run example task
    std::string task = "Find information about C++ compilation and calculate the number of source files";
    std::string result = executor.run(task);
    
    std::cout << "\n=== Execution Complete ===\n";
    std::cout << "Result: " << result << "\n";
    
    return 0;
}
