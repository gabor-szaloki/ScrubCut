#pragma once

#include <string>
#include <vector>
#include <algorithm>

class CommandLine {
public:
    static CommandLine& Get() {
        static CommandLine instance;
        return instance;
    }

    void Init(int argc, char* argv[]) {
        m_args.assign(argv, argv + argc);
    }

    bool HasFlag(const std::string& flag) const {
        return std::find(m_args.begin(), m_args.end(), flag) != m_args.end();
    }

    std::string GetValue(const std::string& key, const std::string& defaultValue = "") const {
        auto it = std::find(m_args.begin(), m_args.end(), key);
        if (it != m_args.end() && ++it != m_args.end())
            return *it;
        return defaultValue;
    }

    // Returns the first non-flag argument (i.e., a file path passed on the command line)
    std::string GetFileArg() const {
        for (size_t i = 1; i < m_args.size(); ++i) {
            if (!m_args[i].empty() && m_args[i][0] != '-')
                return m_args[i];
        }
        return "";
    }

private:
    CommandLine() = default;
    std::vector<std::string> m_args;
};
