/**
 * @file tools/smoke/main.cpp
 * @brief 无头协议冒烟测试: 复用 core/net/service 三层(证明其与 UI 零耦合)
 *
 * 覆盖链路(与 docs/IMS_communication_protocol.md 口径逐条对照):
 *   HTTP 登录 → TCP type=100 登录(101) → GET /api/friends →
 *   TCP 单聊文本(type=1, 带去重 msg_id) → SendAck(113 sent/duplicate) →
 *   历史请求(110) → HistoryResp(111, content 二次解析)
 *
 * 用法: cxim_smoke [username password [friendId]]
 *       (缺省用测试账号 fingerecho11, friendId 缺省取好友列表第一项)
 * 退出码: 0 全链路通过 / 2 超时 / 3 业务失败
 */
#include <QCoreApplication>
#include <QDebug>
#include <QDateTime>

#include "../../src/service/imservice.h"

static int g_rc = 3;

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);

    QString user = argc > 1 ? QString::fromLocal8Bit(argv[1]) : QStringLiteral("fingerecho11");
    QString pass = argc > 2 ? QString::fromLocal8Bit(argv[2]) : QStringLiteral("asdfls");
    quint64 friendId = argc > 3 ? QString::fromLocal8Bit(argv[3]).toULongLong() : 0;

    ImService im;
    qInfo().noquote() << "[smoke] http =" << im.config().httpBaseUrl()
                     << " tcp =" << im.config().tcpHost() << im.config().tcpPort();

    // 看门狗: 40s 内必须走完链路
    QTimer watchdog;
    watchdog.setSingleShot(true);
    QObject::connect(&watchdog, &QTimer::timeout, [&]() {
        qCritical() << "[smoke] TIMEOUT";
        g_rc = 2;
        app.quit();
    });
    watchdog.start(40000);

    quint64 sentClientMsgId = 0;

    // 5) 历史回来 → 成功
    QObject::connect(im.chat(), &ChatService::historyLoaded,
                     [&](const QString &key, const QList<ChatMessage> &msgs,
                         quint64 oldest, bool reachedEnd) {
        Q_UNUSED(oldest); Q_UNUSED(reachedEnd);
        if (key != ChatService::convKey(friendId, false))
            return;
        qInfo().noquote() << "[smoke] history OK:" << msgs.size() << "msgs, latest="
                          << (msgs.isEmpty() ? QStringLiteral("-")
                                             : msgs.last().displayPreview().left(40));
        g_rc = 0;
        app.quit();
    });

    // 4) SendAck → 拉历史
    QObject::connect(im.chat(), &ChatService::messageStatusChanged,
                     [&](const QString &, quint64 cid, MessageStatus st,
                         const QString &err) {
        if (cid != sentClientMsgId)
            return;
        if (st == MessageStatus::Failed) {
            qCritical() << "[smoke] send FAILED:" << err;
            g_rc = 3;
            app.quit();
            return;
        }
        qInfo().noquote() << "[smoke] SendAck OK (status=" << int(st) << ")";
        im.chat()->requestHistory(friendId, false, 0, 20);
    });

    // 3) 好友列表 → 发消息
    QObject::connect(im.chat(), &ChatService::tcpLoggedIn, [&]() {
        qInfo() << "[smoke] TCP login OK (101)";
        im.friends()->listFriends([&](bool ok, const QList<FriendInfo> &friends,
                                      const QString &err) {
            if (!ok || (friendId == 0 && friends.isEmpty())) {
                qCritical() << "[smoke] friends list fail/empty:" << err;
                g_rc = 3;
                app.quit();
                return;
            }
            if (friendId == 0)
                friendId = friends.first().userId;
            qInfo().noquote() << "[smoke] friends =" << friends.size()
                              << "→ send to" << friendId;
            const QString text = QStringLiteral("qt-smoke %1")
                                     .arg(QDateTime::currentMSecsSinceEpoch());
            sentClientMsgId = im.chat()->sendText(friendId, false, text);
        });
    });

    // TCP 致命错误(token 非法等)
    QObject::connect(im.tcp(), &TcpClient::loginResult, [&](bool ok, const QString &reason) {
        if (!ok) {
            qCritical() << "[smoke] TCP login rejected:" << reason;
            g_rc = 3;
            app.quit();
        }
    });

    // 1+2) HTTP 登录 → 启动数据面
    im.account()->login(user, pass, [&](bool ok, const QString &err) {
        if (!ok) {
            qCritical() << "[smoke] HTTP login fail:" << err;
            g_rc = 3;
            app.quit();
            return;
        }
        qInfo().noquote() << "[smoke] HTTP login OK userId ="
                          << im.session()->userId();
        im.startDataPlane();
    });

    app.exec();
    qInfo().noquote() << "[smoke] exit code =" << g_rc;
    return g_rc;
}
