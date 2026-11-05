#include "TencentImService.h"

#ifdef TENCENT_CLOUD_IM_SDK_VERSION

#include "TIMCloud.h"

#include <Windows.h>
#include <bcrypt.h>
#include <cstdio>
#include <ctime>
#include <cstdlib>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

#include "duilib/third_party/zlib/zlib.h"

#pragma comment(lib, "bcrypt.lib")

namespace {

const char* kBase64Chars = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

std::string Base64Encode(const unsigned char* data, size_t length)
{
    std::string result;
    result.reserve(((length + 2) / 3) * 4);

    for (size_t i = 0; i < length; i += 3) {
        uint32_t value = static_cast<uint32_t>(data[i]) << 16;
        const bool hasSecond = (i + 1) < length;
        const bool hasThird = (i + 2) < length;
        if (hasSecond) {
            value |= static_cast<uint32_t>(data[i + 1]) << 8;
        }
        if (hasThird) {
            value |= static_cast<uint32_t>(data[i + 2]);
        }

        result.push_back(kBase64Chars[(value >> 18) & 0x3F]);
        result.push_back(kBase64Chars[(value >> 12) & 0x3F]);
        result.push_back(hasSecond ? kBase64Chars[(value >> 6) & 0x3F] : '=');
        result.push_back(hasThird ? kBase64Chars[value & 0x3F] : '=');
    }
    return result;
}

std::string Base64Encode(const std::vector<unsigned char>& data)
{
    if (data.empty()) {
        return std::string();
    }
    return Base64Encode(data.data(), data.size());
}

std::string JsonEscape(const std::string& value)
{
    std::string result;
    result.reserve(value.size() + 8);

    for (unsigned char ch : value) {
        switch (ch) {
        case '"': result += "\\\""; break;
        case '\\': result += "\\\\"; break;
        case '\b': result += "\\b"; break;
        case '\f': result += "\\f"; break;
        case '\n': result += "\\n"; break;
        case '\r': result += "\\r"; break;
        case '\t': result += "\\t"; break;
        default:
            if (ch < 0x20) {
                char buffer[8] = { 0 };
                std::snprintf(buffer, sizeof(buffer), "\\u%04X", ch);
                result += buffer;
            }
            else {
                result.push_back(static_cast<char>(ch));
            }
            break;
        }
    }
    return result;
}

std::string JsonString(const std::string& value)
{
    return std::string("\"") + JsonEscape(value) + "\"";
}

std::vector<unsigned char> HmacSha256(const std::string& key, const std::string& data)
{
    std::vector<unsigned char> hash;
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    BCRYPT_HASH_HANDLE hashHandle = nullptr;
    DWORD objectLength = 0;
    DWORD hashLength = 0;
    DWORD bytesWritten = 0;

    if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, BCRYPT_ALG_HANDLE_HMAC_FLAG) != 0) {
        return hash;
    }

    bool succeed = BCryptGetProperty(algorithm, BCRYPT_OBJECT_LENGTH,
        reinterpret_cast<PUCHAR>(&objectLength), sizeof(objectLength), &bytesWritten, 0) == 0;
    succeed = succeed && BCryptGetProperty(algorithm, BCRYPT_HASH_LENGTH,
        reinterpret_cast<PUCHAR>(&hashLength), sizeof(hashLength), &bytesWritten, 0) == 0;

    if (succeed) {
        std::vector<unsigned char> hashObject(objectLength);
        hash.resize(hashLength);
        if (BCryptCreateHash(algorithm, &hashHandle, hashObject.data(), objectLength,
            reinterpret_cast<PUCHAR>(const_cast<char*>(key.data())), static_cast<ULONG>(key.size()), 0) == 0) {
            const bool hashOk = BCryptHashData(hashHandle, reinterpret_cast<PUCHAR>(const_cast<char*>(data.data())),
                static_cast<ULONG>(data.size()), 0) == 0;
            if (hashOk) {
                if (BCryptFinishHash(hashHandle, hash.data(), hashLength, 0) != 0) {
                    hash.clear();
                }
            }
            else {
                hash.clear();
            }
            BCryptDestroyHash(hashHandle);
        }
        else {
            hash.clear();
        }
    }

    BCryptCloseAlgorithmProvider(algorithm, 0);
    return hash;
}

