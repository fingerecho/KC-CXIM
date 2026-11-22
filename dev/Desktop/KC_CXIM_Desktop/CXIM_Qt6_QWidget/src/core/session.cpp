#include "session.h"

#include <QSettings>

// 本地偏好存储(记住密码/自动登录)统一走 org/app 注册的 QSettings,
// 与协议无关, 属于客户端本地状态
static QSettings &settings()
{
    static QSettings s(QStringLiteral("CXIM.ini"), QSettings::IniFormat);
    return s;
}

Session::Session(QObject *parent) : QObject(parent)
{
    loadPrefs();
}

bool Session::accessTokenExpiring(int graceSec) const
{
    if (m_accessToken.isEmpty())
        return true;
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    return now >= m_accessExpireMs - static_cast<qint64>(graceSec) * 1000;
}

void Session::setCredentials(quint64 userId, const QString &username,
                             const QString &access, const QString &refresh,
                             qint64 expiresIn, qint64 refreshExpiresIn)
{
    m_userId = userId;
    m_username = username;
    updateTokens(access, refresh, expiresIn, refreshExpiresIn);
}

void Session::updateTokens(const QString &access, const QString &refresh,
                           qint64 expiresIn, qint64 refreshExpiresIn)
{
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    m_accessToken = access;
    m_accessExpireMs = now + expiresIn * 1000;
    if (!refresh.isEmpty()) {          // 刷新接口会轮换 refresh, 登录接口两者都带
        m_refreshToken = refresh;
        m_refreshExpireMs = now + refreshExpiresIn * 1000;
    }
    emit tokenChanged();
}

void Session::clear()
{
    m_userId = 0;
    m_username.clear();
    m_accessToken.clear();
    m_refreshToken.clear();
    m_accessExpireMs = m_refreshExpireMs = 0;
    emit cleared();
}

void Session::setLoginPreferences(bool remember, bool autoLogin,
                                  const QString &username, const QString &password)
{
    m_rememberPassword = remember;
    m_autoLogin = remember ? autoLogin : false;   // 自动登录依赖记住密码
    m_savedUsername = remember ? username : QString();
    m_savedPassword = remember ? password : QString();
    persistPrefs();
}

void Session::disableAutoLogin()
{
    // 没开过就不写盘, 避免每次退出都产生一次无意义的 ini 写入
    if (!m_autoLogin)
        return;
    m_autoLogin = false;
    // m_rememberPassword / m_savedUsername / m_savedPassword 一律不动:
    // 用户只是不想再"启动即登录", 不是要忘掉账号密码。
    persistPrefs();
}

void Session::persistPrefs()
{
    QSettings &s = settings();
    s.setValue(QStringLiteral("login/remember"), m_rememberPassword);
    s.setValue(QStringLiteral("login/autoLogin"), m_autoLogin);
    s.setValue(QStringLiteral("login/username"), m_savedUsername);
    s.setValue(QStringLiteral("login/password"), m_savedPassword);
}

void Session::loadPrefs()
{
    QSettings &s = settings();
    m_rememberPassword = s.value(QStringLiteral("login/remember"), false).toBool();
    m_autoLogin = s.value(QStringLiteral("login/autoLogin"), false).toBool();
    m_savedUsername = s.value(QStringLiteral("login/username")).toString();
    m_savedPassword = s.value(QStringLiteral("login/password")).toString();
}
