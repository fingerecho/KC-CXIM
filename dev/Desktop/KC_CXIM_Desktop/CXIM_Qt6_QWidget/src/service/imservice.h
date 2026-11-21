#ifndef CXIM_IMSERVICE_H
#define CXIM_IMSERVICE_H

#include <QObject>

#include "../core/appconfig.h"
#include "../core/session.h"
#include "../net/httpclient.h"
#include "../net/tcpclient.h"
#include "../net/fileuploader.h"
#include "accountservice.h"
#include "friendservice.h"
#include "groupservice.h"
#include "chatservice.h"

/**
 * @file imservice.h
 * @brief 组装根(Composition Root): 按依赖顺序创建并接线全部功能对象
 *
 * 依赖图(单向, 无环):
 *   AppConfig/Session → HttpClient/TcpClient/FileUploader → 各 Service → UI
 * UI 层只持有 ImService* 并从其取各 service, 不感知 net 层存在;
 * 将来接入 IM 云厂商/多协议时, 替换本组装根即可。
 */
class ImService : public QObject
{
    Q_OBJECT
public:
    explicit ImService(QObject *parent = nullptr);

    Session       *session()  { return &m_session; }
    AccountService *account() { return &m_account; }
    FriendService *friends()  { return &m_friends; }
    GroupService  *groups()   { return &m_groups; }
    ChatService   *chat()     { return &m_chat; }
    FileUploader  *uploader() { return &m_uploader; }
    TcpClient     *tcp()      { return &m_tcp; }
    const AppConfig &config() const { return m_config; }

    /// 登录成功后: 启动 TCP 数据面(HTTP 服务无需显式启动)
    void startDataPlane();
    /// 登出/被踢: 断 TCP, 清会话
    void shutdownDataPlane();

signals:
    /// 凭证彻底失效(401 且刷新失败): UI 监听后回登录页
    void sessionExpired();

private:
    AppConfig m_config;
    Session m_session;
    HttpClient m_http;
    TcpClient m_tcp;
    FileUploader m_uploader;
    AccountService m_account;
    FriendService m_friends;
    GroupService m_groups;
    ChatService m_chat;
};

#endif // CXIM_IMSERVICE_H