std::vector<std::string> ExtractJsonStringValues(const std::string& json, const std::string& key)
{
    std::vector<std::string> values;
    const std::string pattern = std::string("\"") + key + "\"";
    size_t pos = 0;
    while ((pos = json.find(pattern, pos)) != std::string::npos) {
        pos += pattern.size();
        pos = json.find(':', pos);
        if (pos == std::string::npos) {
            break;
        }
        ++pos;
        while ((pos < json.size()) && ((json[pos] == ' ') || (json[pos] == '\t') || (json[pos] == '\r') || (json[pos] == '\n'))) {
            ++pos;
        }
        if ((pos >= json.size()) || (json[pos] != '"')) {
            continue;
        }
        ++pos;
        std::string value;
        while (pos < json.size()) {
            const char ch = json[pos++];
            if (ch == '\\') {
                if (pos < json.size()) {
                    value.push_back(json[pos++]);
                }
            }
            else if (ch == '"') {
                break;
            }
            else {
                value.push_back(ch);
            }
        }
        values.push_back(value);
    }
    return values;
}

int64_t ExtractJsonInt64(const std::string& json, const std::string& key)
{
    const std::string pattern = std::string("\"") + key + "\"";
    size_t pos = json.find(pattern);
    if (pos == std::string::npos) {
        return 0;
    }
    pos = json.find(':', pos + pattern.size());
    if (pos == std::string::npos) {
        return 0;
    }
    ++pos;
    while ((pos < json.size()) && ((json[pos] == ' ') || (json[pos] == '\t') || (json[pos] == '\r') || (json[pos] == '\n'))) {
        ++pos;
    }
    if ((pos >= json.size()) || (json[pos] < '0') || (json[pos] > '9')) {
        return 0;
    }
    int64_t value = 0;
    while ((pos < json.size()) && (json[pos] >= '0') && (json[pos] <= '9')) {
        value = value * 10 + (json[pos] - '0');
        ++pos;
    }
    return value;
}

bool ExtractJsonBool(const std::string& json, const std::string& key)
{
    const std::string pattern = std::string("\"") + key + "\"";
    size_t pos = json.find(pattern);
    if (pos == std::string::npos) {
        return false;
    }
    pos = json.find(':', pos + pattern.size());
    if (pos == std::string::npos) {
        return false;
    }
    ++pos;
    while ((pos < json.size()) && ((json[pos] == ' ') || (json[pos] == '\t') || (json[pos] == '\r') || (json[pos] == '\n'))) {
        ++pos;
    }
    return json.compare(pos, 4, "true") == 0;
}

std::vector<std::string> SplitJsonObjectArray(const std::string& json)
{
    std::vector<std::string> objects;
    bool inString = false;
    bool escaped = false;
    int depth = 0;
    size_t start = std::string::npos;

    for (size_t i = 0; i < json.size(); ++i) {
        const char ch = json[i];
        if (inString) {
            if (escaped) {
                escaped = false;
            }
            else if (ch == '\\') {
                escaped = true;
            }
            else if (ch == '"') {
                inString = false;
            }
            continue;
        }
        if (ch == '"') {
            inString = true;
        }
        else if (ch == '{') {
            if (depth == 0) {
                start = i;
            }
            ++depth;
        }
        else if (ch == '}') {
            --depth;
            if ((depth == 0) && (start != std::string::npos)) {
                objects.push_back(json.substr(start, i - start + 1));
                start = std::string::npos;
            }
        }
    }
    return objects;
}

std::vector<TencentImService::ChatMessage> ParseChatMessages(const std::string& json)
{
    std::vector<TencentImService::ChatMessage> messages;
    for (const std::string& object : SplitJsonObjectArray(json)) {
        const std::vector<std::string> textValues = ExtractJsonStringValues(object, kTIMTextElemContent);
        if (textValues.empty()) {
            continue;
        }

        TencentImService::ChatMessage message;
        message.fromSelf = ExtractJsonBool(object, kTIMMsgIsFormSelf);
        message.timestamp = ExtractJsonInt64(object, kTIMMsgServerTime);
        const std::vector<std::string> senders = ExtractJsonStringValues(object, kTIMMsgSender);
        if (!senders.empty()) {
            message.sender = senders.front();
        }
        for (size_t i = 0; i < textValues.size(); ++i) {
            if (i > 0) {
                message.text += "\n";
            }
            message.text += textValues[i];
        }
        if (!message.text.empty()) {
            messages.push_back(message);
        }
    }
    return messages;
}

