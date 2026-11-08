#include "db_config.h"

#include <cctype>
#include <fstream>
#include <stdexcept>

namespace kc {

namespace {

// 去掉首尾空白（空格、制表符、\r 等）。\r 主要针对 Windows 下编辑的文件。
std::string trim(const std::string& s) {
    std::size_t b = 0;
    std::size_t e = s.size();
    while (b < e && std::isspace(static_cast<unsigned char>(s[b]))) ++b;
    while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) --e;
    return s.substr(b, e - b);
}

// 全小写，用于比较 section / key。
std::string lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

} // namespace

DbConfig DbConfig::load(const std::string& path) {
    std::ifstream in(path);
    if (!in) throw std::runtime_error("cannot open DB config: " + path);

    DbConfig cfg;
    std::string line;
    bool in_db = false;

    while (std::getline(in, line)) {
        line = trim(line);
        if (line.empty()) continue;
        if (line[0] == '#' || line[0] == ';') continue;   // 行首注释

        // [section]
        if (line.front() == '[' && line.back() == ']') {
            std::string sec = lower(trim(line.substr(1, line.size() - 2)));
            in_db = (sec == "db");
            continue;
        }
        if (!in_db) continue;

        // key=value，只按第一个 '=' 切分（密码里可能有特殊字符）
        auto eq = line.find('=');
        if (eq == std::string::npos) continue;

        std::string key = lower(trim(line.substr(0, eq)));
        std::string val = trim(line.substr(eq + 1));

        if      (key == "host")     cfg.host     = val;
        else if (key == "port")     cfg.port     = static_cast<unsigned short>(std::stoi(val));
        else if (key == "dbname")   cfg.dbname   = val;
        else if (key == "user")     cfg.user     = val;
        else if (key == "password") cfg.password = val;
        // 未知键忽略，保持向后兼容
    }

    if (cfg.dbname.empty())   throw std::runtime_error("DB config missing dbname");
    if (cfg.user.empty())     throw std::runtime_error("DB config missing user");
    if (cfg.port == 0)        throw std::runtime_error("DB config invalid port");
    return cfg;
}

} // namespace kc
