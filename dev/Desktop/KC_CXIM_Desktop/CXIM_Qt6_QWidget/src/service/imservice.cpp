#include "imservice.h"

ImService::ImService(QObject *parent)
    : QObject(parent)
    , m_session(this)
    , m_http(&m_session, this)
    , m_tcp(this)
    , m_uploader(&m_http, this)
    , m_account(&m_http, &m_session, this)
    , m_friends(&m_http, this)
    , m_groups(&m_http, this)
    , m_chat(&m_tcp, &m_session, this)
{
    // 配置注入: net 层只拿字符串, 不认识 AppConfig
    m_http.setBaseUrl(m_config.httpBaseUrl());
    m_tcp.setHeartbeatIntervalMs(m_config.heartbeatIntervalMs());

    // 401 兜底(刷新也失败) → 统一走 sessionExpired, UI 只需监听一个信号
    connect(&m_http, &HttpClient::unauthorized, this, [this]() {
        shutdownDataPlane();
        m_session.clear();
        emit sessionExpired();
    });
    // TCP 登录被拒(102, 通常是 access 过期): 先 refresh 再带新 token 重连;
    // 刷新仍失败才回登录页(协议 3.3 断线重连推荐流程)
    connect(&m_tcp, &TcpClient::loginResult, this, [this](bool ok, const QString &) {
        if (ok || m_session.accessToken().isEmpty())
            return;                     // 成功, 或本就是 stop() 后的空转
        m_account.refresh([this](bool refreshed) {
            if (refreshed)
                m_tcp.updateToken(m_session.accessToken());
            else
                emit sessionExpired();
        });
    });
    // access 临期时主动刷新一次, 避免 TCP 登录/HTTP 请求踩到过期(协议 3.3)
    connect(&m_session, &Session::tokenChanged, this, [this]() {
        m_account.installRefreshHook();   // 幂等
    });
}

void ImService::startDataPlane()
{
    m_tcp.start(m_config.tcpHost(), m_config.tcpPort(), m_session.accessToken());
}

void ImService::shutdownDataPlane()
{
    m_tcp.stop();
}