struct CallbackContext
{
    TencentImService::ResultCallback callback;
};

struct JsonCallbackContext
{
    TencentImService::JsonCallback callback;
};

struct FriendListContext
{
    TencentImService::FriendListCallback callback;
};

struct UserProfileContext
{
    TencentImService::UserProfileCallback callback;
};

struct MessageHistoryContext
{
    TencentImService::ChatMessageListCallback callback;
};

int32_t InvokeResultCallback(int32_t code, const char* desc, const void* userData)
{
    const CallbackContext* context = static_cast<const CallbackContext*>(userData);
    if (context != nullptr) {
        if (context->callback) {
            context->callback(code, desc != nullptr ? desc : "");
        }
        delete context;
    }
    return code;
}

int32_t InvokeJsonCallback(int32_t code, const char* desc, const char* jsonParams, const void* userData)
{
    const JsonCallbackContext* context = static_cast<const JsonCallbackContext*>(userData);
    if (context != nullptr) {
        if (context->callback) {
            context->callback(code, desc != nullptr ? desc : "", jsonParams != nullptr ? jsonParams : "");
        }
        delete context;
    }
    return code;
}

int32_t InvokeFriendListCallback(int32_t code, const char* desc, const char* jsonParams, const void* userData)
{
    const FriendListContext* context = static_cast<const FriendListContext*>(userData);
    if (context != nullptr) {
        std::vector<std::string> friendIds;
        if ((code == 0) && (jsonParams != nullptr)) {
            friendIds = ExtractJsonStringValues(jsonParams, kTIMFriendProfileIdentifier);
        }
        if (context->callback) {
            context->callback(code, desc != nullptr ? desc : "", friendIds);
        }
        delete context;
    }
    return code;
}

int32_t InvokeUserProfileCallback(int32_t code, const char* desc, const char* jsonParams, const void* userData)
{
    const UserProfileContext* context = static_cast<const UserProfileContext*>(userData);
    if (context != nullptr) {
        TencentImService::UserProfile profile;
        if ((code == 0) && (jsonParams != nullptr) && (jsonParams[0] != '\0')) {
            const std::vector<std::string> ids = ExtractJsonStringValues(jsonParams, kTIMUserProfileIdentifier);
            const std::vector<std::string> names = ExtractJsonStringValues(jsonParams, kTIMUserProfileNickName);
            if (!ids.empty()) {
                profile.userId = ids.front();
            }
            if (!names.empty()) {
                profile.nickName = names.front();
            }
        }
        if (context->callback) {
            context->callback(code, desc != nullptr ? desc : "", profile);
        }
        delete context;
    }
    return code;
}

int32_t InvokeMessageHistoryCallback(int32_t code, const char* desc, const char* jsonParams, const void* userData)
{
    const MessageHistoryContext* context = static_cast<const MessageHistoryContext*>(userData);
    if (context != nullptr) {
        std::vector<TencentImService::ChatMessage> messages;
        if ((code == 0) && (jsonParams != nullptr)) {
            messages = ParseChatMessages(jsonParams);
        }
        if (context->callback) {
            context->callback(code, desc != nullptr ? desc : "", messages);
        }
        delete context;
    }
    return code;
}

} // namespace

TencentImService& TencentImService::Instance()
{
    static TencentImService instance;
    return instance;
}

uint64_t TencentImService::AppId() const
{
    return std::strtoull(TIM_SDK_APPID, nullptr, 10);
}

bool TencentImService::IsInitialized() const
{
    return m_initialized;
}

