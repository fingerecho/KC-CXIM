#ifndef CXIM_IMS_SESSION_H_
#define CXIM_IMS_SESSION_H_

#include "CXIMAppConfig.h"

#ifdef CXIM_IMS_CORE_SDK_VERSION

#include <cstdint>
#include <string>

/** @class ImsSession
* @brief 全局登录会话上下文（单例），保存 HTTP 登录得到的 userId/username/token(JWT)
*/
class ImsSession
{
public:
    static ImsSession& Instance();

    void SetLoginResult(uint64_t userId, const std::string& username, const std::string& token);
    void Clear();

    uint64_t UserId() const;
    const std::string& Username() const;
    const std::string& Token() const;
    bool IsLoggedIn() const;

private:
    ImsSession() = default;
    ImsSession(const ImsSession&) = delete;
    ImsSession& operator=(const ImsSession&) = delete;

private:
    uint64_t m_userId = 0;
    std::string m_username;
    std::string m_token;
};

#endif // CXIM_IMS_CORE_SDK_VERSION

#endif // CXIM_IMS_SESSION_H_
