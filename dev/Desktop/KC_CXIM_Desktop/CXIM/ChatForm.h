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
    // 群聊分发
    void HandleGroupChatMessage(const ImsMessage& msg);
    void HandleGroupSendAck(const ImsMessage& msg);
    void HandleGroupHistoryResp(const ImsMessage& msg);
    void HandleGroupEvent(int type, const ImsMessage& msg);

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

    // HTTP 业务（后台线程执行 work，UI 线程执行 done）
    void RunAsync(std::function<void()> work, std::function<void()> done);
    void PostToUi(std::function<void()> task);
    void RefreshFriendList();
    void RefreshFriendRequests();
    void RenderFriendList(const std::vector<FriendInfo>& friends, const std::string& error);
    void RenderFriendRequests(const std::vector<FriendRequestInfo>& requests, const std::string& error);
    void UpdateNewFriendsEntry(int count);
    void HandleApiUnauthorized(const std::string& error);

    // ---- 群聊：会话与聊天 ----
    void SelectGroup(uint64_t groupId);
    const ImsGroupInfo* FindGroupInfo(uint64_t groupId) const;
    std::string GroupDisplayName(uint64_t groupId) const;
    std::string GroupMemberName(uint64_t groupId, uint64_t userId) const;
    void RequestGroupHistory(uint64_t groupId, uint64_t beforeSeq);
    void RenderGroupMessages(uint64_t groupId);
    void SendCurrentGroupMessage(uint64_t groupId, const std::string& text);
    void MarkGroupRead(uint64_t groupId);
    void RefreshGroupList();                       // GET /api/groups 全量对齐
    void RenderGroupList();                        // 群聊页签 group_list
    void RefreshGroupDetail(uint64_t groupId);     // GET /api/groups/{id}
    void UpdateGroupAnnouncementBar();

    // 群聊 UI 面板
    void ShowCreateGroupDialog();
    void ShowGroupMemberPanel(uint64_t groupId);
    void ShowGroupOpMenu(uint64_t groupId);
    void RenderGroupMembers(const std::vector<ImsGroupMember>& members);
    void ShowJoinRequestPanel();
    void RenderJoinRequests(const std::vector<ImsGroupRequestInfo>& requests);
    void UpdateGroupRequestRedDot();
    void RefreshGroupRequests();  // GET /api/groups/requests?direction=incoming 计算红点
    void AppendGroupBubble(bool fromSelf, const std::string& senderName, const std::string& text, ui::Label** outLabel);
    void EnsureConversationGroupRow(uint64_t groupId, const std::string& text);
    void DoCreateGroup();
    void ShowInviteDialog(uint64_t groupId);
    void DoInviteSelected(uint64_t groupId);
    void OnCreateDialogConfirm();
    void RenderCreateGroupFriends();
    void RenderCreateGroupFriends(const std::vector<FriendInfo>& friends);

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

    /** 群聊会话本地状态（结构对齐 PeerState，气泡带发送者昵称）
    */
    struct GroupState
    {
        struct Item {
            bool fromSelf = false;
            uint64_t senderId = 0;
            std::string sender;
            std::string text;
            uint64_t conversation_id = 0;
            uint64_t seq = 0;
            uint64_t ts = 0;
        };
        ImsGroupInfo info;                                          // 群资料缓存（name/announcement/memberCount/myRole/status/conversationId）
        std::vector<Item> items;
        std::set<std::pair<uint64_t, uint64_t>> seen;
        std::map<uint64_t, std::string> memberNames;                // userId -> 展示名快照
        uint64_t minSeq = 0;
        bool historyLoaded = false;
        bool removed = false;                                       // 我已被移出/群解散 → 停止收发展示
        int unread = 0;
    };

    std::map<uint64_t, FriendInfo> m_friends;
    std::map<uint64_t, PeerState> m_peers;
    std::map<uint64_t, GroupState> m_groups;                       // groupId -> 群会话
    std::vector<ImsGroupInfo> m_groupList;                         // 我的群列表快照
    std::set<uint64_t> m_groupRequestGroups;                        // 有 143 pending 待审批的群（红点）
    std::vector<std::string> m_conversationOrder;                   // 消息列表会话顺序（新消息置顶，key=C:/G:）
    // 历史请求 FIFO：{is_group, id}，响应不带会话标识，需按发出顺序配对
    struct HistoryKey { bool isGroup = false; uint64_t id = 0; };
    std::vector<HistoryKey> m_historyFifo;
    uint64_t m_currentPeer = 0;
    uint64_t m_currentGroup = 0;                                    // 当前打开的群（与 m_currentPeer 互斥）
    uint64_t m_nextTempMsgId = 0;                                   // 本地临时消息号
    struct PendingSend {
        ui::Label* bubble = nullptr;
        uint64_t peerId = 0;
        uint64_t groupId = 0;
        bool isGroup = false;
        std::string text;
    };
    std::vector<PendingSend> m_pendingSends;                        // 等待 SendAck/Error 的发送队列（FIFO）
    std::vector<FriendRequestInfo> m_incomingRequests;              // 待处理incoming申请
    std::map<uint64_t, std::string> m_outgoingRequestState;         // requestId -> 状态文案
    uint64_t m_opPanelFriendId = 0;                                 // 好友操作面板当前好友
    uint64_t m_opPanelGroupId = 0;                                  // 群操作面板当前群
    uint64_t m_inviteTargetGroupId = 0;                             // 邀请模式目标群（0=建群模式）
    bool m_groupSubmitting = false;                                 // 建群/邀请提交防抖（避免重复创建）
    std::shared_ptr<std::set<uint64_t>> m_createGroupSelected;      // 建群/邀请多选好友集合
    std::map<std::string, std::string> m_lastSnippet;               // 会话摘要（key=C:/G:）

    static std::string ConvKey(bool isGroup, uint64_t id)
    {
        return (isGroup ? "G:" : "C:") + std::to_string(id);
    }
    // 群管理菜单点击动作
    void DoGroupMenuAction(uint64_t groupId, const std::string& action);
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
