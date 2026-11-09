#include "token_service.h"        // 本模块声明的类

#include <jwt-cpp/jwt.h>          // header-only 的 JWT 库：负责编码/解码与 HS256 签名

#include <chrono>                 // std::chrono：生成 iat/exp 时间点
#include <stdexcept>              // std::stoull 等可能抛出异常

namespace kc {

// ---------------------------------------------------------------------------
// 构造：拷贝一份配置，后续签发/验签都用这份不可变配置。
// ---------------------------------------------------------------------------
TokenService::TokenService(const JwtConfig& cfg) : cfg_(cfg) {}   // 拷贝构造，避免外部修改影响运行中的服务

// ---------------------------------------------------------------------------
// sign：签发 HS256 token。
// ---------------------------------------------------------------------------
std::string TokenService::sign(std::uint64_t userId, const std::string& username) const {
    const auto now = std::chrono::system_clock::now();                                              // 当前时间，作为 iat
    const auto exp = now + std::chrono::seconds(cfg_.expire_seconds);                               // 计算过期时间 exp

    return jwt::create()                                                                            // 开始构造 JWT
        .set_issuer(cfg_.issuer)                                                                    // iss：签发者，验签时会比对
        .set_subject(std::to_string(userId))                                                        // sub：用户ID，作为权威身份
        .set_payload_claim("username", jwt::claim(username))                                        // 自定义 claim：用户名
        .set_issued_at(now)                                                                         // iat：签发时间
        .set_expires_at(exp)                                                                        // exp：过期时间，jwt-cpp 验签会自动检查
        .sign(jwt::algorithm::hs256{cfg_.secret});                                                  // 用共享密钥做 HMAC-SHA256 签名
}

// ---------------------------------------------------------------------------
// verify：校验 token 并取回用户身份。
// ---------------------------------------------------------------------------
bool TokenService::verify(const std::string& token,
                          std::uint64_t& userId,
                          std::string& username) const {
    try {                                                                                           // 任何解码/校验异常都视为失败
        const auto decoded = jwt::decode(token);                                                    // 拆出 header/payload，此时还未验签

        auto verifier = jwt::verify()                                                               // 创建校验器
            .allow_algorithm(jwt::algorithm::hs256{cfg_.secret})                                    // 只接受 HS256，且必须用我们的密钥
            .with_issuer(cfg_.issuer);                                                              // 强制校验 iss 与我们一致
        verifier.verify(decoded);                                                                   // 校验签名、iss、exp（过期会抛异常）

        if (!decoded.has_subject()) return false;                                                   // 没有 sub 说明不是本服务签发的合法 token
        const std::uint64_t uid = std::stoull(decoded.get_subject());                               // 把 sub 字符串转回数字 userId

        std::string name;                                                                           // 用户名（可能不存在）
        if (decoded.has_payload_claim("username"))                                                  // 只有存在该 claim 时才读取
            name = decoded.get_payload_claim("username").as_string();                               // 取出用户名

        userId   = uid;                                                                             // 通过引用返回 userId
        username = std::move(name);                                                                 // 通过引用返回 username
        return true;                                                                                // 全部通过
    } catch (...) {                                                                                 // 签名错/过期/格式错/类型错
        return false;                                                                               // 统一按校验失败处理
    }
}

} // namespace kc
