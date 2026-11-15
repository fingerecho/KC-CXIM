/** IMS 协议冒烟测试（不参与 CXIM.exe 构建，独立编译运行）
*  覆盖：注册/登录、搜索用户、好友申请/通过、130/131/132 事件、
*  文本消息、SendAck、去重、112 回执、110/111 历史消息、拉黑。
*  用法: ImsSmokeTest.exe [host] [httpPort] [tcpPort]
*/
#include "HttpClient.h"
#include "ImsSession.h"
#include "TcpClient.h"
#include "ImsJson.h"
#include "ImsTypes.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <vector>

namespace {

int g_failures = 0;

#define CHECK(cond, name)                                                     \
    do {                                                                      \
        if (cond) {                                                           \
            std::cout << "[PASS] " << (name) << std::endl;                    \
        }                                                                     \
        else {                                                                \
            std::cout << "[FAIL] " << (name) << std::endl;                    \
            ++g_failures;                                                     \
        }                                                                     \
    } while (0)

struct EventSink
{
    std::mutex mutex;
    std::condition_variable cv;
    std::vector<ImsMessage> messages;
    bool loggedIn = false;
    bool loginFailed = false;
    std::string loginError;

    void attach(TcpClient& client)
    {
        client.SetLoginCallback([this](bool success, const std::string& error, bool) {
            std::lock_guard<std::mutex> lock(mutex);
            if (success) {
                loggedIn = true;
            }
            else {
                loginFailed = true;
                loginError = error;
            }
            cv.notify_all();
        });
        client.SetMessageCallback([this](const ImsMessage& msg) {
            std::lock_guard<std::mutex> lock(mutex);
            messages.push_back(msg);
            cv.notify_all();
        });
    }

    bool waitLogin(int timeoutMs)
    {
        std::unique_lock<std::mutex> lock(mutex);
        cv.wait_for(lock, std::chrono::milliseconds(timeoutMs),
                    [this] { return loggedIn || loginFailed; });
        return loggedIn;
    }

    bool waitFor(const std::function<bool(const ImsMessage&)>& pred, int timeoutMs, ImsMessage& out)
    {
        std::unique_lock<std::mutex> lock(mutex);
        size_t scanned = 0;
        while (true) {
            for (; scanned < messages.size(); ++scanned) {
                if (pred(messages[scanned])) {
                    out = messages[scanned];
                    ++scanned;
                    return true;
                }
            }
            if (cv.wait_for(lock, std::chrono::milliseconds(200)) == std::cv_status::timeout) {
                if (timeoutMs <= 0) {
                    return false;
                }
                timeoutMs -= 200;
                if (timeoutMs <= 0) {
                    return false;
                }
            }
        }
    }
};

std::string RandomSuffix()
{
    static std::mt19937_64 rng(std::random_device{}());
    return "sm" + std::to_string(rng() % 100000000ULL);
}

} // namespace

