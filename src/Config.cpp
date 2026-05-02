#include "Config.h"
#include <fstream>
#include <sstream>
#include "Logger.h"

Config& Config::GetInstance() {
    static Config instance;
    return instance;
}

bool Config::Load(const std::string& path) {
    std::lock_guard<std::mutex> lock(mutex_);
    std::ifstream f(path);
    if (!f.is_open()) {
        LOG_ERROR("Could not open config file: {}", path);
        return false;
    }

    try {
        f >> data_;
        LOG_INFO("Config loaded successfully from {}", path);
        return true;
    } catch (const std::exception& e) {
        LOG_ERROR("Error parsing config file: {}", e.what());
        return false;
    }
}

std::vector<std::string> Config::SplitPath(const std::string& path) {
    std::vector<std::string> keys;
    std::stringstream ss(path);
    std::string key;
    while (std::getline(ss, key, '/')) {
        if (!key.empty()) keys.push_back(key);
    }
    return keys;
}
