#include "accountservice.h"

#include <utility>   // std::exchange

AccountService::AccountService(HttpClient *http, Session *session, QObject *parent)
    : QObject(parent)
    , m_http(http)
    , m_session(session)
{
}

void AccountService::registerAccount(const QString &username, const QString &password,
                                     RegisterCb cb)
{
    QJsonObject body;
    body[QStringLiteral("username")] = username;
    body[QStringLiteral("password")] = password;
    m_http->post(QStringLiteral("/api/register"), body,
                 [cb](const ApiResult &res) {
        cb(res.ok, res.ok ? QString() : (res.message.isEmpty()
                    ? QStringLiteral("注册失败(http %1)").arg(res.httpStatus) : res.message),
           res.ok ? quint64(res.objectData().value(QStringLiteral("userId")).toDouble()) : 0);
    });
}

void AccountService::login(const QString &username, const QString &password, SimpleCb cb)
{
    QJsonObject body;
    body[QStringLiteral("username")] = username;
    body[QStringLiteral("password")] = password;
    m_http->post(QStringLiteral("/api/login"), body,
                 [this, cb, username](const ApiResult &res) {
        if (!res.ok) {
            QString err = res.message;
            if (err.isEmpty())
                err = res.httpStatus == 401 ? QStringLiteral("账号或密码错误")
                                            : QStringLiteral("登录失败(http %1)").arg(res.httpStatus);
            cb(false, err);
            return;
        }
        // 协议 4.2: data{userId,username,token,refreshToken,expiresIn,refreshExpiresIn}
        const QJsonObject d = res.objectData();
        m_session->setCredentials(
            quint64(d.value(QStringLiteral("userId")).toDouble()),
            d.value(QStringLiteral("username")).toString(username),
            d.value(QStringLiteral("token")).toString(),
            d.value(QStringLiteral("refreshToken")).toString(),
            qint64(d.value(QStringLiteral("expiresIn")).toDouble(86400)),
            qint64(d.value(QStringLiteral("refreshExpiresIn")).toDouble(2592000)));
        cb(true, QString());
        emit loggedIn(m_session->userId(), m_session->username());
    });
}

void AccountService::installRefreshHook()
{
    m_http->setRefreshHook([this](std::function<void(bool)> done) { doRefresh(std::move(done)); });
}

/// 并发去重: 同一时刻多个接口同时 401 时只发一次 /api/token/refresh, 结果广播
void AccountService::doRefresh(std::function<void(bool)> done)
{
    if (!m_session->hasRefreshToken()) { done(false); return; }

    m_refreshWaiters.append(std::move(done));
    if (m_refreshing)
        return;
    m_refreshing = true;

    QJsonObject body;
    body[QStringLiteral("refreshToken")] = m_session->refreshToken();
    m_http->post(QStringLiteral("/api/token/refresh"), body,
                 [this](const ApiResult &res) {
        bool ok = false;
        if (res.ok) {
            const QJsonObject d = res.objectData();
            m_session->updateTokens(
                d.value(QStringLiteral("token")).toString(),
                d.value(QStringLiteral("refreshToken")).toString(),
                qint64(d.value(QStringLiteral("expiresIn")).toDouble(86400)),
                qint64(d.value(QStringLiteral("refreshExpiresIn")).toDouble(2592000)));
            ok = true;
        }
        m_refreshing = false;
        const auto waiters = std::exchange(m_refreshWaiters, {});
        for (auto &w : waiters)
            w(ok);
        if (!ok)
            emit m_http->unauthorized();   // refresh 也失效 → 只能重新登录
    });
}

void AccountService::logout()
{
    m_session->clear();
    emit loggedOut();
}
