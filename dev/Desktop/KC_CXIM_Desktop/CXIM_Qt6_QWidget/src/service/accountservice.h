#ifndef CXIM_ACCOUNTSERVICE_H
#define CXIM_ACCOUNTSERVICE_H

#include <QObject>
#include "../net/httpclient.h"
#include "../core/session.h"

/**
 * @file accountservice.h
 * @brief 账号业务: 注册 / 登录 / 刷新 token / 登出(协议 4.1~4.3)
 *
 * 依赖方向: UI → service → net。本类是 HttpClient 刷新钩子的实现方:
 * HTTP 层遇到 401 时回调 refresh(), 刷新成功则自动重放原请求,
 * 失败则上抛 unauthorized, UI 回登录页(协议 3.3 推荐流程)。
 */
class AccountService : public QObject
{
    Q_OBJECT
public:
    AccountService(HttpClient *http, Session *session, QObject *parent = nullptr);

    using SimpleCb = std::function<void(bool ok, const QString &error)>;
    using RegisterCb = std::function<void(bool ok, const QString &error, quint64 userId)>;

    void registerAccount(const QString &username, const QString &password, RegisterCb cb);
    void login(const QString &username, const QString &password, SimpleCb cb);
    /// 主动刷新(供 TCP 登录失败等场景调用; 内部与 401 钩子共享并发去重)
    void refresh(std::function<void(bool ok)> done) { doRefresh(std::move(done)); }
    /// 把 refresh 能力注入 HttpClient(组装根调用一次)
    void installRefreshHook();
    void logout();

signals:
    void loggedIn(quint64 userId, const QString &username);
    void loggedOut();

private:
    void doRefresh(std::function<void(bool)> done);   // 并发去重: 多个 401 共享一次刷新

    HttpClient *m_http;
    Session *m_session;
    bool m_refreshing = false;
    QList<std::function<void(bool)>> m_refreshWaiters;
};

#endif // CXIM_ACCOUNTSERVICE_H
