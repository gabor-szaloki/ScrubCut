#pragma once

#include <string>
#include <unordered_map>
#include <fstream>
#include <filesystem>

class Settings {
public:
    void Load(const std::filesystem::path& path) {
        m_path = path;
        m_data.clear();
        std::ifstream f(path);
        if (!f.is_open()) return;
        std::string line;
        while (std::getline(f, line)) {
            auto eq = line.find('=');
            if (eq == std::string::npos) continue;
            m_data[line.substr(0, eq)] = line.substr(eq + 1);
        }
    }

    void Save() const {
        std::ofstream f(m_path);
        if (!f.is_open()) return;
        for (auto& [k, v] : m_data)
            f << k << '=' << v << '\n';
    }

    int GetInt(const std::string& key, int def) const {
        auto it = m_data.find(key);
        if (it == m_data.end()) return def;
        try { return std::stoi(it->second); } catch (...) { return def; }
    }

    bool GetBool(const std::string& key, bool def) const {
        auto it = m_data.find(key);
        if (it == m_data.end()) return def;
        return it->second == "1";
    }

    float GetFloat(const std::string& key, float def) const {
        auto it = m_data.find(key);
        if (it == m_data.end()) return def;
        try { return std::stof(it->second); } catch (...) { return def; }
    }

    void SetInt(const std::string& key, int val) { m_data[key] = std::to_string(val); }
    void SetBool(const std::string& key, bool val) { m_data[key] = val ? "1" : "0"; }
    void SetFloat(const std::string& key, float val) { m_data[key] = std::to_string(val); }

private:
    std::filesystem::path m_path;
    std::unordered_map<std::string, std::string> m_data;
};
