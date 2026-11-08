#pragma once
#include <cstdint>
#include <string>

// ============================================================================
// DbConfig：从 DBConfig.ini 读取数据库连接信息
// ----------------------------------------------------------------------------
// 只解析 [db] 段落中的 host / port / dbname / user / password。
// 这里自己写一个极简 INI 解析器，避免额外引入第三方库。
// ============================================================================

namespace kc {

struct DbConfig {
    std::string    host     = "127.0.0.1";
    unsigned short port     = 3306;
    std::string    dbname;
    std::string    user;
    std::string    password;

    // 读取并校验配置文件；失败会抛出 std::runtime_error（main 里统一捕获）。
    static DbConfig load(const std::string& path);
};

} // namespace kc
