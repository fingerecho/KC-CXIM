# 腾讯云 IM Windows C++ SDK（ImSDK 8.9.7511）API 全量说明

> 说明：本目录是**客户端 C++ SDK**（`V2TIM*` 接口），不是服务端 REST API。所有接口都是**异步**的：调用后通过 `V2TIMCallback` / `V2TIMValueCallback<T>` / `V2TIMSendCallback` / `V2TIMDownloadCallback` / `V2TIMCompleteCallback<T>` 回调结果。下面按模块列出全部接口、参数与用法。

---

## 目录

1. [目录结构与接入方式](#一目录结构与接入方式)
2. [通用回调与基础类型](#二通用回调与基础类型)
3. [V2TIMManager（核心入口）](#三v2timmmanager核心入口)
4. [V2TIMMessageManager（高级消息）](#四v2timmessagemanager高级消息)
5. [V2TIMGroupManager（群高级）](#五v2timgroupmanager群高级)
6. [V2TIMConversationManager（会话）](#六v2timconversationmanager会话)
7. [V2TIMFriendshipManager（关系链）](#七v2timfriendshipmanager关系链)
8. [V2TIMCommunityManager（社群/权限组）](#八v2timcommunitymanager社群权限组)
9. [V2TIMSignalingManager（信令）](#九v2timsignalingmanager信令)
10. [V2TIMOfflinePushManager（离线推送）](#十v2timofflinepushmanager离线推送)
11. [事件监听器（回调）](#十一事件监听器回调)
12. [关键数据结构](#十二关键数据结构参数必填字段)
13. [常用错误码](#十三常用错误码)
14. [C++ 完整示例](#十四c-完整示例)
15. [使用建议](#十五使用建议)

---

## 一、目录结构与接入方式

```text
sdk/ImSDK_Windows_8.9.7511/
├── cpp_include/        C++ 头文件（本文档依据）
├── c_include/          C 接口头文件（TIMxxx 系列）
└── shared_lib/
    ├── Win32/ImSDK.dll + ImSDK.lib
    └── Win64/ImSDK.dll + ImSDK.lib
```

接入步骤：

1. 工程包含 `cpp_include`，链接对应平台 `ImSDK.lib`；
2. 运行时把 `ImSDK.dll` 放到可执行文件目录；
3. 头文件默认 `TIM_API = __declspec(dllimport)`，普通业务工程**不要**定义 `TIM_EXPORT`。

---

## 二、通用回调与基础类型

```cpp
// 无返回值的成功/失败回调
class V2TIMCallback {
    virtual void OnSuccess() = 0;
    virtual void OnError(int error_code, const V2TIMString& error_message) = 0;
};

// 带返回值 T 的回调
template<class T> class V2TIMValueCallback {
    virtual void OnSuccess(const T& value) = 0;
    virtual void OnError(int error_code, const V2TIMString& error_message) = 0;
};

// 发送消息回调（带上传进度）
class V2TIMSendCallback : public V2TIMValueCallback<V2TIMMessage> {
    virtual void OnProgress(uint32_t progress) = 0;   // 0~100
};

// 下载回调（带下载进度）
class V2TIMDownloadCallback : public V2TIMCallback {
    virtual void OnDownLoadProgress(uint64_t currentSize, uint64_t totalSize) = 0;
};

// 完成回调（成功/失败都带 T）
template<class T> class V2TIMCompleteCallback {
    virtual void OnComplete(int error_code, const V2TIMString& error_message, const T& value) = 0;
};
```

注意点：

- `V2TIMString` 是 SDK 自定义字符串，用 `CString()` 取 `const char*`，`Size()/Length()` 取长度；
- `V2TIMBuffer` 是二进制缓冲，构造 `V2TIMBuffer(const uint8_t* data, size_t size)`，`Data()/Size()` 读取；
- 所有 `V2TIMXxxVector` / `V2TIMXxxMap` 都是 SDK 封装的容器，支持 `PushBack / Size / operator[] / Empty / Clear / Erase`（Map 支持 `Insert / Get / Count / AllKeys`）；
- 回调对象通常按官方示例 `new` 出来后传入，回调触发后由 SDK 释放；监听器（Listener）需由业务侧持有生命周期，不用时调用 `RemoveXxxListener`。

---

## 三、V2TIMManager（核心入口）

头文件：`V2TIMManager.h`，单例：`V2TIMManager::GetInstance()`。

### 1. 初始化与监听

| 接口 | 参数 | 说明 |
|---|---|---|
| `bool InitSDK(uint32_t sdkAppID, const V2TIMSDKConfig& config)` | sdkAppID：控制台应用 ID；config：SDK 配置 | 同步返回是否初始化成功 |
| `void UnInitSDK()` | 无 | 反初始化 |
| `void AddSDKListener(V2TIMSDKListener*)` | 监听器对象 | 连接/被踢/票据过期/资料变更等 |
| `void RemoveSDKListener(V2TIMSDKListener*)` | 监听器对象 | 移除监听 |
| `V2TIMString GetVersion()` | 无 | SDK 版本 |
| `int64_t GetServerTime()` | 无 | 服务器 UTC 时间（秒） |

`V2TIMSDKConfig` 字段：`initPath`（数据目录）、`logPath`（日志目录）、`logLevel`（`V2TIMLogLevel`）、`logListener`。

### 2. 登录/登出

| 接口 | 参数 | 说明 |
|---|---|---|
| `void Login(const V2TIMString& userID, const V2TIMString& userSig, V2TIMCallback*)` | userID、userSig（后台签发） | 票据过期回调 6206 |
| `void Logout(V2TIMCallback*)` | 回调 | 切换账号需等登出回调后再登录 |
| `V2TIMString GetLoginUser()` | 无 | 当前登录用户 |
| `V2TIMLoginStatus GetLoginStatus()` | 无 | `LOGINED/LOGINING/LOGOUT` |

### 3. 简单消息（文本 / 自定义）

| 接口 | 参数 | 说明 |
|---|---|---|
| `AddSimpleMsgListener / RemoveSimpleMsgListener` | `V2TIMSimpleMsgListener*` | 简单消息监听 |
| `V2TIMString SendC2CTextMessage(text, userID, V2TIMSendCallback*)` | 文本、对方 ID | 最大 12KB，支持敏感词过滤 |
| `V2TIMString SendC2CCustomMessage(const V2TIMBuffer& data, userID, V2TIMSendCallback*)` | 二进制 | 不支持敏感词过滤，默认不推送 |
| `V2TIMString SendGroupTextMessage(text, groupID, V2TIMMessagePriority, V2TIMSendCallback*)` | 优先级 `DEFAULT/HIGH/NORMAL/LOW` | 群文本 |
| `V2TIMString SendGroupCustomMessage(data, groupID, priority, V2TIMSendCallback*)` | 二进制、群 ID、优先级 | 群自定义 |

### 4. 群基础操作

| 接口 | 参数 | 说明 |
|---|---|---|
| `AddGroupListener / RemoveGroupListener` | `V2TIMGroupListener*` | 群事件监听 |
| `CreateGroup(groupType, groupID, groupName, V2TIMValueCallback<V2TIMString>*)` | 类型：`Work/Public/Meeting/Community/AVChatRoom`；groupID 可空 | 返回群 ID |
| `JoinGroup(groupID, message, V2TIMCallback*)` | message：申请附言 | Work 群不能主动加入 |
| `QuitGroup(groupID, V2TIMCallback*)` | 群 ID | 群主不能退公开/会议/直播群 |
| `DismissGroup(groupID, V2TIMCallback*)` | 群 ID | 解散群 |

### 5. 用户资料 / 状态

| 接口 | 参数 | 说明 |
|---|---|---|
| `GetUsersInfo(const V2TIMStringVector& userIDList, V2TIMValueCallback<V2TIMUserFullInfoVector>*)` | 建议 ≤100 个 | 拉取用户资料 |
| `SetSelfInfo(const V2TIMUserFullInfo& info, V2TIMCallback*)` | 需设 `modifyFlag` | 修改自己资料 |
| `SubscribeUserInfo(userIDList, V2TIMCallback*)` | ≤200 个 | 7.4+，订阅陌生人资料 |
| `UnsubscribeUserInfo(userIDList, V2TIMCallback*)` | 空则全部取消 | 7.4+ |
| `SearchUsers(const V2TIMUserSearchParam&, V2TIMValueCallback<V2TIMUserSearchResult>*)` | 关键字/性别/生日/分页 | 8.4+，云端搜索 |
| `GetUserStatus(userIDList, V2TIMValueCallback<V2TIMUserStatusVector>*)` | 用户 ID 列表 | 6.3+，查询在线/自定义状态 |
| `SetSelfStatus(const V2TIMUserStatus&, V2TIMCallback*)` | customStatus ≤100 字节 | 设置自己状态 |
| `SubscribeUserStatus(userIDList, V2TIMCallback*)` | ≤200 个 | 6.3+ |
| `UnsubscribeUserStatus(userIDList, V2TIMCallback*)` | 空则全部取消 | 6.3+ |

### 6. 管理器入口与扩展

| 接口 | 返回 |
|---|---|
| `GetMessageManager()` | `V2TIMMessageManager*` |
| `GetGroupManager()` | `V2TIMGroupManager*` |
| `GetCommunityManager()` | `V2TIMCommunityManager*` |
| `GetConversationManager()` | `V2TIMConversationManager*` |
| `GetFriendshipManager()` | `V2TIMFriendshipManager*` |
| `GetOfflinePushManager()` | `V2TIMOfflinePushManager*` |
| `GetSignalingManager()` | `V2TIMSignalingManager*` |
| `void* CallExperimentalAPI(api, param, V2TIMValueCallback<V2TIMBaseObject>*)` | 实验性接口 |

---

## 四、V2TIMMessageManager（高级消息）

头文件：`V2TIMMessageManager.h`，通过 `GetMessageManager()` 获取。

### 1. 监听

`AddAdvancedMsgListener(V2TIMAdvancedMsgListener*)` / `RemoveAdvancedMsgListener(...)`

### 2. 创建消息（返回 `V2TIMMessage`）

| 接口 | 参数 |
|---|---|
| `CreateTextMessage(text)` | 文本 |
| `CreateTextAtMessage(text, atUserList)` | @列表，`kImSDK_MesssageAtALL` 表示 @所有人（已弃用，推荐下一个） |
| `CreateCustomMessage(data)` | 自定义二进制 |
| `CreateCustomMessage(data, description, extension)` | 描述/扩展，用于离线 Push 展示 |
| `CreateImageMessage(imagePath)` | 图片路径，≤28MB |
| `CreateSoundMessage(soundPath, duration)` | 语音路径、时长（秒），≤28MB |
| `CreateVideoMessage(videoFilePath, type, duration, snapshotPath)` | 类型如 mp4、时长、封面，≤100MB |
| `CreateFileMessage(filePath, fileName)` | 文件路径、显示名，≤100MB |
| `CreateLocationMessage(desc, longitude, latitude)` | 描述、经度、纬度 |
| `CreateFaceMessage(index, data)` | 表情索引、自定义数据 |
| `CreateMergerMessage(messageList, title, abstractList, compatibleText)` | ≤300 条、≤5 条摘要、兼容文本 |
| `CreateForwardMessage(message)` | 转发消息（原消息须发送成功） |
| `CreateTargetedGroupMessage(message, receiverList)` | 定向群消息，≤50 人，6.0+ |
| `CreateAtSignedGroupMessage(message, atUserList)` | 带 @ 的群消息，7.0+ |

### 3. 发送消息

```cpp
V2TIMString SendMessage(
    V2TIMMessage& message,                 // 已创建的消息
    const V2TIMString& receiver,           // C2C 对方 userID
    const V2TIMString& groupID,            // 群聊 groupID
    V2TIMMessagePriority priority,         // 群消息优先级
    bool onlineUserOnly,                   // 仅在线用户可收
    const V2TIMOfflinePushInfo& offlinePushInfo,
    V2TIMSendCallback* callback);
```

返回消息唯一 ID。C2C 只传 receiver，群聊只传 groupID。

### 4. 消息接收选项

| 接口 | 参数 |
|---|---|
| `SetC2CReceiveMessageOpt(userIDList, opt, V2TIMCallback*)` | 批量 ≤30，1 秒 ≤5 次 |
| `GetC2CReceiveMessageOpt(userIDList, V2TIMValueCallback<V2TIMReceiveMessageOptInfoVector>*)` | 查询选项 |
| `SetGroupReceiveMessageOpt(groupID, opt, V2TIMCallback*)` | 群消息免打扰 |
| `SetAllReceiveMessageOpt(opt, startHour, startMinute, startSecond, duration, cb)` | 全局免打扰（时分秒） |
| `SetAllReceiveMessageOpt(opt, startTimeStamp, duration, cb)` | 全局免打扰（时间戳） |
| `GetAllReceiveMessageOpt(V2TIMValueCallback<V2TIMReceiveMessageOptInfo>*)` | 获取全局选项 |

`V2TIMReceiveMessageOpt`：`RECEIVE_MESSAGE`、`NOT_RECEIVE_MESSAGE`、`RECEIVE_NOT_NOTIFY_MESSAGE`、`RECEIVE_NOT_NOTIFY_MESSAGE_EXCEPT_AT`、`NOT_RECEIVE_MESSAGE_EXCEPT_AT`。

### 5. 历史消息 / 撤回 / 删除 / 搜索

| 接口 | 参数 |
|---|---|
| `GetHistoryMessageList(const V2TIMMessageListGetOption&, V2TIMValueCallback<V2TIMMessageVector>*)` | 见下方 Option |
| `RevokeMessage(const V2TIMMessage&, V2TIMCallback*)` | 默认 2 分钟内 |
| `ModifyMessage(const V2TIMMessage&, V2TIMCompleteCallback<V2TIMMessage>*)` | 修改消息 |
| `DeleteMessages(const V2TIMMessageVector&, V2TIMCallback*)` | ≤50 条且同会话，1 秒 1 次 |
| `ClearC2CHistoryMessage(userID, V2TIMCallback*)` | 清空单聊本地+云端 |
| `ClearGroupHistoryMessage(groupID, V2TIMCallback*)` | 清空群聊本地+云端 |
| `InsertGroupMessageToLocalStorage(message, groupID, sender, V2TIMValueCallback<V2TIMMessage>*)` | 本地插入群提示消息 |
| `InsertC2CMessageToLocalStorage(message, userID, sender, V2TIMValueCallback<V2TIMMessage>*)` | 本地插入单聊提示消息 |
| `FindMessages(messageIDList, V2TIMValueCallback<V2TIMMessageVector>*)` | 按 ID 查本地消息 |
| `SearchLocalMessages(param, V2TIMValueCallback<V2TIMMessageSearchResult>*)` | 本地搜索，旗舰版 |
| `SearchCloudMessages(param, V2TIMValueCallback<V2TIMMessageSearchResult>*)` | 云端搜索，7.3+ |

`V2TIMMessageListGetOption` 主要字段：`getType`（`GET_CLOUD_OLDER_MSG`/`GET_CLOUD_NEWER_MSG`/`GET_LOCAL_OLDER_MSG`/`GET_LOCAL_NEWER_MSG`）、`userID`、`groupID`、`count`、`messageTypeList`、`lastMsg`、`lastMsgSeq`、`getTimeBegin`、`getTimePeriod`、`messageSeqList`。

### 6. 已读回执

| 接口 | 参数 |
|---|---|
| `SendMessageReadReceipts(messageList, V2TIMCallback*)` | 同会话消息 |
| `GetMessageReadReceipts(messageList, V2TIMValueCallback<V2TIMMessageReceiptVector>*)` | 获取回执 |
| `GetGroupMessageReadMemberList(message, filter, nextSeq, count, V2TIMValueCallback<V2TIMGroupMessageReadMemberList>*)` | filter：已读/未读；count ≤100 |

### 7. 消息扩展 / 回应 / 翻译 / 置顶

| 接口 | 参数 |
|---|---|
| `SetMessageExtensions(message, extensions, V2TIMValueCallback<V2TIMMessageExtensionResultVector>*)` | 单次 ≤20，单条 ≤300 |
| `GetMessageExtensions(message, V2TIMValueCallback<V2TIMMessageExtensionVector>*)` | 获取扩展 |
| `DeleteMessageExtensions(message, keys, V2TIMValueCallback<V2TIMMessageExtensionResultVector>*)` | keys 空表示全部删除 |
| `AddMessageReaction(message, reactionID, V2TIMCallback*)` | 表情回应，单条 ≤10 种 |
| `RemoveMessageReaction(message, reactionID, V2TIMCallback*)` | 删除回应 |
| `GetMessageReactions(messageList, maxUserCountPerReaction, V2TIMValueCallback<V2TIMMessageReactionResultVector>*)` | ≤20 条，maxUser 0~10 |
| `GetAllUserListOfMessageReaction(message, reactionID, nextSeq, count, V2TIMValueCallback<V2TIMMessageReactionUserResult>*)` | count ≤100 |
| `TranslateText(sourceTextList, sourceLanguage, targetLanguage, V2TIMValueCallback<V2TIMStringToV2TIMStringMap>*)` | auto/语言码 |
| `PinGroupMessage(groupID, message, isPinned, V2TIMCallback*)` | 7.9+，≤10 条 |
| `GetPinnedGroupMessageList(groupID, V2TIMValueCallback<V2TIMMessageVector>*)` | 获取置顶列表 |

### 8. 会话已读（旧接口）

`MarkC2CMessageAsRead(userID, cb)`、`MarkGroupMessageAsRead(groupID, cb)`、`MarkAllMessageAsRead(cb)`（建议改用 `CleanConversationUnreadMessageCount`）。

---

## 五、V2TIMGroupManager（群高级）

头文件：`V2TIMGroupManager.h`。

### 1. 群资料 / 属性 / 计数器

| 接口 | 参数 |
|---|---|
| `CreateGroup(const V2TIMGroupInfo& info, const V2TIMCreateGroupMemberInfoVector& memberList, V2TIMValueCallback<V2TIMString>*)` | 可指定初始成员，≤20 |
| `GetJoinedGroupList(V2TIMValueCallback<V2TIMGroupInfoVector>*)` | 已加入群列表 |
| `GetGroupsInfo(groupIDList, V2TIMValueCallback<V2TIMGroupInfoResultVector>*)` | 拉取群资料 |
| `SearchGroups(const V2TIMGroupSearchParam&, V2TIMValueCallback<V2TIMGroupInfoVector>*)` | 本地搜索 |
| `SearchCloudGroups(const V2TIMGroupSearchParam&, V2TIMValueCallback<V2TIMGroupSearchResult>*)` | 云端搜索 8.4+ |
| `SetGroupInfo(const V2TIMGroupInfo&, V2TIMCallback*)` | 需设 `modifyFlag` |
| `InitGroupAttributes(groupID, const V2TIMGroupAttributeMap&, cb)` | 重置全部属性 |
| `SetGroupAttributes(groupID, attributes, cb)` | 增/改 |
| `DeleteGroupAttributes(groupID, keys, cb)` | keys 空=清空 |
| `GetGroupAttributes(groupID, keys, V2TIMValueCallback<V2TIMGroupAttributeMap>*)` | keys 空=全部 |
| `GetGroupOnlineMemberCount(groupID, V2TIMValueCallback<uint32_t>*)` | 获取在线人数 |
| `SetGroupCounters(groupID, const V2TIMStringToInt64Map&, V2TIMValueCallback<V2TIMStringToInt64Map>*)` | 7.0+ |
| `GetGroupCounters(groupID, keys, V2TIMValueCallback<V2TIMStringToInt64Map>*)` | 获取计数器 |
| `IncreaseGroupCounter(groupID, key, value, V2TIMValueCallback<V2TIMStringToInt64Map>*)` | 递增 |
| `DecreaseGroupCounter(groupID, key, value, V2TIMValueCallback<V2TIMStringToInt64Map>*)` | 递减 |

### 2. 群成员

| 接口 | 参数 |
|---|---|
| `GetGroupMemberList(groupID, filter, nextSeq, V2TIMValueCallback<V2TIMGroupMemberInfoResult>*)` | filter：`ALL/OWNER/ADMIN/COMMON` |
| `GetGroupMembersInfo(groupID, memberList, V2TIMValueCallback<V2TIMGroupMemberFullInfoVector>*)` | 指定成员资料 |
| `SearchGroupMembers(param, V2TIMValueCallback<V2TIMGroupSearchGroupMembersMap>*)` | 本地搜索 |
| `SearchCloudGroupMembers(param, V2TIMValueCallback<V2TIMGroupMemberSearchResult>*)` | 8.4+ |
| `SetGroupMemberInfo(groupID, info, V2TIMCallback*)` | 需设 `modifyFlag` |
| `MuteGroupMember(groupID, userID, seconds, V2TIMCallback*)` | 禁言 |
| `MuteAllGroupMembers(groupID, isMute, V2TIMCallback*)` | 7.5+ |
| `InviteUserToGroup(groupID, userList, V2TIMValueCallback<V2TIMGroupMemberOperationResultVector>*)` | ≤20 |
| `KickGroupMember(groupID, memberList, reason, duration, V2TIMValueCallback<V2TIMGroupMemberOperationResultVector>*)` | duration：禁止再申请时长 |
| `SetGroupMemberRole(groupID, userID, role, V2TIMCallback*)` | 200/300 |
| `MarkGroupMemberList(groupID, memberList, markType, enableMark, V2TIMCallback*)` | 旗舰版 |
| `TransferGroupOwner(groupID, userID, V2TIMCallback*)` | 转让群主 |

### 3. 加群申请

| 接口 | 参数 |
|---|---|
| `GetGroupApplicationList(V2TIMValueCallback<V2TIMGroupApplicationResult>*)` | ≤50 |
| `AcceptGroupApplication(application, reason, V2TIMCallback*)` | 同意 |
| `RefuseGroupApplication(application, reason, V2TIMCallback*)` | 拒绝 |
| `SetGroupApplicationRead(V2TIMCallback*)` | 标记已读 |

### 4. 社群话题（GroupManager 中也有一份）

`GetJoinedCommunityList`、`CreateTopicInCommunity(groupID, topicInfo, cb)`、`DeleteTopicFromCommunity(groupID, topicIDList, cb)`、`SetTopicInfo(topicInfo, cb)`、`GetTopicInfoList(groupID, topicIDList, cb)`。

---

## 六、V2TIMConversationManager（会话）

头文件：`V2TIMConversationManager.h`。会话 ID 规则：单聊 `c2c_用户ID`，群聊 `group_群ID`。

| 接口 | 参数 |
|---|---|
| `AddConversationListener / RemoveConversationListener` | `V2TIMConversationListener*` |
| `GetConversationList(nextSeq, count, V2TIMValueCallback<V2TIMConversationResult>*)` | 分页建议 100 |
| `GetConversation(conversationID, V2TIMValueCallback<V2TIMConversation>*)` | 单个会话 |
| `GetConversationList(const V2TIMStringVector& conversationIDList, V2TIMValueCallback<V2TIMConversationVector>*)` | 指定多个 |
| `GetConversationListByFilter(filter, nextSeq, count, V2TIMValueCallback<V2TIMConversationResult>*)` | 6.5+ |
| `DeleteConversation(conversationID, V2TIMCallback*)` | 本地+服务端删除 |
| `DeleteConversationList(conversationIDList, bool clearMessage, V2TIMValueCallback<V2TIMConversationOperationResultVector>*)` | ≤100，7.1+ |
| `SetConversationDraft(conversationID, draftText, V2TIMCallback*)` | 仅本地 |
| `SetConversationCustomData(conversationIDList, customData, V2TIMValueCallback<V2TIMConversationOperationResultVector>*)` | ≤256 bytes |
| `PinConversation(conversationID, isPinned, V2TIMCallback*)` | 置顶 |
| `MarkConversation(conversationIDList, markType, enableMark, V2TIMValueCallback<V2TIMConversationOperationResultVector>*)` | 6.5+ |
| `GetTotalUnreadMessageCount(V2TIMValueCallback<uint64_t>*)` | 全部未读数 |
| `GetUnreadMessageCountByFilter(filter, V2TIMValueCallback<uint64_t>*)` | 7.0+ |
| `SubscribeUnreadMessageCountByFilter(filter)` | 注册监听 |
| `UnsubscribeUnreadMessageCountByFilter(filter)` | 取消监听 |
| `CleanConversationUnreadMessageCount(conversationID, cleanTimestamp, cleanSequence, V2TIMCallback*)` | 清未读，7.1+ |
| `CreateConversationGroup(groupName, conversationIDList, V2TIMValueCallback<V2TIMConversationOperationResultVector>*)` | ≤20 分组 |
| `GetConversationGroupList(V2TIMValueCallback<V2TIMStringVector>*)` | 获取分组 |
| `DeleteConversationGroup(groupName, V2TIMCallback*)` | 删除分组 |
| `RenameConversationGroup(oldName, newName, V2TIMCallback*)` | 重命名 |
| `AddConversationsToGroup(groupName, conversationIDList, V2TIMValueCallback<V2TIMConversationOperationResultVector>*)` | 添加会话到分组 |
| `DeleteConversationsFromGroup(groupName, conversationIDList, V2TIMValueCallback<V2TIMConversationOperationResultVector>*)` | 从分组移除会话 |

`V2TIMConversationListFilter` 字段：`type`（C2C/GROUP）、`filterType`（位或：会话标记/分组/未读数/群@）、`conversationGroup`、`markType`、`hasUnreadCount`、`hasGroupAtInfo`。

---

## 七、V2TIMFriendshipManager（关系链）

头文件：`V2TIMFriendshipManager.h`。

### 1. 好友

| 接口 | 参数 |
|---|---|
| `AddFriendListener / RemoveFriendListener` | `V2TIMFriendshipListener*` |
| `GetFriendList(V2TIMValueCallback<V2TIMFriendInfoVector>*)` | 好友列表 |
| `GetFriendsInfo(userIDList, V2TIMValueCallback<V2TIMFriendInfoResultVector>*)` | ≤100 |
| `SetFriendInfo(const V2TIMFriendInfo&, V2TIMCallback*)` | 备注/自定义字段，需 `modifyFlag` |
| `SearchFriends(param, V2TIMValueCallback<V2TIMFriendInfoResultVector>*)` | 本地搜索，旗舰版 |
| `AddFriend(const V2TIMFriendAddApplication&, V2TIMValueCallback<V2TIMFriendOperationResult>*)` | 见结构 |
| `DeleteFromFriendList(userIDList, deleteType, V2TIMValueCallback<V2TIMFriendOperationResultVector>*)` | SINGLE/BOTH |
| `CheckFriend(userIDList, checkType, V2TIMValueCallback<V2TIMFriendCheckResultVector>*)` | 检查关系 |

### 2. 好友申请

| 接口 | 参数 |
|---|---|
| `GetFriendApplicationList(V2TIMValueCallback<V2TIMFriendApplicationResult>*)` | ≤100 |
| `AcceptFriendApplication(application, acceptType, V2TIMValueCallback<V2TIMFriendOperationResult>*)` | AGREE / AGREE_AND_ADD |
| `AcceptFriendApplication(application, acceptType, remark, V2TIMValueCallback<V2TIMFriendOperationResult>*)` | 带备注，≤96 字节 |
| `RefuseFriendApplication(application, V2TIMValueCallback<V2TIMFriendOperationResult>*)` | 拒绝 |
| `DeleteFriendApplication(application, V2TIMCallback*)` | 删除申请 |
| `SetFriendApplicationRead(V2TIMCallback*)` | 标记已读 |

### 3. 黑名单

| 接口 | 参数 |
|---|---|
| `AddToBlackList(userIDList, V2TIMValueCallback<V2TIMFriendOperationResultVector>*)` | 加入黑名单 |
| `DeleteFromBlackList(userIDList, V2TIMValueCallback<V2TIMFriendOperationResultVector>*)` | 移出黑名单 |
| `GetBlackList(V2TIMValueCallback<V2TIMFriendInfoVector>*)` | 获取黑名单 |

### 4. 好友分组

| 接口 | 参数 |
|---|---|
| `CreateFriendGroup(groupName, userIDList, V2TIMValueCallback<V2TIMFriendOperationResultVector>*)` | 新建分组 |
| `GetFriendGroups(groupNameList, V2TIMValueCallback<V2TIMFriendGroupVector>*)` | 空=全部 |
| `DeleteFriendGroup(groupNameList, V2TIMCallback*)` | 删除分组 |
| `RenameFriendGroup(oldName, newName, V2TIMCallback*)` | 重命名 |
| `AddFriendsToFriendGroup(groupName, userIDList, V2TIMValueCallback<V2TIMFriendOperationResultVector>*)` | 加好友到分组 |
| `DeleteFriendsFromFriendGroup(groupName, userIDList, V2TIMValueCallback<V2TIMFriendOperationResultVector>*)` | 从分组删除好友 |

### 5. 公众号（7.6+）

| 接口 | 参数 |
|---|---|
| `SubscribeOfficialAccount(officialAccountID, V2TIMCallback*)` | 订阅公众号 |
| `UnsubscribeOfficialAccount(officialAccountID, V2TIMCallback*)` | 取消订阅 |
| `GetOfficialAccountsInfo(officialAccountIDList, V2TIMValueCallback<V2TIMOfficialAccountInfoResultVector>*)` | 空=已订阅列表 |

### 6. 关注/粉丝（7.8+）

| 接口 | 参数 |
|---|---|
| `FollowUser(userIDList, V2TIMValueCallback<V2TIMFollowOperationResultVector>*)` | ≤20 |
| `UnfollowUser(userIDList, V2TIMValueCallback<V2TIMFollowOperationResultVector>*)` | ≤20 |
| `GetMyFollowingList(nextCursor, V2TIMValueCallback<V2TIMUserInfoResult>*)` | 分页游标 |
| `GetMyFollowersList(nextCursor, V2TIMValueCallback<V2TIMUserInfoResult>*)` | 粉丝列表 |
| `GetMutualFollowersList(nextCursor, V2TIMValueCallback<V2TIMUserInfoResult>*)` | 互关 |
| `GetUserFollowInfo(userIDList, V2TIMValueCallback<V2TIMFollowInfoVector>*)` | 数量信息 |
| `CheckFollowType(userIDList, V2TIMValueCallback<V2TIMFollowTypeCheckResultVector>*)` | 关注类型 |

---

## 八、V2TIMCommunityManager（社群/权限组）

头文件：`V2TIMCommunityManager.h`。

### 1. 监听与社群话题

| 接口 | 参数 |
|---|---|
| `AddCommunityListener / RemoveCommunityListener` | `V2TIMCommunityListener*` |
| `CreateCommunity(const V2TIMGroupInfo&, const V2TIMCreateGroupMemberInfoVector&, V2TIMValueCallback<V2TIMString>*)` | 创建支持话题的社群 |
| `GetJoinedCommunityList(V2TIMValueCallback<V2TIMGroupInfoVector>*)` | 已加入社群 |
| `CreateTopicInCommunity(groupID, topicInfo, V2TIMValueCallback<V2TIMString>*)` | groupID 须 `@TGS#_` 开头 |
| `DeleteTopicFromCommunity(groupID, topicIDList, V2TIMValueCallback<V2TIMTopicOperationResultVector>*)` | 删除话题 |
| `SetTopicInfo(topicInfo, V2TIMCallback*)` | 需设 `modifyFlag` |
| `GetTopicInfoList(groupID, topicIDList, V2TIMValueCallback<V2TIMTopicInfoResultVector>*)` | 空=全部话题 |

### 2. 权限组（7.8+，旗舰版）

| 接口 | 参数 |
|---|---|
| `CreatePermissionGroupInCommunity(info, V2TIMValueCallback<V2TIMString>*)` | 创建权限组 |
| `DeletePermissionGroupFromCommunity(groupID, permissionGroupIDList, V2TIMValueCallback<V2TIMPermissionGroupOperationResultVector>*)` | 删除权限组 |
| `ModifyPermissionGroupInfoInCommunity(info, V2TIMCallback*)` | 修改权限组 |
| `GetJoinedPermissionGroupListInCommunity(groupID, V2TIMValueCallback<V2TIMPermissionGroupInfoResultVector>*)` | 已加入权限组 |
| `GetPermissionGroupListInCommunity(groupID, permissionGroupIDList, V2TIMValueCallback<V2TIMPermissionGroupInfoResultVector>*)` | 空=全部 |
| `AddCommunityMembersToPermissionGroup(groupID, permissionGroupID, memberList, V2TIMValueCallback<V2TIMPermissionGroupMemberOperationResultVector>*)` | 添加成员 |
| `RemoveCommunityMembersFromPermissionGroup(groupID, permissionGroupID, memberList, V2TIMValueCallback<V2TIMPermissionGroupMemberOperationResultVector>*)` | 删除成员 |
| `GetCommunityMemberListInPermissionGroup(groupID, permissionGroupID, nextCursor, V2TIMValueCallback<V2TIMPermissionGroupMemberInfoResult>*)` | 成员列表 |
| `AddTopicPermissionToPermissionGroup(groupID, permissionGroupID, const V2TIMStringToUint64Map&, V2TIMValueCallback<V2TIMTopicOperationResultVector>*)` | 添加话题权限 |
| `DeleteTopicPermissionFromPermissionGroup(groupID, permissionGroupID, topicIDList, V2TIMValueCallback<V2TIMTopicOperationResultVector>*)` | 删除话题权限 |
| `ModifyTopicPermissionInPermissionGroup(groupID, permissionGroupID, topicPermissionMap, V2TIMValueCallback<V2TIMTopicOperationResultVector>*)` | 修改话题权限 |
| `GetTopicPermissionInPermissionGroup(groupID, permissionGroupID, topicIDList, V2TIMValueCallback<V2TIMTopicPermissionResultVector>*)` | 获取话题权限 |

---

## 九、V2TIMSignalingManager（信令）

头文件：`V2TIMSignalingManager.h`。

| 接口 | 参数 |
|---|---|
| `AddSignalingListener / RemoveSignalingListener` | `V2TIMSignalingListener*` |
| `V2TIMString Invite(invitee, data, onlineUserOnly, const V2TIMOfflinePushInfo&, int timeout, V2TIMCallback*)` | 单聊邀请，timeout 秒；0 不超时；返回 inviteID |
| `V2TIMString InviteInGroup(groupID, inviteeList, data, onlineUserOnly, timeout, V2TIMCallback*)` | 群内邀请 |
| `Cancel(inviteID, data, V2TIMCallback*)` | 邀请方取消 |
| `Accept(inviteID, data, V2TIMCallback*)` | 接收方接受 |
| `Reject(inviteID, data, V2TIMCallback*)` | 接收方拒绝 |
| `V2TIMSignalingInfo GetSignalingInfo(const V2TIMMessage&)` | 解析消息中的信令信息 |
| `AddInvitedSignaling(const V2TIMSignalingInfo&, V2TIMCallback*)` | 离线补发信令 |
| `ModifyInvitation(inviteID, data, V2TIMCallback*)` | 6.7+ 修改 data |

---

## 十、V2TIMOfflinePushManager（离线推送）

| 接口 | 参数 |
|---|---|
| `SetOfflinePushConfig(const V2TIMOfflinePushConfig&, V2TIMCallback*)` | token、businessID、token_type；传空配置关闭推送 |
| `DoBackground(uint32_t unreadCount, V2TIMCallback*)` | 退后台，设置角标未读数 |
| `DoForeground(V2TIMCallback*)` | 进前台，停止离线推送 |

---

## 十一、事件监听器（回调）

| 监听器 | 主要回调 |
|---|---|
| `V2TIMSDKListener` | `OnConnecting`、`OnConnectSuccess`、`OnConnectFailed`、`OnKickedOffline`、`OnUserSigExpired`、`OnSelfInfoUpdated`、`OnUserStatusChanged`、`OnUserInfoChanged`、`OnAllReceiveMessageOptChanged` |
| `V2TIMSimpleMsgListener` | `OnRecvC2CTextMessage`、`OnRecvC2CCustomMessage`、`OnRecvGroupTextMessage`、`OnRecvGroupCustomMessage` |
| `V2TIMAdvancedMsgListener` | `OnRecvNewMessage`、`OnRecvC2CReadReceipt`、`OnRecvMessageReadReceipts`、`OnRecvMessageRevoked`、`OnRecvMessageModified`、`OnRecvMessageExtensionsChanged/Deleted`、`OnRecvMessageReactionsChanged`、`OnGroupMessagePinned` |
| `V2TIMGroupListener` | `OnMemberEnter/Leave/Invited/Kicked`、`OnGroupCreated/Dismissed/Recycled`、`OnGroupInfoChanged`、`OnGroupAttributeChanged`、`OnGroupCounterChanged`、`OnReceiveJoinApplication`、`OnApplicationProcessed`、`OnGrantAdministrator`、`OnRevokeAdministrator`、`OnTopicCreated/Deleted/Changed` 等 |
| `V2TIMCommunityListener` | 话题创建/删除/修改、权限组创建/删除/修改、权限组成员/话题权限变更 |
| `V2TIMConversationListener` | `OnSyncServerStart/Finish/Failed`、`OnNewConversation`、`OnConversationChanged`、`OnConversationDeleted`、`OnTotalUnreadMessageCountChanged`、`OnUnreadMessageCountChangedByFilter`、会话分组相关 |
| `V2TIMFriendshipListener` | 好友申请新增/删除/已读、好友新增/删除/资料变更、黑名单变更、好友分组变更、公众号、关注/粉丝 |
| `V2TIMSignalingListener` | `OnReceiveNewInvitation`、`OnInviteeAccepted/Rejected`、`OnInvitationCancelled/Timeout/Modified` |
| `V2TIMLogListener` | `OnLog(V2TIMLogLevel, const V2TIMString&)` |

---

## 十二、关键数据结构（参数必填字段）

```cpp
// 消息
struct V2TIMMessage {
    V2TIMString msgID, sender, nickName, faceURL, groupID, userID;
    int64_t timestamp;
    uint64_t seq, random;
    V2TIMMessageStatus status;      // SENDING/SEND_SUCC/SEND_FAIL/HAS_DELETED/...
    bool isSelf, needReadReceipt, supportMessageExtension;
    V2TIMMessagePriority priority;
    V2TIMStringVector groupAtUserList;
    V2TIMElemVector elemList;       // 用 elemType 判断后 static_cast
    V2TIMBuffer cloudCustomData;
    V2TIMOfflinePushInfo offlinePushInfo;
};

// 文本/自定义消息元素
struct V2TIMTextElem   : V2TIMElem { V2TIMString text; };
struct V2TIMCustomElem : V2TIMElem { V2TIMBuffer data; V2TIMString desc, extension; };

// 图片元素
struct V2TIMImageElem : V2TIMElem { V2TIMString path; V2TIMImageVector imageList; };
struct V2TIMImage { V2TIMString uuid, url; V2TIMImageType type; uint64_t size;
                    uint32_t width, height; void DownloadImage(path, V2TIMDownloadCallback*); };

// 语音/视频/文件元素
struct V2TIMSoundElem : V2TIMElem { V2TIMString path, uuid; uint64_t dataSize; uint32_t duration;
    void GetUrl(V2TIMValueCallback<V2TIMString>*);
    void DownloadSound(path, V2TIMDownloadCallback*);
    void ConvertVoiceToText(language, V2TIMValueCallback<V2TIMString>*); };
struct V2TIMVideoElem : V2TIMElem { V2TIMString videoPath, snapshotPath, videoUUID, videoType, snapshotUUID;
    uint64_t videoSize, snapshotSize; uint32_t duration, snapshotWidth, snapshotHeight;
    void GetVideoUrl(...); void GetSnapshotUrl(...); void DownloadVideo(...); void DownloadSnapshot(...); };
struct V2TIMFileElem : V2TIMElem { V2TIMString path, uuid, filename; uint64_t fileSize;
    void GetUrl(...); void DownloadFile(...); };

// 合并消息
struct V2TIMMergerElem : V2TIMElem { bool layersOverLimit; V2TIMString title;
    V2TIMStringVector abstractList; void DownloadMergerMessage(V2TIMValueCallback<V2TIMMessageVector>*); };

// 群资料
struct V2TIMGroupInfo {
    V2TIMString groupID, groupType, groupName, notification, introduction, faceURL, owner;
    bool isSupportTopic, allMuted, enablePermissionGroup;
    V2TIMCustomInfo customInfo;
    uint32_t createTime, lastInfoTime, lastMessageTime, memberCount, memberMaxCount, role, joinTime;
    V2TIMGroupAddOpt groupAddOpt, groupApproveOpt;
    V2TIMReceiveMessageOpt recvOpt;
    uint64_t defaultPermissions;
    uint32_t modifyFlag;             // 修改时按位或 V2TIM_GROUP_INFO_MODIFY_FLAG_xxx
};

// 群成员
struct V2TIMGroupMemberFullInfo : V2TIMGroupMemberInfo {
    V2TIMCustomInfo customInfo;
    uint32_t role, muteUntil, modifyFlag;
    int64_t joinTime;
    bool isOnline;
};

// 用户资料
struct V2TIMUserFullInfo : V2TIMUserInfo {
    V2TIMString selfSignature;
    V2TIMGender gender;
    uint32_t role, level, birthday;
    V2TIMFriendAllowType allowType;
    V2TIMCustomInfo customInfo;
    uint32_t modifyFlag;             // V2TIM_USER_INFO_MODIFY_FLAG_xxx
};

// 会话
struct V2TIMConversation {
    V2TIMConversationType type;      // V2TIM_C2C / V2TIM_GROUP
    V2TIMString conversationID, userID, groupID, groupType, showName, faceUrl, draftText;
    int unreadCount;
    V2TIMReceiveMessageOpt recvOpt;
    V2TIMMessage* lastMessage;
    V2TIMGroupAtInfoVector groupAtInfolist;
    uint64_t draftTimestamp, orderKey, c2cReadTimestamp, groupReadSequence;
    bool isPinned;
    V2TIMUInt64Vector markList;
    V2TIMBuffer customData;
    V2TIMStringVector conversationGroupList;
};

// 离线推送配置 / 信息
struct V2TIMOfflinePushConfig { V2TIMBuffer token; uint32_t businessID; V2TIMOfflinePushTokenType token_type; };
struct V2TIMOfflinePushInfo {
    V2TIMString title, desc, ext;
    bool disablePush, ignoreIOSBadge, enableIOSBackgroundNotification, ignoreHarmonyBadge;
    V2TIMIOSOfflinePushType iOSPushType;
    V2TIMString iOSSound, iOSInterruptionLevel, AndroidSound, AndroidOPPOChannelID,
                AndroidFCMChannelID, AndroidXiaoMiChannelID, AndroidVIVOCategory,
                AndroidHuaWeiCategory, AndroidOPPOCategory, AndroidHonorImportance,
                AndroidHuaWeiImage, AndroidHonorImage, AndroidFCMImage, iOSImage,
                HarmonyImage, HarmonyCategory, vendorParams;
    int AndroidVIVOClassification, AndroidOPPONotifyLevel, AndroidMeizuNotifyType;
};

// 信令
struct V2TIMSignalingInfo {
    V2TIMString inviteID, groupID, inviter, data;
    V2TIMStringVector inviteeList;
    V2TIMSignalingActionType actionType;
    int timeout;
};

// 话题
struct V2TIMTopicInfo {
    V2TIMString topicID, topicType, topicName, topicFaceURL, introduction, notification, customString, draftText;
    bool isAllMuted;
    uint32_t selfMuteTime, createTime, memberMaxCount;
    V2TIMReceiveMessageOpt recvOpt;
    uint64_t unreadCount, readSequence, defaultPermissions;
    V2TIMMessage* lastMessage;
    V2TIMGroupAddOpt topicAddOpt, topicApproveOpt;
    V2TIMCreateGroupMemberInfoVector memberlist;
    V2TIMGroupAtInfoVector groupAtInfoList;
    uint32_t modifyFlag;
};
```

枚举常用值：

| 枚举 | 常用值 |
|---|---|
| `V2TIMMessageStatus` | `SENDING=1, SEND_SUCC=2, SEND_FAIL=3, HAS_DELETED=4, LOCAL_IMPORTED=5, LOCAL_REVOKED=6` |
| `V2TIMElemType` | `TEXT=1, CUSTOM=2, IMAGE=3, SOUND=4, VIDEO=5, FILE=6, LOCATION=7, FACE=8, GROUP_TIPS=9, MERGER=10, STREAM=11` |
| `V2TIMGroupMemberRole` | `MEMBER=200, ADMIN=300, SUPER=400` |
| `V2TIMFriendType` | `SINGLE=1, BOTH=2` |
| `V2TIMFriendRelationType` | `NONE=0, IN_MY=1, IN_OTHER=2, BOTH_WAY=3` |
| `V2TIMMessagePriority` | `DEFAULT=0, HIGH=1, NORMAL=2, LOW=3` |
| `V2TIMGroupAddOpt` | `FORBID=0, AUTH=1, ANY=2, UNKNOWN=3` |

---

## 十三、常用错误码

| 错误码 | 含义 |
|---|---|
| `0 ERR_SUCC` | 成功 |
| `6013` | SDK 未初始化 |
| `6014` | 未登录 |
| `6206` | userSig 过期，需重新签发 |
| `6208` | 被其他端踢下线 |
| `6017` | 参数无效 |
| `7008` | 调用超频 |
| `8001` | 消息体超过 12KB |
| `8501~8514` | 群相关错误 |
| `9001~9006` | 关系链参数/数量错误 |
| `9506~9512` | 网络类错误 |
| `60020` | 未开通该增值服务 |

更多错误码请参考 `cpp_include/V2TIMErrorCode.h`。

---

## 十四、C++ 完整示例

### 1. 初始化 + 登录 + 监听

```cpp
#include "V2TIMManager.h"
#include <iostream>

using namespace std;

// ---- 全局 SDK 监听 ----
class MySDKListener : public V2TIMSDKListener {
public:
    void OnConnectSuccess() override { cout << "connected\n"; }
    void OnConnectFailed(int code, const V2TIMString& msg) override {
        cout << "connect failed: " << code << " " << msg.CString() << endl;
    }
    void OnKickedOffline() override { cout << "kicked offline\n"; }
    void OnUserSigExpired() override { cout << "userSig expired\n"; }
};

class LoginCallback : public V2TIMCallback {
public:
    void OnSuccess() override { cout << "login success\n"; }
    void OnError(int code, const V2TIMString& msg) override {
        cout << "login error " << code << " " << msg.CString() << endl;
    }
};

MySDKListener g_sdkListener;

void InitAndLogin(uint32_t sdkAppID, const char* userID, const char* userSig) {
    V2TIMSDKConfig config;
    config.logLevel  = V2TIM_LOG_INFO;
    config.initPath  = "C:\\im\\data";
    config.logPath   = "C:\\im\\log";

    V2TIMManager* im = V2TIMManager::GetInstance();
    if (!im->InitSDK(sdkAppID, config)) {
        cout << "InitSDK failed\n";
        return;
    }
    im->AddSDKListener(&g_sdkListener);
    im->Login(userID, userSig, new LoginCallback());
}
```

### 2. 发送文本消息 / 高级消息

```cpp
#include "V2TIMMessageManager.h"

class SendCallback : public V2TIMSendCallback {
public:
    void OnSuccess(const V2TIMMessage& msg) override {
        cout << "send ok, msgID=" << msg.msgID.CString() << endl;
    }
    void OnError(int code, const V2TIMString& msg) override {
        cout << "send error " << code << " " << msg.CString() << endl;
    }
    void OnProgress(uint32_t progress) override {
        cout << "progress " << progress << "%\n";
    }
};

void SendText(const char* toUserID, const char* text) {
    V2TIMMessageManager* mm = V2TIMManager::GetInstance()->GetMessageManager();

    V2TIMMessage msg = mm->CreateTextMessage(text);
    V2TIMOfflinePushInfo push;
    push.title = "新消息";
    push.desc  = text;

    mm->SendMessage(msg, toUserID, "", V2TIM_PRIORITY_NORMAL,
                    false, push, new SendCallback());
}

void SendImage(const char* toUserID, const char* imagePath) {
    V2TIMMessageManager* mm = V2TIMManager::GetInstance()->GetMessageManager();
    V2TIMMessage msg = mm->CreateImageMessage(imagePath);
    mm->SendMessage(msg, toUserID, "", V2TIM_PRIORITY_NORMAL,
                    false, V2TIMOfflinePushInfo(), new SendCallback());
}
```

### 3. 拉取历史消息

```cpp
class HistoryCallback : public V2TIMValueCallback<V2TIMMessageVector> {
public:
    void OnSuccess(const V2TIMMessageVector& list) override {
        for (size_t i = 0; i < list.Size(); ++i) {
            const V2TIMMessage& m = list[i];
            if (m.elemList.Size() > 0 &&
                m.elemList[0]->elemType == V2TIM_ELEM_TYPE_TEXT) {
                auto* text = static_cast<V2TIMTextElem*>(m.elemList[0]);
                cout << m.sender.CString() << ": " << text->text.CString() << endl;
            }
        }
    }
    void OnError(int code, const V2TIMString& msg) override {
        cout << "history error " << code << " " << msg.CString() << endl;
    }
};

void GetHistory(const char* peerUserID) {
    V2TIMMessageListGetOption opt;
    opt.getType    = V2TIM_GET_CLOUD_OLDER_MSG;
    opt.userID     = peerUserID;
    opt.count      = 20;
    opt.lastMsg    = nullptr;
    opt.lastMsgSeq = 0;

    V2TIMManager::GetInstance()->GetMessageManager()
        ->GetHistoryMessageList(opt, new HistoryCallback());
}
```

### 4. 创建群 / 拉取群列表

```cpp
#include "V2TIMGroupManager.h"

class GroupCreateCallback : public V2TIMValueCallback<V2TIMString> {
public:
    void OnSuccess(const V2TIMString& groupID) override {
        cout << "group created: " << groupID.CString() << endl;
    }
    void OnError(int code, const V2TIMString& msg) override {
        cout << "create group error " << code << " " << msg.CString() << endl;
    }
};

void CreatePublicGroup(const char* name) {
    V2TIMGroupInfo info;
    info.groupType  = "Public";
    info.groupName  = name;

    V2TIMCreateGroupMemberInfoVector members;   // 可添加初始成员
    V2TIMManager::GetInstance()->GetGroupManager()
        ->CreateGroup(info, members, new GroupCreateCallback());
}
```

### 5. 会话列表

```cpp
#include "V2TIMConversationManager.h"

class ConvCallback : public V2TIMValueCallback<V2TIMConversationResult> {
public:
    void OnSuccess(const V2TIMConversationResult& result) override {
        for (size_t i = 0; i < result.conversationList.Size(); ++i) {
            const V2TIMConversation& c = result.conversationList[i];
            cout << c.conversationID.CString()
                 << " unread=" << c.unreadCount
                 << " showName=" << c.showName.CString() << endl;
        }
    }
    void OnError(int code, const V2TIMString& msg) override {}
};

void LoadConversations() {
    V2TIMManager::GetInstance()->GetConversationManager()
        ->GetConversationList(0, 100, new ConvCallback());
}
```

### 6. 加好友

```cpp
#include "V2TIMFriendshipManager.h"

class FriendOpCallback : public V2TIMValueCallback<V2TIMFriendOperationResult> {
public:
    void OnSuccess(const V2TIMFriendOperationResult& r) override {
        cout << "add friend result=" << r.resultCode << " " << r.resultInfo.CString() << endl;
    }
    void OnError(int code, const V2TIMString& msg) override {}
};

void AddFriend(const char* userID) {
    V2TIMFriendAddApplication app;
    app.userID      = userID;
    app.friendRemark = "备注";
    app.addWording  = "你好，加个好友";
    app.addType     = V2TIM_FRIEND_TYPE_BOTH;

    V2TIMManager::GetInstance()->GetFriendshipManager()
        ->AddFriend(app, new FriendOpCallback());
}
```

### 7. 信令邀请

```cpp
#include "V2TIMSignalingManager.h"

V2TIMManager* im = V2TIMManager::GetInstance();
V2TIMOfflinePushInfo push;
push.desc = "邀请你加入通话";

V2TIMString inviteID = im->GetSignalingManager()->Invite(
    "userB",                 // 被邀请人
    "{\"cmd\":\"call\"}",    // 自定义 data
    false,                   // onlineUserOnly
    push,
    30,                      // 30 秒超时
    nullptr);                // 可用 V2TIMCallback 监听失败
```

### 8. 离线推送配置

```cpp
#include "V2TIMOfflinePushManager.h"

void SetPush(const char* tokenStr) {
    V2TIMOfflinePushConfig cfg;
    cfg.token      = V2TIMBuffer((const uint8_t*)tokenStr, strlen(tokenStr));
    cfg.businessID = 12345;   // 控制台证书 ID
    cfg.token_type = V2TIM_OFFLINE_PUSH_TOKEN_TYPE_DEFAULT;

    V2TIMManager::GetInstance()->GetOfflinePushManager()
        ->SetOfflinePushConfig(cfg, nullptr);
}
```

### 9. 消息监听器

```cpp
class MyMsgListener : public V2TIMAdvancedMsgListener {
public:
    void OnRecvNewMessage(const V2TIMMessage& msg) override {
        cout << "new msg from " << msg.sender.CString() << endl;
        if (msg.elemList.Size() > 0) {
            V2TIMElem* e = msg.elemList[0];
            if (e->elemType == V2TIM_ELEM_TYPE_TEXT) {
                auto* t = static_cast<V2TIMTextElem*>(e);
                cout << "text: " << t->text.CString() << endl;
            }
        }
    }
};

MyMsgListener g_msgListener;

void RegisterMsgListener() {
    V2TIMManager::GetInstance()->GetMessageManager()
        ->AddAdvancedMsgListener(&g_msgListener);
}
```

---

## 十五、使用建议

1. **登录前**先 `InitSDK` 并注册 `V2TIMSDKListener`，登录成功后再操作消息/群/会话。
2. **UI 线程**回调：SDK 回调一般在主线程/独立线程触发，更新 UI 需切回主线程。
3. **回调对象**按官方示例 `new` 传入，回调后由 SDK 释放；**监听器对象**由业务持有，退出时 `RemoveXxxListener`。
4. **UserSig 由服务端签发**，客户端不要内置密钥；收到 `6206` 需重新签发并登录。
5. **高频接口限频**（群属性 5 秒 10 次、消息删除 1 秒 1 次等），注意错误码 `7008/8511`。
6. 需要旗舰版的能力（已读回执、消息扩展、回应、搜索、权限组、关注粉丝等）需在控制台购买并开启对应开关。

---

> 本文档覆盖本目录 `cpp_include` 中 `V2TIMManager`、`V2TIMMessageManager`、`V2TIMGroupManager`、`V2TIMConversationManager`、`V2TIMFriendshipManager`、`V2TIMCommunityManager`、`V2TIMSignalingManager`、`V2TIMOfflinePushManager` 的全部公开接口及参数。
