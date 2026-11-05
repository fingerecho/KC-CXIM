#ifndef CXIM_TENCENT_IM_SERVICE_H_
#define CXIM_TENCENT_IM_SERVICE_H_

#include "Resource.h"

#ifdef TENCENT_CLOUD_IM_SDK_VERSION

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "spdlog/spdlog.h"

/** 腾讯云 IM 接口封装
*   所有接口均在 TENCENT_CLOUD_IM_SDK_VERSION 宏下启用
*/
class TencentImService
{
public:
    struct UserProfile
    {
        std::string userId;
        std::string nickName;
    };

    struct ChatMessage
    {
        bool fromSelf = false;
        std::string sender;
        std::string text;
        int64_t timestamp = 0;
    };

    using ResultCallback = std::function<void(int32_t code, const std::string& desc)>;
    using JsonCallback = std::function<void(int32_t code, const std::string& desc, const std::string& jsonParams)>;
    using MessageCallback = std::function<void(const std::string& jsonMessages)>;
    using FriendListCallback = std::function<void(int32_t code, const std::string& desc, const std::vector<std::string>& friendIds)>;
    using UserProfileCallback = std::function<void(int32_t code, const std::string& desc, const UserProfile& profile)>;
    using ChatMessageListCallback = std::function<void(int32_t code, const std::string& desc, const std::vector<ChatMessage>& messages)>;
    using ChatMessageCallback = std::function<void(const std::vector<ChatMessage>& messages)>;

public:
    static TencentImService& Instance();

    /** 初始化 SDK，使用 TIM_SDK_APPID 宏作为 SDKAppID */
    bool Initialize();

    /** 反初始化 SDK，释放 SDK 资源 */
    void Uninitialize();

    /** SDK 是否已初始化 */
    bool IsInitialized() const;

    /** 使用已生成的 UserSig 登录 */
    int Login(const std::string& userId, const std::string& userSig, ResultCallback callback = nullptr);

    /** 使用账号登录，内部使用 TIM_SDK_APPID/TIM_SDK_APPKEY 生成 UserSig */
    int LoginAccount(const std::string& userId, ResultCallback callback = nullptr, uint32_t expireSeconds = 7 * 24 * 60 * 60);

    /** 退出登录 */
    int Logout(ResultCallback callback = nullptr);

    /** 获取当前登录用户的资料 */
    int GetSelfProfile(UserProfileCallback callback = nullptr);

    /** 获取好友列表（返回好友的 UserID 列表） */
    int GetFriendList(FriendListCallback callback = nullptr);

    /** 搜索用户资料（用于添加好友前确认用户是否存在） */
    int SearchUser(const std::string& userId, JsonCallback callback = nullptr);

    /** 添加好友 */
    int AddFriend(const std::string& userId, const std::string& addWording,
                  const std::string& remark = std::string(), ResultCallback callback = nullptr);

    /** 给好友发送文本消息（C2C） */
    int SendTextMessage(const std::string& peerUserId, const std::string& text, ResultCallback callback = nullptr);

    /** 获取与指定好友的历史消息 */
    int GetMessageHistory(const std::string& peerUserId, ChatMessageListCallback callback = nullptr);

    /** 生成 UserSig（TLS 2.0 签名） */
    std::string GenerateUserSig(const std::string& userId, uint32_t expireSeconds = 7 * 24 * 60 * 60) const;

    /** 设置收到新消息的回调 */
    void SetNewMessageCallback(ChatMessageCallback callback);

    /** 设置收到好友申请的回调 */
    void SetFriendRequestCallback(MessageCallback callback);

    /** 设置好友添加成功的回调 */
    void SetFriendAddedCallback(MessageCallback callback);

    /** 获取当前配置的 SDKAppID */
    uint64_t AppId() const;

private:
    static void OnResultThunk(int32_t code, const char* desc, const char* jsonParams, const void* userData);
    static void OnJsonResultThunk(int32_t code, const char* desc, const char* jsonParams, const void* userData);
    static void OnFriendListThunk(int32_t code, const char* desc, const char* jsonParams, const void* userData);
    static void OnUserProfileThunk(int32_t code, const char* desc, const char* jsonParams, const void* userData);
    static void OnMessageHistoryThunk(int32_t code, const char* desc, const char* jsonParams, const void* userData);
    static void OnRecvNewMsgThunk(const char* jsonMessages, const void* userData);
    static void OnFriendRequestThunk(const char* jsonRequests, const void* userData);
    static void OnFriendAddedThunk(const char* jsonFriendIds, const void* userData);

private:
    TencentImService() = default;
    TencentImService(const TencentImService&) = delete;
    TencentImService& operator=(const TencentImService&) = delete;
    ~TencentImService() = default;

private:
    bool m_initialized = false;
    ChatMessageCallback m_newMessageCallback;
    MessageCallback m_friendRequestCallback;
    MessageCallback m_friendAddedCallback;
};

#endif // TENCENT_CLOUD_IM_SDK_VERSION

#endif // CXIM_TENCENT_IM_SERVICE_H_
