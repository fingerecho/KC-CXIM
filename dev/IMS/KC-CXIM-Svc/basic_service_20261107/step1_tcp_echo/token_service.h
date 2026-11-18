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
//
// /*************P0-3：access / refresh 两类 token***************/
// 用 payload 里的自定义 claim "typ" 区分两类 token，密钥与校验路径完全相同：
//   typ=access   短有效期，用于 HTTP Bearer 与 TCP 登录验签；
//   typ=refresh  长有效期，只能拿去 POST /api/token/refresh 换新的 access token。
// 这样"断线重连"的成本从"一次 bcrypt 校验密码（几十~几百毫秒 CPU）"降为
// "一次 HMAC 验签（微秒）"，是重连风暴下登录延迟从十几秒回到毫秒级的关键。
//
// 两类 token 互不通用：verify() 明确拒绝 typ=refresh，verifyRefresh() 明确拒绝
// typ=access，避免长效 refresh token 被当成访问凭证直接使用。
// 兼容说明：本次改动之前签发的 token 没有 typ claim，verify() 把"缺 typ"按
// access 处理，保证升级过程中老客户端的在线会话不被打断。
// ============================================================================

namespace kc {

class TokenService {
public:
    explicit TokenService(const JwtConfig& cfg);   // 构造时保存配置（密钥/签发者/有效期）

    // 为登录成功的用户签发 access token。
    // userId 写进 JWT 的 sub，username 写进自定义 claim "username"。
    std::string sign(std::uint64_t userId, const std::string& username) const;

    // 签发 refresh token（P0-3）。同样只含 sub/username，靠 typ=refresh 区分用途。
    std::string signRefresh(std::uint64_t userId, const std::string& username) const;

    // 校验 access token 的签名、签发者、过期时间与 typ。
    // 成功时通过引用返回 userId / username，并返回 true；任何失败返回 false。
    bool verify(const std::string& token,
                std::uint64_t& userId,
                std::string& username) const;

    // 校验 refresh token（只接受 typ=refresh）。
    bool verifyRefresh(const std::string& token,
                       std::uint64_t& userId,
                       std::string& username) const;

    long long accessExpireSeconds()  const { return cfg_.expire_seconds; }
    long long refreshExpireSeconds() const { return cfg_.refresh_expire_seconds; }

private:
    // 共用的验签实现：expectedTyp 为 "access" / "refresh"。
    bool verifyAs(const std::string& token, const char* expectedTyp,
                  std::uint64_t& userId, std::string& username) const;

    JwtConfig cfg_;   // 只读配置副本，保证对象生命周期内密钥稳定
};

} // namespace kc