bool TencentImService::Initialize()
{
    auto logger = spdlog::get("im_file_logger");
    if (m_initialized) {
        return true;
    }

    const uint64_t appId = AppId();
    if (appId == 0) {
        return false;
    }

    if (TIMInit(appId, "{}") != TIM_SUCC) {
        logger->error("TIMInit 初始化失败");
        return false;
    }

    m_initialized = true;
    logger->info("腾讯云SDK初始化成功");

    TIMAddRecvNewMsgCallback(&TencentImService::OnRecvNewMsgThunk, this);
    TIMSetOnAddFriendCallback(&TencentImService::OnFriendAddedThunk, this);
    TIMSetFriendAddRequestCallback(&TencentImService::OnFriendRequestThunk, this);
    return true;
}

void TencentImService::Uninitialize()
{
    if (!m_initialized) {
        return;
    }

    TIMRemoveRecvNewMsgCallback(&TencentImService::OnRecvNewMsgThunk);
    TIMSetOnAddFriendCallback(nullptr, nullptr);
    TIMSetFriendAddRequestCallback(nullptr, nullptr);
    TIMUninit();
    m_initialized = false;
}

int TencentImService::Login(const std::string& userId, const std::string& userSig, ResultCallback callback)
{
    if (!m_initialized || userId.empty() || userSig.empty()) {
        return TIM_ERR_PARAM;
    }

    CallbackContext* context = new CallbackContext{ std::move(callback) };
    const int result = TIMLogin(userId.c_str(), userSig.c_str(), &TencentImService::OnResultThunk, context);
    if (result != TIM_SUCC) {
        delete context;
    }
    return result;
}

int TencentImService::LoginAccount(const std::string& userId, ResultCallback callback, uint32_t expireSeconds)
{
    const std::string userSig = GenerateUserSig(userId, expireSeconds);
    if (userSig.empty()) {
        return TIM_ERR_PARAM;
    }
    return Login(userId, userSig, std::move(callback));
}

int TencentImService::Logout(ResultCallback callback)
{
    if (!m_initialized) {
        return TIM_ERR_SDKUNINIT;
    }

    CallbackContext* context = new CallbackContext{ std::move(callback) };
    const int result = TIMLogout(&TencentImService::OnResultThunk, context);
    if (result != TIM_SUCC) {
        delete context;
    }
    return result;
}

int TencentImService::GetSelfProfile(UserProfileCallback callback)
{
    auto logger = spdlog::get("im_file_logger");
    if (!m_initialized) {
        if (logger) {
            logger->warn("获取当前用户资料失败：SDK未初始化");
        }
        return TIM_ERR_SDKUNINIT;
    }

    char userId[128] = { 0 };
    if ((TIMGetLoginUserID(userId) != TIM_SUCC) || (userId[0] == '\0')) {
        return TIM_ERR_NOTLOGIN;
    }

    std::string json = "{";
    json += JsonString(kTIMFriendShipGetProfileListParamIdentifierArray) + ":[" + JsonString(userId) + "]}";

    UserProfileContext* context = new UserProfileContext{ std::move(callback) };
    const int result = TIMProfileGetUserProfileList(json.c_str(), &TencentImService::OnUserProfileThunk, context);
    if (result != TIM_SUCC) {
        delete context;
        if (logger) {
            logger->error("获取当前用户资料失败，ret: {}", result);
        }
    }
    return result;
}

int TencentImService::GetFriendList(FriendListCallback callback)
{
    auto logger = spdlog::get("im_file_logger");
    if (!m_initialized) {
        if (logger) {
            logger->warn("获取好友列表失败：SDK未初始化");
        }
        return TIM_ERR_SDKUNINIT;
    }

    FriendListContext* context = new FriendListContext{ std::move(callback) };
    const int result = TIMFriendshipGetFriendProfileList(&TencentImService::OnFriendListThunk, context);
    if (result != TIM_SUCC) {
        delete context;
        if (logger) {
            logger->error("获取好友列表失败，ret: {}", result);
        }
    }
    else if (logger) {
        logger->info("获取好友列表请求已发送");
    }
    return result;
}