int main(int argc, char** argv)
{
    const std::string host = (argc > 1) ? argv[1] : "110.40.206.253";
    const uint16_t httpPort = (argc > 2) ? static_cast<uint16_t>(std::atoi(argv[2])) : 8080;
    const uint16_t tcpPort = (argc > 3) ? static_cast<uint16_t>(std::atoi(argv[3])) : 8888;

    HttpClient& http = HttpClient::Instance();
    http.SetServer(host, httpPort);
    std::cout << "目标服务器 " << host << " HTTP:" << httpPort << " TCP:" << tcpPort << std::endl;

    const std::string suffix = RandomSuffix();
    const std::string userA = "smt_a_" + suffix;
    const std::string userB = "smt_b_" + suffix;
    const std::string password = "secret123";

    // 1) 注册 A/B
    int statusCode = 0;
    std::string responseBody;
    CHECK(http.Register(userA, password, statusCode, responseBody), "注册A " + responseBody);

    // 2) 登录 A，写入会话
    HttpClient::LoginResult loginA;
    if (!http.Login(userA, password, statusCode, responseBody, loginA) || (statusCode != 200)) {
        std::cout << "[FAIL] 登录A: " << responseBody << std::endl;
        return 1;
    }
    ImsSession::Instance().SetLoginResult(loginA.userId, loginA.username, loginA.token);
    std::cout << "A userId=" << loginA.userId << std::endl;

    // 3) 注册 B（Register 不需要鉴权）、登录 B 并保存 token
    CHECK(http.Register(userB, password, statusCode, responseBody), "注册B " + responseBody);
    HttpClient::LoginResult loginB;
    if (!http.Login(userB, password, statusCode, responseBody, loginB) || (statusCode != 200)) {
        std::cout << "[FAIL] 登录B: " << responseBody << std::endl;
        return 1;
    }
    std::cout << "B userId=" << loginB.userId << std::endl;

    // 4) A 会话：搜索 B（验证 GET + Bearer + 解析）
    ImsSession::Instance().SetLoginResult(loginA.userId, loginA.username, loginA.token);
    std::vector<SearchUser> found;
    auto result = http.SearchUsers(userB, 20, found);
    CHECK(result.ok && (found.size() == 1) && (found[0].userId == loginB.userId), "A 搜索 B");

    // 5) B 先建立 TCP 连接
    TcpClient tcpB;
    tcpB.SetServer(host, tcpPort);
    EventSink sinkB;
    sinkB.attach(tcpB);
    tcpB.Start(loginB.token);
    CHECK(sinkB.waitLogin(10000), "B TCP 登录");

    // 6) A 会话：发送好友申请 → B 实时收到 130
    FriendRequestInfo createdReq;
    result = http.SendFriendRequest(loginB.userId, "你好，我是 A", createdReq);
    CHECK(result.ok && (createdReq.requestId != 0), "A 发送好友申请");

    ImsMessage ev130;
    const bool got130 = sinkB.waitFor(
        [](const ImsMessage& m) { return m.type == static_cast<int>(ImsMsgType::FriendRequest); },
        10000, ev130);
    bool eventOk130 = false;
    ImsFriendEvent fev130;
    if (got130) {
        eventOk130 = ParseImsFriendEvent(ev130.content, fev130) &&
                     (fev130.event == "new") && (fev130.requesterId == loginA.userId);
    }
    CHECK(got130 && eventOk130, "B 收到 130 好友申请事件");

    // 7) A 先建立 TCP 连接，再由 B 通过申请 → A 实时收到 131/132，B 收到 132
    TcpClient tcpA;
    tcpA.SetServer(host, tcpPort);
    EventSink sinkA;
    sinkA.attach(tcpA);
    tcpA.Start(loginA.token);
    CHECK(sinkA.waitLogin(10000), "A TCP 登录");

    ImsSession::Instance().SetLoginResult(loginB.userId, loginB.username, loginB.token);
    const uint64_t requestId = (fev130.requestId != 0) ? fev130.requestId : ev130.msg_id;
    result = http.AcceptFriendRequest(requestId);
    CHECK(result.ok, "B 通过好友申请");

    ImsMessage ev131;
    CHECK(sinkA.waitFor([](const ImsMessage& m) {
              return m.type == static_cast<int>(ImsMsgType::FriendRequestResult);
          }, 10000, ev131) &&
              (ParseImsFriendEvent(ev131.content, fev130) || true) &&
              (ev131.content.find("accepted") != std::string::npos),
          "A 收到 131 accepted");

    ImsMessage ev132;
    CHECK(sinkA.waitFor([](const ImsMessage& m) {
              return m.type == static_cast<int>(ImsMsgType::FriendListChanged) &&
                     m.content.find("added") != std::string::npos;
          }, 10000, ev132),
          "A 收到 132 added");

    // 8) A 会话：好友列表包含 B（PATCH 备注 + 列表 remark 字段）
    ImsSession::Instance().SetLoginResult(loginA.userId, loginA.username, loginA.token);
    result = http.SetFriendRemark(loginB.userId, "测试备注B");
    CHECK(result.ok, "A 修改 B 的备注");
    std::vector<FriendInfo> friendsA;
    result = http.GetFriends(friendsA);
    bool remarkOk = false;
    for (const FriendInfo& f : friendsA) {
        if ((f.userId == loginB.userId) && (f.remark == "测试备注B")) {
            remarkOk = true;
        }
    }
    CHECK(result.ok && remarkOk, "A 好友列表包含 B 且备注正确");

    // 9) A -> B 发文本，B 收到并回 112；A 收到 113
    const uint64_t tmpMsgId = 9876543210ULL;
    ImsMessage chatMsg;
    chatMsg.type = static_cast<int>(ImsMsgType::Text);
    chatMsg.to = loginB.userId;
    chatMsg.msg_id = tmpMsgId;
    chatMsg.content = "你好B hello";
    CHECK(tcpA.Send(chatMsg), "A 发送文本消息");

    ImsMessage gotChat;
    const bool gotMsg = sinkB.waitFor(
        [&](const ImsMessage& m) {
            return (m.type == static_cast<int>(ImsMsgType::Text)) && (m.from == loginA.userId);
        },
        10000, gotChat);
    CHECK(gotMsg && (gotChat.content == "你好B hello"), "B 收到文本消息");

    if (gotMsg) {
        ImsMessage ack;
        ack.type = static_cast<int>(ImsMsgType::Ack);
        ack.conversation_id = gotChat.conversation_id;
        ack.seq = gotChat.seq;
        ack.content = "delivered";
        CHECK(tcpB.Send(ack), "B 回投递回执 112 delivered");
        ack.content.clear();
        CHECK(tcpB.Send(ack), "B 回已读回执 112");
    }

    ImsMessage gotAck;
    const bool gotSendAck = sinkA.waitFor(
        [](const ImsMessage& m) { return m.type == static_cast<int>(ImsMsgType::SendAck); },
        10000, gotAck);
    CHECK(gotSendAck && ((gotAck.content == "sent") || (gotAck.content == "duplicate")),
          "A 收到 113 SendAck");

    // 10) 相同 msg_id 再发一次 → 服务端去重，SendAck content=duplicate
    CHECK(tcpA.Send(chatMsg), "A 重发相同临时 msg_id");
    ImsMessage gotDup;
    const bool gotDuplicate = sinkA.waitFor(
        [](const ImsMessage& m) {
            return (m.type == static_cast<int>(ImsMsgType::SendAck)) && (m.content == "duplicate");
        },
        10000, gotDup);
    CHECK(gotDuplicate, "重复消息命中去重 (duplicate)");

    // 11) B 拉取与 A 的历史消息
    ImsMessage histReq;
    histReq.type = static_cast<int>(ImsMsgType::HistoryReq);
    histReq.to = loginA.userId;
    histReq.seq = 0;
    histReq.content = "20";
    CHECK(tcpB.Send(histReq), "B 请求历史消息 110");

    ImsMessage histResp;
    const bool gotHistory = sinkB.waitFor(
        [](const ImsMessage& m) { return m.type == static_cast<int>(ImsMsgType::HistoryResp); },
        10000, histResp);
    std::vector<ImsHistoryMessage> history;
    std::string histErr;
    if (gotHistory) {
        ParseImsHistory(histResp.content, history, histErr);
    }
    bool historyHasMsg = false;
    for (const ImsHistoryMessage& h : history) {
        if ((h.from == loginA.userId) && (h.content == "你好B hello")) {
            historyHasMsg = true;
        }
    }
    CHECK(gotHistory && historyHasMsg, "B 历史消息包含刚发的消息(二次JSON解析正确)");

    // 12) 离线补推：停止 A 的连接，B 再发消息，A 重连后补收
    tcpA.Stop();
    std::this_thread::sleep_for(std::chrono::milliseconds(800));
    ImsMessage offlineMsg;
    offlineMsg.type = static_cast<int>(ImsMsgType::Text);
    offlineMsg.to = loginA.userId;
    offlineMsg.msg_id = 1234567890ULL;
    offlineMsg.content = "offline_push_test";
    CHECK(tcpB.Send(offlineMsg), "B 发送消息（A 离线）");
    std::this_thread::sleep_for(std::chrono::milliseconds(800));

    TcpClient tcpA2;
    tcpA2.SetServer(host, tcpPort);
    EventSink sinkA2;
    sinkA2.attach(tcpA2);
    tcpA2.Start(loginA.token);
    ImsMessage gotOffline;
    CHECK(sinkA2.waitLogin(10000) &&
              sinkA2.waitFor([](const ImsMessage& m) {
                  return (m.type == static_cast<int>(ImsMsgType::Text)) &&
                         (m.content == "offline_push_test");
              }, 10000, gotOffline),
          "A 重连收到离线补推消息");

    // 13) B 拉黑 A（HTTP）；随后 A 发消息应收到 102 错误
    result = http.BlockFriend(loginB.userId); // 当前会话是 A，A 拉黑 B
    CHECK(result.ok, "A 拉黑 B");
    ImsMessage blockedMsg;
    blockedMsg.type = static_cast<int>(ImsMsgType::Text);
    blockedMsg.to = loginB.userId;
    blockedMsg.msg_id = 222233334444ULL;
    blockedMsg.content = "should_fail";
    CHECK(tcpA2.Send(blockedMsg), "被拉黑方继续发送");
    ImsMessage gotErr;
    CHECK(sinkA2.waitFor([](const ImsMessage& m) {
              return m.type == static_cast<int>(ImsMsgType::Error);
          }, 10000, gotErr) &&
              (gotErr.content == "blocked by recipient"),
          "A 收到 102 blocked by recipient");

    result = http.UnblockFriend(loginB.userId);
    CHECK(result.ok, "A 取消拉黑 B");

    tcpA2.Stop();
    tcpB.Stop();

    std::cout << "==== 冒烟测试完成，失败项: " << g_failures << " ====" << std::endl;
    return (g_failures == 0) ? 0 : 1;
}
