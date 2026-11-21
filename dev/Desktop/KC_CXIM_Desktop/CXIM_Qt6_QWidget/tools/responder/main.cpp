/**
 * @file tools/responder/main.cpp
 * @brief 无头"对端账号"应答器: 模拟 GUI 测试的对手方(fingerecho15)
 *
 * 职责(全程走真实后端, 与 GUI 端互测):
 *   1. 登录 + TCP 上线;
 *   2. 收到来自目标账号的好友申请 → HTTP accept(启动时也补一次 pending 列表);
 *   3. 收到文本 → 回复 "收到: <text>";
 *   4. 收到图片/文件 → 回复一个文本文件 + (图片来时)一张生成的 PNG,
 *      完整覆盖 FileUploader 直传 + sendMedia 链路;
 *   5. 空闲 10 分钟自动退出。
 *
 * 用法: cxim_responder <username> <password> <target_username>
 */
#include <QCoreApplication>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QImage>
#include <QPainter>
#include <QJsonDocument>
#include <QRandomGenerator>
#include <QTimer>

#include "../../src/service/imservice.h"

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);
    if (argc < 4) {
        qCritical() << "usage: cxim_responder <user> <pass> <target_user>";
        return 3;
    }
    const QString user = QString::fromLocal8Bit(argv[1]);
    const QString pass = QString::fromLocal8Bit(argv[2]);
    const QString targetUser = QString::fromLocal8Bit(argv[3]);

    ImService im;
    quint64 targetId = 0;                 // fingerecho11 的 userId
    QTimer idleQuit;                      // 空闲退出看门狗
    idleQuit.setSingleShot(true);
    QObject::connect(&idleQuit, &QTimer::timeout, [] {
        qInfo() << "[resp] idle timeout, bye";
        QCoreApplication::quit();
    });
    auto kick = [&idleQuit] { idleQuit.start(600000); };
    kick();

    auto ensureTargetId = [&](std::function<void()> then) {
        if (targetId) { then(); return; }
        im.friends()->searchUsers(targetUser, [&](bool ok, const QList<UserCard> &users,
                                                  const QString &err) {
            if (!ok || users.isEmpty()) {
                qWarning() << "[resp] search target failed:" << err;
                return;
            }
            targetId = users.first().userId;
            qInfo().noquote() << "[resp] target" << targetUser << "id =" << targetId;
            then();
        });
    };

    auto acceptPendingFromTarget = [&]() {
        im.friends()->listRequests(QStringLiteral("incoming"),
                                   [&](bool ok, const QList<FriendRequestInfo> &reqs,
                                       const QString &err) {
            if (!ok) {
                qWarning() << "[resp] listRequests FAIL:" << err;
                return;
            }
            qInfo() << "[resp] pending incoming requests =" << reqs.size();
            for (const FriendRequestInfo &r : reqs) {
                if (r.status == 0 && r.requesterUsername == targetUser) {
                    im.friends()->acceptRequest(r.requestId, [&](bool aok, const QString &e) {
                        qInfo().noquote() << "[resp] accepted friend request"
                                          << r.requestId << (aok ? "OK" : "FAIL " + e);
                    });
                }
            }
        });
    };

    // ---- 回复文件: 生成小文件 → 直传 → sendMedia ----
    auto sendFileBack = [&](int mediaType) {
        const QString dir = QDir::tempPath() + QStringLiteral("/cxim_resp");
        QDir().mkpath(dir);
        QString path;
        if (mediaType == MsgType::Image) {
            path = dir + QStringLiteral("/resp_%1.png")
                       .arg(QRandomGenerator::global()->bounded(100000));
            QImage img(120, 80, QImage::Format_RGB32);
            img.fill(QColor(0x4a, 0xa3, 0xe8));
            QPainter p(&img);
            p.setPen(Qt::white);
            p.drawText(img.rect(), Qt::AlignCenter, QStringLiteral("RESP"));
            p.end();
        } else {
            path = dir + QStringLiteral("/resp_%1.txt")
                       .arg(QRandomGenerator::global()->bounded(100000));
            QFile f(path);
            f.open(QIODevice::WriteOnly);
            f.write("reply from responder\n");
            f.close();
        }
        const int type = mediaType;
        // 直传完成后在回调里 sendMedia(上传结果 → fileId)
        im.uploader()->upload(path, nullptr,
                              [&im, targetId, type](const FileUploader::Result &r) {
            if (!r.ok) {
                qWarning() << "[resp] upload fail:" << r.error;
                return;
            }
            qInfo().noquote() << "[resp] sendMedia back type=" << type
                              << "fileId=" << r.meta.fileId;
            im.chat()->sendMedia(targetId, false, type, r.meta.fileId, r.meta.name);
        });
    };

    // ---- 收到消息 → 回复 ----
    QObject::connect(im.chat(), &ChatService::messageIncoming,
                     [&](const QString &key, const ChatMessage &m) {
        Q_UNUSED(key);
        if (m.outgoing || m.fromUid != targetId)
            return;
        kick();
        qInfo().noquote() << "[resp] got type" << m.type << ":" << m.content.left(60);
        switch (m.type) {
        case MsgType::Text:
            im.chat()->sendText(targetId, false, QStringLiteral("收到: ") + m.content);
            break;
        case MsgType::Image:
            im.chat()->sendText(targetId, false, QStringLiteral("收到图片, 回你一张"));
            sendFileBack(MsgType::Image);
            break;
        case MsgType::File:
            im.chat()->sendText(targetId, false, QStringLiteral("收到文件, 回你一个"));
            sendFileBack(MsgType::File);
            break;
        default:
            break;
        }
    });

    // ---- 收到好友申请 → 通过 ----
    QObject::connect(im.chat(), &ChatService::friendRequestEvent,
                     [&](const QJsonObject &payload) {
        kick();
        const QString requester = payload.value(QStringLiteral("requester_username")).toString();
        if (requester != targetUser)
            return;
        // 优先用事件里的 request_id 直接 accept(少一次列表往返); 缺失再走列表兜底
        const quint64 reqId = quint64(payload.value(QStringLiteral("request_id")).toDouble());
        qInfo().noquote() << "[resp] friend request from target, request_id =" << reqId;
        if (reqId) {
            im.friends()->acceptRequest(reqId, [&](bool aok, const QString &e) {
                qInfo().noquote() << "[resp] accepted" << reqId << (aok ? "OK" : "FAIL " + e);
                if (!aok)
                    acceptPendingFromTarget();
            });
        } else {
            acceptPendingFromTarget();
        }
    });

    // ---- 启动链: 登录 → TCP → 找 target → 补通过期申请 ----
    QObject::connect(im.chat(), &ChatService::tcpLoggedIn, [&]() {
        qInfo() << "[resp] tcp online";
        ensureTargetId(acceptPendingFromTarget);
    });
    QObject::connect(im.tcp(), &TcpClient::loginResult,
                     [](bool ok, const QString &reason) {
        if (!ok) { qCritical() << "[resp] tcp login rejected:" << reason;
                   QCoreApplication::exit(3); }
    });

    im.account()->login(user, pass, [&](bool ok, const QString &err) {
        if (!ok) { qCritical() << "[resp] login fail:" << err;
                   QCoreApplication::exit(3); return; }
        qInfo().noquote() << "[resp] logged in as" << user
                          << "id =" << im.session()->userId();
        im.startDataPlane();
    });

    app.exec();
    return 0;
}
