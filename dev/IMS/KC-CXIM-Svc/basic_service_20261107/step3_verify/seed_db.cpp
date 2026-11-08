// ============================================================================
// seed_db：用 MariaDB Connector/C++ 执行 SQL 文件（schema / 测试数据）
// ----------------------------------------------------------------------------
// 为什么需要它？
//   验证机（Windows/MSYS）上可能没有 mariadb/mysql 命令行客户端。
//   本工具复用项目已链接的 Connector，直接连远程库执行 SQL。
//
// 用法：
//   seed_db <DBConfig.ini> <file1.sql> [file2.sql ...]
//
// 特性：
//   - 连接时不指定 schema，因此即使目标库还不存在也能连上；
//   - 先尝试 USE 目标库，不存在再 CREATE DATABASE；
//   - 自动剥离 -- 行注释和 /* */ 块注释，并按引号外的 ';' 拆语句；
//   - 所有语句在同一连接上执行（SET FOREIGN_KEY_CHECKS 等会话变量才会生效）。
// ============================================================================

#include <mariadb/conncpp.hpp>

#include "db_config.h"

#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

std::string trim(const std::string& s) {
    std::size_t b = 0, e = s.size();
    auto ws = [](unsigned char c) {
        return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\f' || c == '\v';
    };
    while (b < e && ws(static_cast<unsigned char>(s[b]))) ++b;
    while (e > b && ws(static_cast<unsigned char>(s[e - 1]))) --e;
    return s.substr(b, e - b);
}

std::string read_file(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) throw std::runtime_error("cannot open " + path);
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

// 按引号外的 ';' 拆分语句，同时跳过 -- 行注释和 /* */ 块注释。
std::vector<std::string> split_statements(const std::string& text) {
    std::vector<std::string> out;
    std::string cur;
    char quote = 0;
    bool line_comment = false;
    bool block_comment = false;

    for (std::size_t i = 0; i < text.size(); ++i) {
        char c = text[i];
        char n = (i + 1 < text.size()) ? text[i + 1] : '\0';

        if (line_comment) {
            if (c == '\n') { line_comment = false; cur.push_back(c); }
            continue;
        }
        if (block_comment) {
            if (c == '*' && n == '/') { block_comment = false; ++i; }
            continue;
        }

        if (quote) {
            cur.push_back(c);
            if (c == quote) {
                // 处理 '' / "" / `` 这种成对引号
                if (n == quote) { cur.push_back(n); ++i; }
                else if (c != '`' && i > 0 && text[i - 1] == '\\') { /* 转义，仍在引号内 */ }
                else quote = 0;
            }
            continue;
        }

        if (c == '-' && n == '-') { line_comment = true; ++i; continue; }
        if (c == '/' && n == '*') { block_comment = true; ++i; continue; }
        if (c == '\'' || c == '"' || c == '`') { quote = c; cur.push_back(c); continue; }

        if (c == ';') {
            if (!trim(cur).empty()) out.push_back(cur);
            cur.clear();
            continue;
        }
        cur.push_back(c);
    }
    if (!trim(cur).empty()) out.push_back(cur);
    return out;
}

} // namespace

int main(int argc, char* argv[]) {
    if (argc < 3) {
        std::cerr << "用法: seed_db <DBConfig.ini> <file1.sql> [file2.sql ...]\n";
        return 2;
    }

    try {
        kc::DbConfig cfg = kc::DbConfig::load(argv[1]);
        if (cfg.dbname.find('`') != std::string::npos)
            throw std::runtime_error("dbname must not contain backtick");

        sql::Driver* driver = sql::mariadb::get_driver_instance();

        // 注意：这里不设置 schema，避免目标库尚未创建时连不上。
        sql::Properties props;
        props["hostName"]       = sql::SQLString(cfg.host);
        props["port"]           = sql::SQLString(std::to_string(cfg.port));
        props["userName"]       = sql::SQLString(cfg.user);
        props["password"]       = sql::SQLString(cfg.password);
        props["connectTimeout"] = sql::SQLString("10000");
        props["socketTimeout"]  = sql::SQLString("60000");

        std::unique_ptr<sql::Connection> conn(driver->connect(props));
        conn->setAutoCommit(true);
        std::unique_ptr<sql::Statement> st(conn->createStatement());

        // 目标库存在就直接 USE；不存在再尝试 CREATE。
        const std::string use_db = "USE `" + cfg.dbname + "`";
        try {
            st->execute(use_db);
        } catch (...) {
            std::cout << "[seed_db] database " << cfg.dbname
                      << " not found, creating...\n";
            st->execute("CREATE DATABASE IF NOT EXISTS `" + cfg.dbname +
                        "` DEFAULT CHARACTER SET utf8mb4 "
                        "DEFAULT COLLATE utf8mb4_unicode_ci");
            st->execute(use_db);
        }
        st->execute("SET NAMES utf8mb4");

        for (int f = 2; f < argc; ++f) {
            std::cout << "[seed_db] executing " << argv[f] << "\n";
            std::string text = read_file(argv[f]);
            auto stmts = split_statements(text);

            for (auto& s : stmts) {
                if (trim(s).empty()) continue;
                try {
                    st->execute(s);
                } catch (const std::exception& e) {
                    std::string preview = trim(s);
                    if (preview.size() > 200) preview = preview.substr(0, 200) + "...";
                    std::cerr << "[seed_db] statement failed: " << e.what() << "\n"
                              << "  SQL: " << preview << "\n";
                    return 1;
                }
            }
            // 每个文件执行前重新切回目标库，避免文件里 USE 到别处。
            st->execute(use_db);
        }

        std::cout << "[seed_db] done\n";
    } catch (const std::exception& e) {
        std::cerr << "[seed_db] error: " << e.what() << "\n";
        return 1;
    }
    return 0;
}