int TencentImService::SearchUser(const std::string& userId, JsonCallback callback)
{
    auto logger = spdlog::get("im_file_logger");
    if (!m_initialized || userId.empty()) {
        if (logger) {
            logger->warn("搜索用户失败：SDK未初始化或用户ID为空");
        }
        return TIM_ERR_PARAM;
    }

    std::string json = "{";
    json += JsonString(kTIMFriendShipGetProfileListParamIdentifierArray) + ":[" + JsonString(userId) + "]}";

    JsonCallbackContext* context = new JsonCallbackContext{ std::move(callback) };
    const int result = TIMProfileGetUserProfileList(json.c_str(), &TencentImService::OnJsonResultThunk, context);
    if (result != TIM_SUCC) {
        delete context;
        if (logger) {
            logger->error("搜索用户失败，UserID: {}, ret: {}", userId, result);
        }
    }
    else if (logger) {
        logger->info("搜索用户请求已发送，UserID: {}", userId);
    }
    return result;
}

int TencentImService::AddFriend(const std::string& userId, const std::string& addWording,
    const std::string& remark, ResultCallback callback)
{
    if (!m_initialized || userId.empty()) {
        return TIM_ERR_PARAM;
    }

    std::string json = "{";
    json += JsonString(kTIMFriendshipAddFriendParamIdentifier) + ":" + JsonString(userId) + ",";
    json += JsonString(kTIMFriendshipAddFriendParamFriendType) + ":" + std::to_string(static_cast<int>(FriendTypeBoth)) + ",";
    json += JsonString(kTIMFriendshipAddFriendParamAddSource) + ":" + JsonString("Windows");
    if (!addWording.empty()) {
        json += "," + JsonString(kTIMFriendshipAddFriendParamAddWording) + ":" + JsonString(addWording);
    }
    if (!remark.empty()) {
        json += "," + JsonString(kTIMFriendshipAddFriendParamRemark) + ":" + JsonString(remark);
    }
    json += "}";

    CallbackContext* context = new CallbackContext{ std::move(callback) };
    const int result = TIMFriendshipAddFriend(json.c_str(), &TencentImService::OnResultThunk, context);
    if (result != TIM_SUCC) {
        delete context;
    }
    return result;
}

int TencentImService::SendTextMessage(const std::string& peerUserId, const std::string& text, ResultCallback callback)
{
    auto logger = spdlog::get("im_file_logger");

    if (!m_initialized || peerUserId.empty() || text.empty()) {
        return TIM_ERR_PARAM;
    }

    std::string element = "{";
    element += JsonString(kTIMElemType) + ":" + std::to_string(static_cast<int>(kTIMElem_Text)) + ",";
    element += JsonString(kTIMTextElemContent) + ":" + JsonString(text) + "}";

    std::string message = "{";
    message += JsonString(kTIMMsgElemArray) + ":[" + element + "]}";

    char messageId[128] = { 0 };
    CallbackContext* context = new CallbackContext{ std::move(callback) };
    const int result = TIMMsgSendMessage(peerUserId.c_str(), kTIMConv_C2C, message.c_str(), messageId,
        &TencentImService::OnResultThunk, context);
    if (result != TIM_SUCC) {
        logger->error("发送消息{}失败!", message);
        delete context;
    }else{
        logger->info("发送消息{}成功!", message);
    }
    return result;
}

int TencentImService::GetMessageHistory(const std::string& peerUserId, ChatMessageListCallback callback)
{
    auto logger = spdlog::get("im_file_logger");
    if (!m_initialized || peerUserId.empty()) {
        return TIM_ERR_PARAM;
    }

    std::string json = "{";
    json += JsonString(kTIMMsgGetMsgListParamCount) + ":50,";
    json += JsonString(kTIMMsgGetMsgListParamIsRamble) + ":true,";
    json += JsonString(kTIMMsgGetMsgListParamIsForward) + ":false}";

    MessageHistoryContext* context = new MessageHistoryContext{ std::move(callback) };
    const int result = TIMMsgGetMsgList(peerUserId.c_str(), kTIMConv_C2C, json.c_str(),
        &TencentImService::OnMessageHistoryThunk, context);
    if (result != TIM_SUCC) {
        delete context;
        if (logger) {
            logger->error("获取历史消息失败，UserID: {}, ret: {}", peerUserId, result);
        }
    }
    else if (logger) {
        logger->info("获取历史消息请求已发送，UserID: {}", peerUserId);
    }
    return result;
}

