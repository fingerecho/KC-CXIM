#include "jwt_config.h"   // 引入本模块的头文件（结构体定义与 load 声明）

#include "rapidjson/document.h"   // RapidJSON DOM 解析器：把 JSON 文本解析成文档树

#include <fstream>     // std::ifstream：读取磁盘上的 JSON 配置文件
#include <sstream>     // std::ostringstream：把整个文件内容快速读成字符串
#include <stdexcept>   // std::runtime_error：配置非法时抛出异常

namespace kc {

// ---------------------------------------------------------------------------
// load：读取 JWTConfig.json，校验必填字段并填充 JwtConfig。
// ---------------------------------------------------------------------------
JwtConfig JwtConfig::load(const std::string& path) {
    std::ifstream in(path, std::ios::binary);                 // 以二进制方式打开，避免换行被转换
    if (!in) throw std::runtime_error("cannot open jwt config: " + path);   // 打不开直接报错，让服务启动失败

    std::ostringstream ss;                                    // 用于承接文件全部内容
    ss << in.rdbuf();                                         // 一次性把文件流写入字符串缓冲
    const std::string text = ss.str();                        // 得到完整 JSON 文本

    rapidjson::Document doc;                                  // RapidJSON 文档对象（解析结果）
    doc.Parse(text.data(), text.size());                      // 必须带长度解析，避免字符串无 '\0' 时越界
    if (doc.HasParseError()) throw std::runtime_error("jwt config is not valid JSON");   // JSON 语法错误
    if (!doc.IsObject())     throw std::runtime_error("jwt config root must be object"); // 根节点必须是对象

    JwtConfig cfg;                                            // 待返回的配置对象

    // 读取 jwt_secret：必填且必须是非空字符串。
    if (!doc.HasMember("jwt_secret") || !doc["jwt_secret"].IsString())   // 字段缺失或类型不对
        throw std::runtime_error("jwt config missing jwt_secret");        // 直接失败，避免用空密钥签发
    cfg.secret = doc["jwt_secret"].GetString();               // 取出密钥字符串

    // 读取 jwt_issuer：必填且必须是非空字符串。
    if (!doc.HasMember("jwt_issuer") || !doc["jwt_issuer"].IsString())   // 字段缺失或类型不对
        throw std::runtime_error("jwt config missing jwt_issuer");        // 同样直接失败
    cfg.issuer = doc["jwt_issuer"].GetString();               // 取出签发者字符串

    if (cfg.secret.empty()) throw std::runtime_error("jwt_secret must not be empty");   // 空密钥没有安全性
    if (cfg.issuer.empty()) throw std::runtime_error("jwt_issuer must not be empty");   // 空签发者无法校验

    // 读取可选的 expire_seconds：存在且为正整数时覆盖默认值。
    if (doc.HasMember("expire_seconds") && doc["expire_seconds"].IsInt64()) {   // 只有合法整数才采用
        const long long v = doc["expire_seconds"].GetInt64();                   // 取出秒数
        if (v > 0) cfg.expire_seconds = v;                                      // 非法/非正数保持默认
    }

    return cfg;   // 返回加载完成的配置
}

} // namespace kc
