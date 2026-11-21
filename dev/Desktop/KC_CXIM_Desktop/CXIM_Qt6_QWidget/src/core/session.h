#ifndef CXIM_SESSION_H
#define CXIM_SESSION_H

#include <QObject>
#include <QDateTime>

/**
 * @file session.h
 * @brief 登录会话与凭证管理(协议第 3 章: JWT access/refresh 双 token)
 *
 * 职责:
 *  - 保存 userId/username/access/refresh 及其绝对过期时间;
 *  - 判断 access 是否临近过期(供 HttpClient 在 401 前主动 refresh);
 *  - "记住密码/自动登录"的本地持久化(QSettings, 与旧版 Win32 客户端行为对齐);
 *  - 通过 tokenChanged/cleared 信号通知网络层, UI 层不直接读凭证。
 *
 * 安全说明: MVP 与后端协议口径一致, 凭证保存在用户目录 QSettings 中;
 * 后续如需加密可只替换本类的持久化实现。
 */
class Session : public QObject
{
    Q_OBJECT
public:
    explicit Session(QObject *parent = nullptr);

    bool isLoggedIn() const { return !m_accessToken.isEmpty() && m_userId != 0; }

    quint64 userId() const { return m_userId; }
    QString username() const { return m_username; }
    QString accessToken() const { return m_accessToken; }
    QString refreshToken() const { return m_refreshToken; }

    /** access 是否已过期或将在 graceSec 秒内过期(用于主动刷新) */
    bool accessTokenExpiring(int graceSec = 120) const;
    bool hasRefreshToken() const { return !m_refreshToken.isEmpty(); }

    /// 登录/刷新成功后统一入口(expiresIn 为秒)
    void setCredentials(quint64 userId, const QString &username,
                        const QString &access, const QString &refresh,
                        qint64 expiresIn, qint64 refreshExpiresIn);

    /// 仅更新 token(刷新成功后调用, userId/username 不变)
    void updateTokens(const QString &access, const QString &refresh,
                      qint64 expiresIn, qint64 refreshExpiresIn);

    void clear();

    // ---------------- 记住密码 / 自动登录(仅本地偏好) ----------------
    bool rememberPassword() const { return m_rememberPassword; }
    bool autoLogin() const { return m_autoLogin; }
    void setLoginPreferences(bool remember, bool autoLogin,
                             const QString &username, const QString &password);
    QString savedUsername() const { return m_savedUsername; }
    QString savedPassword() const { return m_savedPassword; }   // 本地明文, 与旧客户端一致

signals:
    void tokenChanged();     // 网络层监听: 重连 TCP 时使用新 token
    void cleared();

private:
    void persistPrefs();
    void loadPrefs();

    quint64 m_userId = 0;
    QString m_username;
    QString m_accessToken;
    QString m_refreshToken;
    qint64 m_accessExpireMs = 0;     // 绝对到期时间(毫秒)
    qint64 m_refreshExpireMs = 0;

    bool m_rememberPassword = false;
    bool m_autoLogin = false;
    QString m_savedUsername;
    QString m_savedPassword;
};

#endif // CXIM_SESSION_H
