#pragma once

#include <string>
#include <map>
#include <vector>
#include <memory>
#include <variant>

namespace nlohmann {

// Simple JSON implementation to avoid external dependency
class json {
public:
    using value_type = std::variant<
        std::nullptr_t,
        bool,
        int64_t,
        double,
        std::string,
        std::vector<json>,
        std::map<std::string, json>
    >;
    
    json() : value_(nullptr) {}
    json(std::nullptr_t) : value_(nullptr) {}
    json(bool v) : value_(v) {}
    json(int v) : value_(static_cast<int64_t>(v)) {}
    json(int64_t v) : value_(v) {}
    json(double v) : value_(v) {}
    json(const char* v) : value_(std::string(v)) {}
    json(const std::string& v) : value_(v) {}
    json(std::initializer_list<std::pair<std::string, json>> init) {
        std::map<std::string, json> obj;
        for (const auto& [k, v] : init) {
            obj[k] = v;
        }
        value_ = obj;
    }
    
    bool is_null() const {
        return std::holds_alternative<std::nullptr_t>(value_);
    }
    
    bool is_boolean() const {
        return std::holds_alternative<bool>(value_);
    }
    
    bool is_number() const {
        return std::holds_alternative<int64_t>(value_) || 
               std::holds_alternative<double>(value_);
    }
    
    bool is_string() const {
        return std::holds_alternative<std::string>(value_);
    }
    
    bool is_array() const {
        return std::holds_alternative<std::vector<json>>(value_);
    }
    
    bool is_object() const {
        return std::holds_alternative<std::map<std::string, json>>(value_);
    }
    
    bool contains(const std::string& key) const {
        if (auto* obj = std::get_if<std::map<std::string, json>>(&value_)) {
            return obj->find(key) != obj->end();
        }
        return false;
    }
    
    json& operator[](const std::string& key) {
        if (!is_object()) {
            value_ = std::map<std::string, json>();
        }
        return std::get<std::map<std::string, json>>(value_)[key];
    }
    
    const json& operator[](const std::string& key) const {
        static json null_value;
        if (auto* obj = std::get_if<std::map<std::string, json>>(&value_)) {
            auto it = obj->find(key);
            if (it != obj->end()) {
                return it->second;
            }
        }
        return null_value;
    }
    
    template<typename T>
    T get() const {
        if constexpr (std::is_same_v<T, bool>) {
            if (auto* v = std::get_if<bool>(&value_)) return *v;
        } else if constexpr (std::is_same_v<T, int>) {
            if (auto* v = std::get_if<int64_t>(&value_)) return static_cast<int>(*v);
        } else if constexpr (std::is_same_v<T, int64_t>) {
            if (auto* v = std::get_if<int64_t>(&value_)) return *v;
        } else if constexpr (std::is_same_v<T, double>) {
            if (auto* v = std::get_if<double>(&value_)) return *v;
        } else if constexpr (std::is_same_v<T, std::string>) {
            if (auto* v = std::get_if<std::string>(&value_)) return *v;
        }
        return T{};
    }
    
    std::string dump(int indent = -1) const {
        // Simple serialization
        if (is_null()) return "null";
        if (auto* v = std::get_if<bool>(&value_)) return *v ? "true" : "false";
        if (auto* v = std::get_if<int64_t>(&value_)) return std::to_string(*v);
        if (auto* v = std::get_if<double>(&value_)) return std::to_string(*v);
        if (auto* v = std::get_if<std::string>(&value_)) return "\"" + *v + "\"";
        if (auto* v = std::get_if<std::vector<json>>(&value_)) {
            std::string result = "[";
            for (size_t i = 0; i < v->size(); ++i) {
                if (i > 0) result += ",";
                result += (*v)[i].dump(indent);
            }
            result += "]";
            return result;
        }
        if (auto* v = std::get_if<std::map<std::string, json>>(&value_)) {
            std::string result = "{";
            bool first = true;
            for (const auto& [k, val] : *v) {
                if (!first) result += ",";
                result += "\"" + k + "\":" + val.dump(indent);
                first = false;
            }
            result += "}";
            return result;
        }
        return "{}";
    }
    
    static json parse(const std::string& str) {
        // Simple parser - just return empty object for now
        return json();
    }
    
private:
    value_type value_;
};

} // namespace nlohmann
