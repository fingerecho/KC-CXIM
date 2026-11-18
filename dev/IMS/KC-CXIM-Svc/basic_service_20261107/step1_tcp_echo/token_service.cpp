#include "token_service.h"        // 本模块声明的类

#include <jwt-cpp/jwt.h>          // header-only 的 JWT 库：负责编码/解码与 HS256 签名

#include <chrono>                 // std::chrono：生成 iat/exp 时间点
#include <cstdio>                 // std::snprintf：拼 jti 的十六进制串
#include <random>                 // jti 随机串：让同一秒内两次签发的 token 也不相同
#include <stdexcept>              // std::stoull 等可能抛出异常

namespace kc {

namespace {

// jti：16 字节随机值的十六进制串。
// 不参与安全校验，只用于区分"同一秒签发的两个 token"，便于日志排查，
// 也为将来接入撤销表（jti 黑名单）留好主键。
std::string randomJti() {
    static thread_local std::mt19937_64 rng{std::random_device{}()};
    char buf[33];
    for (int i = 0; i < 2; ++i) {
        std::snprintf(buf + i * 16, 17, "%016llx",
                      static_cast<unsigned long long>(rng()));
    }
    buf[32] = '\0';
    return std::string(buf, 32);
}

} // namespace

// ---------------------------------------------------------------------------
// 构造：拷贝一份配置，后续签发/验签都用这份不可变配置。
// ---------------------------------------------------------------------------
TokenService::TokenService(const JwtConfig& cfg) : cfg_(cfg) {}   // 拷贝构造，避免外部修改影响运行中的服务

// ---------------------------------------------------------------------------
// sign / signRefresh：签发 HS256 token。
// ----------------------------------------------------------------------------
// /*************P0-3：用 payload claim "typ" 区分两类 token***************/
//   typ=access   短有效期（expire_seconds），用于 HTTP Bearer 与 TCP 登录验签；
//   typ=refresh  长有效期（refresh_expire_seconds，默认 30 天），
//                只能拿去 POST /api/token/refresh 换新 access token。
// 密钥与验签路径完全相同，因此"刷新"的 CPU 成本是微秒级 HMAC，而不是几十毫秒的
// bcrypt —— 断线重连不再需要回头校验密码，这是重连风暴下登录延迟的根治手段。
// 两类 token 互不通用：见 verifyAs()。
// ---------------------------------------------------------------------------
std::string TokenService::sign(std::uint64_t userId, const std::string& username) const {
    const auto now = std::chrono::system_clock::now();                                              // 当前时间，作为 iat
    const auto exp = now + std::chrono::seconds(cfg_.expire_seconds);                               // 计算过期时间 exp

    return jwt::create()                                                                            // 开始构造 JWT
        .set_issuer(cfg_.issuer)                                                                    // iss：签发者，验签时会比对
        .set_subject(std::to_string(userId))                                                        // sub：用户ID，作为权威身份
        .set_id(randomJti())                                                                        // jti：本次签发的随机标识
        .set_payload_claim("username", jwt::claim(username))                                        // 自定义 claim：用户名
        .set_payload_claim("typ", jwt::claim(std::string("access")))                                // P0-3：访问令牌
        .set_issued_at(now)                                                                         // iat：签发时间
        .set_expires_at(exp)                                                                        // exp：过期时间，jwt-cpp 验签会自动检查
        .sign(jwt::algorithm::hs256{cfg_.secret});                                                  // 用共享密钥做 HMAC-SHA256 签名
}

std::string TokenService::signRefresh(std::uint64_t userId, const std::string& username) const {
    const auto now = std::chrono::system_clock::now();
    const auto exp = now + std::chrono::seconds(cfg_.refresh_expire_seconds);                       // 默认 30 天

    return jwt::create()
        .set_issuer(cfg_.issuer)
        .set_subject(std::to_string(userId))
        .set_id(randomJti())
        .set_payload_claim("username", jwt::claim(username))
        .set_payload_claim("typ", jwt::claim(std::string("refresh")))                               // P0-3：刷新令牌，禁止当访问凭证用
        .set_issued_at(now)
        .set_expires_at(exp)
        .sign(jwt::algorithm::hs256{cfg_.secret});
}

// ---------------------------------------------------------------------------
// verifyAs：两类 token 共用的验签实现。
// ----------------------------------------------------------------------------
// typ 判定规则（P0-3）：
//   * claim 存在：必须与 expectedTyp 完全一致，否则拒绝。refresh token 拿来
//     当 Bearer 用、或 access token 拿来刷新，都会被挡掉；
//   * claim 不存在：本次改动之前签发的老 token，只接受 access 用途。这样升级
//     过程中在线会话不会被强制下线，而 refresh token 一定带 typ，不存在
//     "没有 typ 的老 refresh token"混进来的可能。
// ---------------------------------------------------------------------------
bool TokenService::verifyAs(const std::string& token, const char* expectedTyp,
                            std::uint64_t& userId, std::string& username) const {
    try {                                                                                           // 任何解码/校验异常都视为失败
        const auto decoded = jwt::decode(token);                                                    // 拆出 header/payload，此时还未验签

        auto verifier = jwt::verify()                                                               // 创建校验器
            .allow_algorithm(jwt::algorithm::hs256{cfg_.secret})                                    // 只接受 HS256，且必须用我们的密钥
            .with_issuer(cfg_.issuer);                                                              // 强制校验 iss 与我们一致
        verifier.verify(decoded);                                                                   // 校验签名、iss、exp（过期会抛异常）

        if (!decoded.has_subject()) return false;                                                   // 没有 sub 说明不是本服务签发的合法 token

        if (decoded.has_payload_claim("typ")) {                                                     // 新签发的 token 一定带 typ
            const std::string typ = decoded.get_payload_claim("typ").as_string();
            if (typ != std::string(expectedTyp)) return false;                                      // 两类 token 严禁互串用途
        } else if (std::string(expectedTyp) != "access") {
            return false;                                                                           // 无 typ 的老 token 只能当 access 用
        }

        const std::uint64_t uid = std::stoull(decoded.get_subject());                               // 把 sub 字符串转回数字 userId

        std::string name;                                                                           // 用户名（可能不存在）
        if (decoded.has_payload_claim("username"))                                                  // 只有存在该 claim 时才读取
            name = decoded.get_payload_claim("username").as_string();                               // 取出用户名

        userId   = uid;                                                                             // 通过引用返回 userId
        username = std::move(name);                                                                 // 通过引用返回 username
        return true;                                                                                // 全部通过
    } catch (...) {                                                                                 // 签名错/过期/格式错/类型错/stoull 失败
        return false;                                                                               // 统一按校验失败处理
    }
}

// ---------------------------------------------------------------------------
// verify：校验 access token 并取回用户身份（HTTP Bearer / TCP 登录都走这里）。
// ---------------------------------------------------------------------------
bool TokenService::verify(const std::string& token,
                          std::uint64_t& userId,
                          std::string& username) const {
    return verifyAs(token, "access", userId, username);
}

// ---------------------------------------------------------------------------
// verifyRefresh：校验 refresh token（只接受 typ=refresh）。
// ---------------------------------------------------------------------------
bool TokenService::verifyRefresh(const std::string& token,
                                 std::uint64_t& userId,
                                 std::string& username) const {
    return verifyAs(token, "refresh", userId, username);
}

} // namespace kc
