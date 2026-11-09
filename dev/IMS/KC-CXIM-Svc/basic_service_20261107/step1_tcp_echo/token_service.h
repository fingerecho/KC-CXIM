#pragma once
#include <cstdint>       // std::uint64_t：用户ID类型，与协议保持一致
#include <string>        // std::string：token / username 的载体

#include "jwt_config.h"  // JwtConfig：持有密钥、签发者与有效期

// ============================================================================
// TokenService：无状态 JWT（HS256）的签发与校验
// ----------------------------------------------------------------------------
// 为什么"无状态"很重要？
//   token 本身自带用户身份（sub）与过期时间（exp），校验只需要共享密钥，
//   不依赖任何进程内 map / 缓存 / 共享内存。因此：
//     - 同一个进程里 HTTP 登录与 TCP 长连接可以直接复用同一份密钥；
//     - 将来即使把两者拆到不同进程/机器，只要读到同一份 JWTConfig.json，
//       任何进程都能独立验签，天然支持跨进程。
//
// 线程模型：
//   本类的 sign/verify 都是纯 CPU 的 HMAC-SHA256 运算（微秒级），
//   不会像 bcrypt 那样耗时，允许在 io_context 线程里直接调用。
// ============================================================================

namespace kc {

class TokenService {
public:
    explicit TokenService(const JwtConfig& cfg);   // 构造时保存配置（密钥/签发者/有效期）

    // 为登录成功的用户签发 token。
    // userId 写进 JWT 的 sub，username 写进自定义 claim "username"。
    std::string sign(std::uint64_t userId, const std::string& username) const;

    // 校验 token 的签名、签发者与过期时间。
    // 成功时通过引用返回 userId / username，并返回 true；任何失败返回 false。
    bool verify(const std::string& token,
                std::uint64_t& userId,
                std::string& username) const;

private:
    JwtConfig cfg_;   // 只读配置副本，保证对象生命周期内密钥稳定
};

} // namespace kc
