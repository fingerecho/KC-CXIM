#pragma once
#include <string>   // 使用 std::string 保存密钥与签发者

// ============================================================================
// JwtConfig：从项目根目录的 JWTConfig.json 读取 HS256 签名配置
// ----------------------------------------------------------------------------
// 为什么单独做成配置而不是写死在代码里？
//   1) 密钥属于敏感信息，写死在源码里会随代码仓库泄漏；
//   2) 未来即便把 HTTP 登录进程和 TCP 长连接进程拆开部署，只要两边读到
//      同一份 JSON（同一个 jwt_secret），就能各自独立验签，无需进程间通信；
//   3) issuer 参与校验，可避免别的系统用同一密钥签发的 token 混进来。
//
// JSON 字段（与根目录已有的 JWTConfig.json 保持一致）：
//   jwt_secret      : HS256 的共享密钥（必填，建议 64 位十六进制随机串）
//   jwt_issuer      : 签发者标识（必填）
//   expire_seconds  : token 有效期秒数（可选，缺省 86400 = 24 小时）
// ============================================================================

namespace kc {

struct JwtConfig {
    std::string secret;                      // HS256 共享密钥；签发与验签都只用它
    std::string issuer;                      // 签发者；验签时用 with_issuer 校验
    long long   expire_seconds = 86400;      // token 有效期（秒），默认 24 小时

    // 读取并校验配置文件；失败抛 std::runtime_error（main 统一捕获）。
    // 注意：这是一个"启动时一次性加载"的操作，不属于请求热路径，可以同步读文件。
    static JwtConfig load(const std::string& path);
};

} // namespace kc
