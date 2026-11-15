#ifndef EXAMPLES_LAYOUTS_FORM_H_
#define EXAMPLES_LAYOUTS_FORM_H_

// duilib
#include "duilib/duilib.h"

#include "CXIMAppConfig.h"

#ifdef TENCENT_CLOUD_IM_SDK_VERSION
#include "TIMCloud.h"
#endif //!TENCENT_CLOUD_IM_SDK_VERSION

#ifdef CXIM_IMS_CORE_SDK_VERSION
#include "ImsTypes.h"
#include <set>
#include <vector>
#endif //CXIM_IMS_CORE_SDK_VERSION

#include <memory>
#include <map>
#include <string>

class ChatForm : public ui::WindowImplBase
{
public:
    ChatForm(const DString& class_name, const DString& theme_directory, const DString& layout_xml);
    virtual ~ChatForm() override;

    /** 资源相关接口
     * GetSkinFolder 接口设置你要绘制的窗口皮肤资源路径
     * GetSkinFile 接口设置你要绘制的窗口的 xml 描述文件
     */
    virtual DString GetSkinFolder() override;
    virtual DString GetSkinFile() override;

    /** 当窗口创建完成以后调用此函数，供子类中做一些初始化的工作
    */
    virtual void OnInitWindow() override;

public:
    static void ShowCustomWindow(const DString& class_name, const DString& theme_directory, const DString& layout_xml);

private:
#ifdef TENCENT_CLOUD_IM_SDK_VERSION
    void LoadSelfProfile();
    void RefreshFriendList();
    void SelectFriend(const std::string& userId);
    void LoadMessageHistory(const std::string& userId);
    void AppendChatMessage(bool fromSelf, const std::string& sender, const std::string& text);
    void SendCurrentMessage();
    void EnsureConversationRow(const std::string& userId, const std::string& text);
#endif

#ifdef CXIM_IMS_CORE_SDK_VERSION
    /** ---- IMS 核心接口对接（HTTP + TCP）---- */
    void InitIms();

    // TCP 事件（网络线程回调，内部转投 UI 线程）
    void OnTcpLoginResult(bool success, const std::string& error, bool firstLogin);
    void OnTcpDisconnected();
    void OnTcpMessage(const ImsMessage& msg);

    // TCP 消息分发（UI 线程）
    void HandleChatMessage(const ImsMessage& msg, bool fromHistory);
    void HandleSendAck(const ImsMessage& msg);
    void HandleHistoryResp(const ImsMessage& msg);
    void HandleTcpError(const ImsMessage& msg);
    void HandleFriendEvent(const ImsMessage& msg);

    // HTTP 业务（后台线程执行 work，UI 线程执行 done）
    void RunAsync(std::function<void()> work, std::function<void()> done);
    void PostToUi(std::function<void()> task);
    void RefreshFriendList();
    void RefreshFriendRequests();
    void RenderFriendList(const std::vector<FriendInfo>& friends, const std::string& error);
    void RenderFriendRequests(const std::vector<FriendRequestInfo>& requests, const std::string& error);
    void UpdateNewFriendsEntry(int count);
    void HandleApiUnauthorized(const std::string& error);

    // 会话与聊天
    void SelectPeer(uint64_t peerId);
    const FriendInfo* FindFriend(uint64_t peerId) const;
    std::string PeerDisplayName(uint64_t peerId) const;
    void RequestHistory(uint64_t peerId, uint64_t beforeSeq);
    void RenderPeerMessages();
    void AppendChatBubble(bool fromSelf, const std::string& sender, const std::string& text);
    ui::Label* AppendChatBubbleWithLabel(bool fromSelf, const std::string& sender, const std::string& text);
    void SendCurrentMessage();
    void EnsureConversationRow(uint64_t peerId, const std::string& text);
    void RenderConversationList();
    void MarkPeerRead(uint64_t peerId);

    // 好友操作面板
    void ShowFriendOpPanel(uint64_t friendId);
    void HideOverlayPanels();

    /** 单聊会话本地状态
    */
    struct PeerState
    {
        struct Item {
            bool fromSelf = false;
            std::string sender;
            std::string text;
            uint64_t conversation_id = 0;
            uint64_t seq = 0;
            uint64_t ts = 0;
        };
        std::vector<Item> items;                                    // 按 seq 升序
        std::set<std::pair<uint64_t, uint64_t>> seen;               // (conversation_id, seq) 去重
        uint64_t minSeq = 0;                                        // 已加载最小 seq，用于翻页
        bool historyLoaded = false;
        int unread = 0;
        ui::Label* snippetLabel = nullptr;                          // 消息列表行摘要
        ui::Label* nameLabel = nullptr;
    };

    std::map<uint64_t, FriendInfo> m_friends;
    std::map<uint64_t, PeerState> m_peers;
    std::map<uint64_t, std::string> m_lastSnippet;                  // 会话最后一条消息摘要
    std::vector<uint64_t> m_conversationOrder;                      // 消息列表会话顺序（新消息置顶）
    std::vector<uint64_t> m_historyFifo;                            // 历史请求 FIFO（响应不带 peer 字段）
    uint64_t m_currentPeer = 0;
    uint64_t m_nextTempMsgId = 0;                                   // 本地临时消息号
    struct PendingSend {
        ui::Label* bubble = nullptr;
        uint64_t peerId = 0;
        std::string text;
    };
    std::vector<PendingSend> m_pendingSends;                        // 等待 SendAck/Error 的发送队列（FIFO）
    std::vector<FriendRequestInfo> m_incomingRequests;              // 待处理incoming申请
    std::map<uint64_t, std::string> m_outgoingRequestState;         // requestId -> 状态文案
    uint64_t m_opPanelFriendId = 0;                                 // 好友操作面板当前好友
#endif //CXIM_IMS_CORE_SDK_VERSION

private:
    std::shared_ptr<bool> m_alive;
#ifdef TENCENT_CLOUD_IM_SDK_VERSION
    std::string m_selfUserId;
    std::string m_currentFriendId;
    std::map<std::string, ui::Label*> m_conversationRows;
#endif
    DString m_themeDirectory;
    DString m_layoutXml;
    ui::Label* m_version_label = nullptr;
};

#endif //EXAMPLES_LAYOUTS_FORM_H_
