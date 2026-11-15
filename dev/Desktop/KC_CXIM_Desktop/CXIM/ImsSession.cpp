#include "ImsSession.h"

#ifdef CXIM_IMS_CORE_SDK_VERSION

ImsSession& ImsSession::Instance()
{
    static ImsSession instance;
    return instance;
}

void ImsSession::SetLoginResult(uint64_t userId, const std::string& username, const std::string& token)
{
    m_userId = userId;
    m_username = username;
    m_token = token;
}

void ImsSession::Clear()
{
    m_userId = 0;
    m_username.clear();
    m_token.clear();
}

uint64_t ImsSession::UserId() const
{
    return m_userId;
}

const std::string& ImsSession::Username() const
{
    return m_username;
}

const std::string& ImsSession::Token() const
{
    return m_token;
}

bool ImsSession::IsLoggedIn() const
{
    return (m_userId != 0) && !m_token.empty();
}

#endif // CXIM_IMS_CORE_SDK_VERSION