std::string TencentImService::GenerateUserSig(const std::string& userId, uint32_t expireSeconds) const
{
    if (userId.empty()) {
        return std::string();
    }

    const uint64_t appId = AppId();
    if (appId == 0) {
        return std::string();
    }

    const uint64_t now = static_cast<uint64_t>(std::time(nullptr));
    std::string content;
    content += "TLS.identifier:" + userId + "\n";
    content += "TLS.sdkappid:" + std::to_string(appId) + "\n";
    content += "TLS.time:" + std::to_string(now) + "\n";
    content += "TLS.expire:" + std::to_string(expireSeconds) + "\n";

    const std::vector<unsigned char> signature = HmacSha256(TIM_SDK_APPKEY, content);
    if (signature.empty()) {
        return std::string();
    }

    std::string json = "{";
    json += JsonString("TLS.ver") + ":" + JsonString("2.0") + ",";
    json += JsonString("TLS.identifier") + ":" + JsonString(userId) + ",";
    json += JsonString("TLS.sdkappid") + ":" + std::to_string(appId) + ",";
    json += JsonString("TLS.expire") + ":" + std::to_string(expireSeconds) + ",";
    json += JsonString("TLS.time") + ":" + std::to_string(now) + ",";
    json += JsonString("TLS.sig") + ":" + JsonString(Base64Encode(signature));
    json += "}";

    uLongf compressedLength = compressBound(static_cast<uLong>(json.size()));
    std::vector<unsigned char> compressed(compressedLength);
    const int result = compress2(compressed.data(), &compressedLength,
        reinterpret_cast<const Bytef*>(json.data()), static_cast<uLong>(json.size()), Z_DEFAULT_COMPRESSION);
    if (result != Z_OK) {
        return std::string();
    }
    compressed.resize(compressedLength);
    return Base64Encode(compressed);
}

void TencentImService::SetNewMessageCallback(ChatMessageCallback callback)
{
    m_newMessageCallback = std::move(callback);
}

void TencentImService::SetFriendRequestCallback(MessageCallback callback)
{
    m_friendRequestCallback = std::move(callback);
}

void TencentImService::SetFriendAddedCallback(MessageCallback callback)
{
    m_friendAddedCallback = std::move(callback);
}

void TencentImService::OnResultThunk(int32_t code, const char* desc, const char* /*jsonParams*/, const void* userData)
{
    InvokeResultCallback(code, desc, userData);
}

void TencentImService::OnJsonResultThunk(int32_t code, const char* desc, const char* jsonParams, const void* userData)
{
    InvokeJsonCallback(code, desc, jsonParams, userData);
}

void TencentImService::OnFriendListThunk(int32_t code, const char* desc, const char* jsonParams, const void* userData)
{
    InvokeFriendListCallback(code, desc, jsonParams, userData);
}

void TencentImService::OnUserProfileThunk(int32_t code, const char* desc, const char* jsonParams, const void* userData)
{
    InvokeUserProfileCallback(code, desc, jsonParams, userData);
}

void TencentImService::OnMessageHistoryThunk(int32_t code, const char* desc, const char* jsonParams, const void* userData)
{
    InvokeMessageHistoryCallback(code, desc, jsonParams, userData);
}

void TencentImService::OnRecvNewMsgThunk(const char* jsonMessages, const void* userData)
{
    const TencentImService* service = static_cast<const TencentImService*>(userData);
    if ((service != nullptr) && service->m_newMessageCallback) {
        const std::vector<TencentImService::ChatMessage> messages = ParseChatMessages(jsonMessages != nullptr ? jsonMessages : "");
        if (!messages.empty()) {
            service->m_newMessageCallback(messages);
        }
    }
}

void TencentImService::OnFriendRequestThunk(const char* jsonRequests, const void* userData)
{
    const TencentImService* service = static_cast<const TencentImService*>(userData);
    if ((service != nullptr) && service->m_friendRequestCallback) {
        service->m_friendRequestCallback(jsonRequests != nullptr ? jsonRequests : "");
    }
}

void TencentImService::OnFriendAddedThunk(const char* jsonFriendIds, const void* userData)
{
    const TencentImService* service = static_cast<const TencentImService*>(userData);
    if ((service != nullptr) && service->m_friendAddedCallback) {
        service->m_friendAddedCallback(jsonFriendIds != nullptr ? jsonFriendIds : "");
    }
}

#endif // TENCENT_CLOUD_IM_SDK_VERSION
