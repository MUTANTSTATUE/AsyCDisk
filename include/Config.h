#pragma once
#include <nlohmann/json.hpp>
#include <string>
#include <mutex>
#include <vector>

class Config {
public:
    static Config& GetInstance();

    // 加载配置文件
    bool Load(const std::string& path);

    // 获取配置项（支持层级路径，如 "server/port"）
    template<typename T>
    T Get(const std::string& path, T defaultValue) {
        std::lock_guard<std::mutex> lock(mutex_);
        try {
            auto keys = SplitPath(path);
            nlohmann::json curr = data_;
            for (const auto& key : keys) {
                if (!curr.is_object() || !curr.contains(key)) return defaultValue;
                curr = curr[key];
            }
            return curr.get<T>();
        } catch (...) {
            return defaultValue;
        }
    }

    // 获取整个 JSON 对象（用于高级扩展）
    nlohmann::json GetRaw() {
        std::lock_guard<std::mutex> lock(mutex_);
        return data_;
    }

private:
    Config() = default;
    std::vector<std::string> SplitPath(const std::string& path);

    nlohmann::json data_;
    std::mutex mutex_;
};
