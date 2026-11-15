#include "ChatForm.h"
#include "TencentImService.h"

#include <algorithm>
#include <cctype>

#ifdef CXIM_IMS_CORE_SDK_VERSION
#include "HttpClient.h"
#include "ImsSession.h"
#include "TcpClient.h"
#include "LoginForm.h"

#include <ctime>
#include <functional>
#include <thread>
#include <spdlog/spdlog.h>
#endif // CXIM_IMS_CORE_SDK_VERSION

ChatForm::ChatForm(const DString& class_name, const DString& theme_directory, const DString& layout_xml):
    m_alive(std::make_shared<bool>(true)),
    m_themeDirectory(theme_directory),
    m_layoutXml(layout_xml)
{
}


ChatForm::~ChatForm()
{
#ifdef CXIM_IMS_CORE_SDK_VERSION
    TcpClient::Instance().SetMessageCallback(nullptr);
    TcpClient::Instance().SetLoginCallback(nullptr);
    TcpClient::Instance().SetDisconnectedCallback(nullptr);
    TcpClient::Instance().Stop();
#endif
    if (m_alive) {
        *m_alive = false;
    }
}

DString ChatForm::GetSkinFolder()
{
    return m_themeDirectory;
}

DString ChatForm::GetSkinFile()
{
    return m_layoutXml;
}

void ChatForm::OnInitWindow()
{
    //版本号示例代码
    //m_version_label = dynamic_cast<ui::Label*>(FindControl(_T("my_version_label")));
    //const char* labelText = TIMGetSDKVersion();
    //if (m_version_label != nullptr) {
    //    m_version_label->SetText(ui::StringConvert::UTF8ToT(labelText));
    //}

    ui::TabBox* mainPages = dynamic_cast<ui::TabBox*>(FindControl(_T("main_pages")));
    ui::Option* navChat = dynamic_cast<ui::Option*>(FindControl(_T("nav_chat")));
    ui::Option* navContacts = dynamic_cast<ui::Option*>(FindControl(_T("nav_contacts")));

    if (mainPages != nullptr) {
        mainPages->SelectItem(_T("chat_page"));
    }

    if (navChat != nullptr) {
        navChat->Selected(true, false);
        navChat->AttachClick([mainPages](const ui::EventArgs&) {
            if (mainPages != nullptr) {
                mainPages->SelectItem(_T("chat_page"));
            }
            return true;
        });
    }

    if (navContacts != nullptr) {
        navContacts->Selected(false, false);
        navContacts->AttachClick([this, mainPages](const ui::EventArgs&) {
            if (mainPages != nullptr) {
                mainPages->SelectItem(_T("contacts_page"));
            }
#ifdef CXIM_IMS_CORE_SDK_VERSION
            RefreshFriendList();
#endif
            return true;
        });
    }

    ui::VListBox* messageList = dynamic_cast<ui::VListBox*>(FindControl(_T("message_list")));
    if (messageList != nullptr) {
        messageList->SelectItem(1, false, false);
    }

#ifdef TENCENT_CLOUD_IM_SDK_VERSION
    ui::TabBox* contactsListPages = dynamic_cast<ui::TabBox*>(FindControl(_T("contacts_list_pages")));
    ui::Option* tabFriend = dynamic_cast<ui::Option*>(FindControl(_T("tab_friend")));
    ui::Option* tabGroup = dynamic_cast<ui::Option*>(FindControl(_T("tab_group")));

    if (contactsListPages != nullptr) {
        contactsListPages->SelectItem(_T("friend_list"));
    }
    if (tabFriend != nullptr) {
        tabFriend->Selected(true, false);
        tabFriend->AttachClick([contactsListPages](const ui::EventArgs&) {
            if (contactsListPages != nullptr) {
                contactsListPages->SelectItem(_T("friend_list"));
            }
            return true;
        });
    }
    if (tabGroup != nullptr) {
        tabGroup->Selected(false, false);
        tabGroup->AttachClick([contactsListPages](const ui::EventArgs&) {
            if (contactsListPages != nullptr) {
                contactsListPages->SelectItem(_T("group_list"));
            }
            return true;
        });
    }

    RefreshFriendList();

    ui::Button* btnAddContact = dynamic_cast<ui::Button*>(FindControl(_T("btn_add_contact")));
    ui::Box* addFriendPanel = dynamic_cast<ui::Box*>(FindControl(_T("add_friend_panel")));
    ui::Button* btnCloseAddFriend = dynamic_cast<ui::Button*>(FindControl(_T("btn_close_add_friend")));
    ui::RichEdit2* addFriendSearchEdit = dynamic_cast<ui::RichEdit2*>(FindControl(_T("add_friend_search_edit")));
    ui::Button* btnSearchFriend = dynamic_cast<ui::Button*>(FindControl(_T("btn_search_friend")));
    ui::Label* addFriendHint = dynamic_cast<ui::Label*>(FindControl(_T("add_friend_hint")));
    ui::Box* addFriendResult = dynamic_cast<ui::Box*>(FindControl(_T("add_friend_result")));
    ui::Label* addFriendUserId = dynamic_cast<ui::Label*>(FindControl(_T("add_friend_user_id")));
    ui::Button* btnAddFriend = dynamic_cast<ui::Button*>(FindControl(_T("btn_add_friend")));
    ui::Label* addFriendAdded = dynamic_cast<ui::Label*>(FindControl(_T("add_friend_added")));

    auto alive = m_alive;
    auto searchedUserId = std::make_shared<std::string>();

    ui::Button* btnSend = dynamic_cast<ui::Button*>(FindControl(_T("btn_send")));
    if (btnSend != nullptr) {
        btnSend->AttachClick([this, alive](const ui::EventArgs&) {
            if (!*alive) {
                return true;
            }
            SendCurrentMessage();
            return true;
        });
    }

    TencentImService::Instance().SetNewMessageCallback([this, alive](const std::vector<TencentImService::ChatMessage>& messages) {
        if (!*alive) {
            return;
        }
        for (const TencentImService::ChatMessage& message : messages) {
            if (message.fromSelf || message.sender.empty()) {
                continue;
            }
            EnsureConversationRow(message.sender, message.text);
            if (message.sender == m_currentFriendId) {
                AppendChatMessage(false, message.sender, message.text);
            }
        }
    });

    TencentImService::Instance().SetFriendAddedCallback([this, alive](const std::string& /*jsonFriendIds*/) {
        if (!*alive) {
            return;
        }
        RefreshFriendList();
    });

    LoadSelfProfile();

    if ((btnAddContact != nullptr) && (addFriendPanel != nullptr)) {
        btnAddContact->AttachClick([=](const ui::EventArgs&) {
            addFriendPanel->SetVisible(true);
            if (addFriendSearchEdit != nullptr) {
                addFriendSearchEdit->SetText(_T(""));
            }
            if (addFriendResult != nullptr) {
                addFriendResult->SetVisible(false);
            }
            if (addFriendHint != nullptr) {
                addFriendHint->SetText(_T(""));
            }
            if (btnAddFriend != nullptr) {
                btnAddFriend->SetVisible(true);
                btnAddFriend->SetEnabled(true);
                btnAddFriend->SetText(_T("添加"));
            }
            if (addFriendAdded != nullptr) {
                addFriendAdded->SetVisible(false);
            }
            searchedUserId->clear();
            return true;
        });
    }

    if ((btnCloseAddFriend != nullptr) && (addFriendPanel != nullptr)) {
        btnCloseAddFriend->AttachClick([addFriendPanel](const ui::EventArgs&) {
            addFriendPanel->SetVisible(false);
            return true;
        });
    }

    if (btnSearchFriend != nullptr) {
        btnSearchFriend->AttachClick([=](const ui::EventArgs&) {
            if (addFriendSearchEdit == nullptr) {
                return true;
            }

            std::string userId = ui::StringConvert::TToUTF8(addFriendSearchEdit->GetText());
            const auto notSpace = [](unsigned char ch) {
                return (ch != ' ') && (ch != '\t') && (ch != '\r') && (ch != '\n');
            };
            userId.erase(userId.begin(), std::find_if(userId.begin(), userId.end(), notSpace));
            userId.erase(std::find_if(userId.rbegin(), userId.rend(), notSpace).base(), userId.end());
            *searchedUserId = userId;

            if (userId.empty()) {
                if (addFriendHint != nullptr) {
                    addFriendHint->SetText(_T("请输入用户ID"));
                }
                return true;
            }

            if (addFriendResult != nullptr) {
                addFriendResult->SetVisible(false);
            }
            if (btnAddFriend != nullptr) {
                btnAddFriend->SetVisible(true);
                btnAddFriend->SetEnabled(true);
                btnAddFriend->SetText(_T("添加"));
            }
            if (addFriendAdded != nullptr) {
                addFriendAdded->SetVisible(false);
            }
            if (addFriendHint != nullptr) {
                addFriendHint->SetText(_T("搜索中..."));
            }

            auto logger = spdlog::get("im_file_logger");
            if (logger) {
                logger->info("开始搜索用户，UserID: {}", userId);
            }

            TencentImService::Instance().SearchUser(userId, [=](int32_t code, const std::string& desc, const std::string& jsonParams) {
                if (!*alive) {
                    return;
                }

                const bool found = (code == 0) && (jsonParams.find(userId) != std::string::npos);
                auto log = spdlog::get("im_file_logger");
                if (found) {
                    if (addFriendHint != nullptr) {
                        addFriendHint->SetText(_T(""));
                    }
                    if (addFriendUserId != nullptr) {
                        addFriendUserId->SetText(ui::StringConvert::UTF8ToT(userId));
                    }
                    if (addFriendResult != nullptr) {
                        addFriendResult->SetVisible(true);
                    }
                    if (btnAddFriend != nullptr) {
                        btnAddFriend->SetVisible(true);
                        btnAddFriend->SetEnabled(true);
                        btnAddFriend->SetText(_T("添加"));
                    }
                    if (addFriendAdded != nullptr) {
                        addFriendAdded->SetVisible(false);
                    }
                    if (log) {
                        log->info("搜索用户成功，UserID: {}", userId);
                    }
                }
                else {
                    if (addFriendResult != nullptr) {
                        addFriendResult->SetVisible(false);
                    }
                    if (addFriendHint != nullptr) {
                        addFriendHint->SetText(_T("未找到该用户"));
                    }
                    if (log) {
                        log->warn("搜索用户失败，UserID: {}, code: {}, desc: {}", userId, code, desc);
                    }
                }
            });
            return true;
        });
    }

    if (btnAddFriend != nullptr) {
        btnAddFriend->AttachClick([=](const ui::EventArgs&) {
            if (!*alive) {
                return true;
            }

            const std::string userId = *searchedUserId;
            if (userId.empty()) {
                return true;
            }

            btnAddFriend->SetEnabled(false);
            btnAddFriend->SetText(_T("添加中..."));
            if (addFriendHint != nullptr) {
                addFriendHint->SetText(_T(""));
            }

            auto logger = spdlog::get("im_file_logger");
            if (logger) {
                logger->info("请求添加好友，UserID: {}", userId);
            }

            TencentImService::Instance().AddFriend(userId, "请求添加你为好友", std::string(),
                [=](int32_t code, const std::string& desc) {
                    if (!*alive) {
                        return;
                    }

                    auto log = spdlog::get("im_file_logger");
                    if (code == 0) {
                        if (btnAddFriend != nullptr) {
                            btnAddFriend->SetVisible(false);
                        }
                        if (addFriendAdded != nullptr) {
                            addFriendAdded->SetText(_T("已添加"));
                            addFriendAdded->SetVisible(true);
                        }
                        if (addFriendHint != nullptr) {
                            addFriendHint->SetText(_T("添加成功"));
                        }
                        if (log) {
                            log->info("添加好友成功，UserID: {}", userId);
                        }
                        RefreshFriendList();
                    }
                    else {
                        if (btnAddFriend != nullptr) {
                            btnAddFriend->SetEnabled(true);
                            btnAddFriend->SetText(_T("添加"));
                        }
                        if (addFriendHint != nullptr) {
                            const std::string text = std::string("添加失败: ") + desc;
                            addFriendHint->SetText(ui::StringConvert::UTF8ToT(text));
                        }
                        if (log) {
                            log->error("添加好友失败，UserID: {}, code: {}, desc: {}", userId, code, desc);
                        }
                    }
                });
            return true;
        });
    }
#endif

#ifdef CXIM_IMS_CORE_SDK_VERSION
    InitIms();
#endif
}

#ifdef TENCENT_CLOUD_IM_SDK_VERSION
void ChatForm::LoadSelfProfile()
{
    auto alive = m_alive;
    TencentImService::Instance().GetSelfProfile([this, alive](int32_t code, const std::string& desc, const TencentImService::UserProfile& profile) {
        if (!*alive) {
            return;
        }

        auto logger = spdlog::get("im_file_logger");
        if (code != 0) {
            if (logger) {
                logger->error("获取当前用户资料失败，code: {}, desc: {}", code, desc);
            }
            return;
        }

        m_selfUserId = profile.userId;
        ui::Label* selfLabel = dynamic_cast<ui::Label*>(FindControl(_T("self_user_label")));
        if (selfLabel != nullptr) {
            std::string text = profile.nickName;
            if (!profile.userId.empty()) {
                if (!text.empty()) {
                    text += " ";
                }
                text += profile.userId;
            }
            if (text.empty()) {
                text = m_selfUserId;
            }
            selfLabel->SetText(ui::StringConvert::UTF8ToT(text));
        }
        if (logger) {
            logger->info("当前用户资料加载完成，UserID: {}, NickName: {}", profile.userId, profile.nickName);
        }
    });
}

void ChatForm::SelectFriend(const std::string& userId)
{
    if (userId.empty()) {
        return;
    }

    m_currentFriendId = userId;

    ui::TabBox* mainPages = dynamic_cast<ui::TabBox*>(FindControl(_T("main_pages")));
    if (mainPages != nullptr) {
        mainPages->SelectItem(_T("chat_page"));
    }
    ui::Option* navChat = dynamic_cast<ui::Option*>(FindControl(_T("nav_chat")));
    if (navChat != nullptr) {
        navChat->Selected(true, false);
    }
    ui::Option* navContacts = dynamic_cast<ui::Option*>(FindControl(_T("nav_contacts")));
    if (navContacts != nullptr) {
        navContacts->Selected(false, false);
    }

    ui::Label* titleLabel = dynamic_cast<ui::Label*>(FindControl(_T("chat_title_label")));
    if (titleLabel != nullptr) {
        titleLabel->SetText(ui::StringConvert::UTF8ToT(userId));
    }
    LoadMessageHistory(userId);
}

void ChatForm::LoadMessageHistory(const std::string& userId)
{
    ui::VListBox* messageList = dynamic_cast<ui::VListBox*>(FindControl(_T("chat_message_list")));
    if (messageList == nullptr) {
        return;
    }
    messageList->RemoveAllItems();

    auto alive = m_alive;
    TencentImService::Instance().GetMessageHistory(userId, [this, messageList, alive, userId](int32_t code, const std::string& desc, const std::vector<TencentImService::ChatMessage>& messages) {
        if (!*alive || (userId != m_currentFriendId)) {
            return;
        }

        auto logger = spdlog::get("im_file_logger");
        if (code != 0) {
            if (logger) {
                logger->error("加载历史消息失败，UserID: {}, code: {}, desc: {}", userId, code, desc);
            }
            return;
        }

        messageList->RemoveAllItems();
        std::vector<TencentImService::ChatMessage> orderedMessages = messages;
        std::stable_sort(orderedMessages.begin(), orderedMessages.end(),
            [](const TencentImService::ChatMessage& left, const TencentImService::ChatMessage& right) {
                return left.timestamp < right.timestamp;
            });
        for (const TencentImService::ChatMessage& message : orderedMessages) {
            AppendChatMessage(message.fromSelf, message.sender, message.text);
        }
        const size_t messageCount = messageList->GetItemCount();
        if (messageCount > 0) {
            messageList->EnsureVisible(messageCount - 1);
        }
        messageList->EndDown();
        if (logger) {
            logger->info("历史消息加载完成，UserID: {}, 数量: {}", userId, messages.size());
        }
    });
}

void ChatForm::AppendChatMessage(bool fromSelf, const std::string& sender, const std::string& text)
{
    ui::VListBox* messageList = dynamic_cast<ui::VListBox*>(FindControl(_T("chat_message_list")));
    if (messageList == nullptr) {
        return;
    }

    ui::ListBoxItemH* item = new ui::ListBoxItemH(this);
    item->SetAttribute(_T("width"), _T("stretch"));
    item->SetAttribute(_T("height"), _T("40"));
    item->SetAttribute(_T("padding"), _T("0,2,0,2"));

    ui::Control* spacer = new ui::Control(this);
    spacer->SetAttribute(_T("width"), _T("stretch"));

    ui::Label* bubble = new ui::Label(this);
    std::string displayText;
    if (fromSelf) {
        displayText = std::string("我: ") + text;
    }
    else {
        displayText = (sender.empty() ? std::string("好友: ") : sender + ": ") + text;
    }
    bubble->SetAttribute(_T("width"), _T("auto"));
    bubble->SetAttribute(_T("height"), _T("auto"));
    bubble->SetAttribute(_T("padding"), _T("10,5,10,5"));
    bubble->SetAttribute(_T("border_round"), _T("8,8"));
    bubble->SetAttribute(_T("bkcolor"), fromSelf ? _T("#FFD9ECFD") : _T("#FFF2F4F7"));
    bubble->SetAttribute(_T("normal_text_color"), _T("#FF1F2329"));
    bubble->SetAttribute(_T("font"), _T("system_regular_12"));
    bubble->SetAttribute(_T("text_align"), _T("left,vcenter"));
    bubble->SetAttribute(_T("mouse_enabled"), _T("false"));
    bubble->SetAttribute(_T("text"), ui::StringConvert::UTF8ToT(displayText));

    if (fromSelf) {
        item->AddItem(spacer);
        item->AddItem(bubble);
    }
    else {
        item->AddItem(bubble);
        item->AddItem(spacer);
    }
    messageList->AddItem(item);
    const size_t messageCount = messageList->GetItemCount();
    if (messageCount > 0) {
        messageList->EnsureVisible(messageCount - 1);
    }
    messageList->EndDown();
}

void ChatForm::SendCurrentMessage()
{
    if (m_currentFriendId.empty()) {
        return;
    }

    ui::RichEdit2* messageEdit = dynamic_cast<ui::RichEdit2*>(FindControl(_T("message_edit")));
    if (messageEdit == nullptr) {
        return;
    }

    std::string text = ui::StringConvert::TToUTF8(messageEdit->GetText());
    const auto notSpace = [](unsigned char ch) {
        return std::isspace(ch) == 0;
    };
    text.erase(text.begin(), std::find_if(text.begin(), text.end(), notSpace));
    text.erase(std::find_if(text.rbegin(), text.rend(), notSpace).base(), text.end());
    if (text.empty()) {
        return;
    }

    messageEdit->SetText(_T(""));
    AppendChatMessage(true, m_selfUserId, text);
    EnsureConversationRow(m_currentFriendId, text);

    auto alive = m_alive;
    const std::string peerUserId = m_currentFriendId;
    const int result = TencentImService::Instance().SendTextMessage(peerUserId, text, [this, alive, peerUserId](int32_t code, const std::string& desc) {
        if (!*alive) {
            return;
        }
        auto logger = spdlog::get("im_file_logger");
        if (code != 0) {
            if (logger) {
                logger->error("发送消息失败，UserID: {}, code: {}, desc: {}", peerUserId, code, desc);
            }
        }
    });

    if (result != TIM_SUCC) {
        auto logger = spdlog::get("im_file_logger");
        if (logger) {
            logger->error("发送消息接口调用失败，UserID: {}, ret: {}", peerUserId, result);
        }
    }
}

void ChatForm::EnsureConversationRow(const std::string& userId, const std::string& text)
{
    ui::VListBox* conversationList = dynamic_cast<ui::VListBox*>(FindControl(_T("message_list")));
    if ((conversationList == nullptr) || userId.empty()) {
        return;
    }

    auto iter = m_conversationRows.find(userId);
    if (iter != m_conversationRows.end()) {
        if (iter->second != nullptr) {
            iter->second->SetText(ui::StringConvert::UTF8ToT(text));
        }
        return;
    }

    ui::ListBoxItemH* item = new ui::ListBoxItemH(this);
    item->SetAttribute(_T("width"), _T("stretch"));
    item->SetAttribute(_T("height"), _T("60"));
    item->SetAttribute(_T("margin"), _T("6,1,6,1"));
    item->SetAttribute(_T("padding"), _T("10,0,10,0"));
    item->SetAttribute(_T("border_round"), _T("6,6"));
    item->SetAttribute(_T("hovered_color"), _T("#FFF2F7FC"));
    item->SetAttribute(_T("pressed_color"), _T("#FFE6F1FB"));
    item->SetAttribute(_T("selected_normal_color"), _T("#FFE3F1FD"));
    item->SetAttribute(_T("fade_hovered"), _T("false"));

    ui::VBox* textBox = new ui::VBox(this);
    textBox->SetAttribute(_T("width"), _T("stretch"));
    textBox->SetAttribute(_T("height"), _T("stretch"));
    textBox->SetAttribute(_T("mouse_enabled"), _T("false"));

    ui::Label* idLabel = new ui::Label(this);
    idLabel->SetAttribute(_T("height"), _T("26"));
    idLabel->SetAttribute(_T("margin"), _T("0,6,0,0"));
    idLabel->SetAttribute(_T("valign"), _T("center"));
    idLabel->SetAttribute(_T("font"), _T("system_regular_13"));
    idLabel->SetAttribute(_T("normal_text_color"), _T("#FF1F2329"));
    idLabel->SetAttribute(_T("mouse_enabled"), _T("false"));
    idLabel->SetAttribute(_T("text"), ui::StringConvert::UTF8ToT(userId));

    ui::Label* snippetLabel = new ui::Label(this);
    snippetLabel->SetAttribute(_T("height"), _T("20"));
    snippetLabel->SetAttribute(_T("font"), _T("system_regular_11"));
    snippetLabel->SetAttribute(_T("normal_text_color"), _T("#FF9AA1AC"));
    snippetLabel->SetAttribute(_T("mouse_enabled"), _T("false"));
    snippetLabel->SetAttribute(_T("text"), ui::StringConvert::UTF8ToT(text));

    textBox->AddItem(idLabel);
    textBox->AddItem(snippetLabel);
    item->AddItem(textBox);

    auto alive = m_alive;
    item->AttachClick([this, userId, alive](const ui::EventArgs&) {
        if (!*alive) {
            return true;
        }
        SelectFriend(userId);
        return true;
    });

    conversationList->AddItemAt(item, 0);
    m_conversationRows[userId] = snippetLabel;
}

void ChatForm::RefreshFriendList()
{
    ui::VListBox* friendList = dynamic_cast<ui::VListBox*>(FindControl(_T("friend_list")));
    if (friendList == nullptr) {
        return;
    }

    friendList->RemoveAllItems();

    auto alive = m_alive;
    TencentImService::Instance().GetFriendList([this, friendList, alive](int32_t code, const std::string& desc, const std::vector<std::string>& friendIds) {
        if (!*alive) {
            return;
        }

        auto logger = spdlog::get("im_file_logger");
        if (code != 0) {
            if (logger) {
                logger->error("加载好友列表失败，code: {}, desc: {}", code, desc);
            }
            return;
        }

        friendList->RemoveAllItems();
        for (const std::string& userId : friendIds) {
            ui::ListBoxItemH* item = new ui::ListBoxItemH(this);
            item->SetAttribute(_T("width"), _T("stretch"));
            item->SetAttribute(_T("height"), _T("40"));
            item->SetAttribute(_T("padding"), _T("14,0,14,0"));
            item->SetAttribute(_T("border_round"), _T("6,6"));
            item->SetAttribute(_T("hovered_color"), _T("#FFF2F7FC"));
            item->SetAttribute(_T("pressed_color"), _T("#FFE6F1FB"));
            item->SetAttribute(_T("selected_normal_color"), _T("#FFE3F1FD"));
            item->SetAttribute(_T("fade_hovered"), _T("false"));

            ui::Label* userIdLabel = new ui::Label(this);
            userIdLabel->SetAttribute(_T("width"), _T("stretch"));
            userIdLabel->SetAttribute(_T("valign"), _T("center"));
            userIdLabel->SetAttribute(_T("font"), _T("system_regular_13"));
            userIdLabel->SetAttribute(_T("normal_text_color"), _T("#FF1F2329"));
            userIdLabel->SetAttribute(_T("mouse_enabled"), _T("false"));
            userIdLabel->SetAttribute(_T("text"), ui::StringConvert::UTF8ToT(userId));
            item->AttachClick([this, userId, alive](const ui::EventArgs&) {
                if (!*alive) {
                    return true;
                }
                SelectFriend(userId);
                return true;
            });
            item->AddItem(userIdLabel);
            friendList->AddItem(item);
        }

        if (logger) {
            logger->info("好友列表加载完成，好友数量: {}", friendIds.size());
        }
    });
}
#endif

#ifdef CXIM_IMS_CORE_SDK_VERSION

namespace {

std::string ImsTrim(const std::string& value)
{
    const auto notSpace = [](unsigned char ch) {
        return (ch != ' ') && (ch != '\t') && (ch != '\r') && (ch != '\n');
    };
    std::string result = value;
    result.erase(result.begin(), std::find_if(result.begin(), result.end(), notSpace));
    result.erase(std::find_if(result.rbegin(), result.rend(), notSpace).base(), result.end());
    return result;
}

std::string ImsFormatClock(uint64_t msEpoch)
{
    if (msEpoch == 0) {
        return std::string();
    }
    const time_t sec = static_cast<time_t>(msEpoch / 1000);
    struct tm timeInfo;
#ifdef _MSC_VER
    localtime_s(&timeInfo, &sec);
#else
    struct tm* t = localtime(&sec);
    if (t == nullptr) {
        return std::string();
    }
    timeInfo = *t;
#endif
    char buffer[16];
    std::snprintf(buffer, sizeof(buffer), "%02d:%02d", timeInfo.tm_hour, timeInfo.tm_min);
    return std::string(buffer);
}

uint64_t ImsSelfId()
{
    return ImsSession::Instance().UserId();
}

} // namespace

void ChatForm::PostToUi(std::function<void()> task)
{
    auto alive = m_alive;
    ui::GlobalManager::Instance().Thread().PostTask(ui::kThreadUI, [alive, task = std::move(task)]() {
        if ((alive != nullptr) && *alive) {
            task();
        }
    });
}

void ChatForm::RunAsync(std::function<void()> work, std::function<void()> done)
{
    std::thread([this, work = std::move(work), done = std::move(done)]() mutable {
        work();
        PostToUi(std::move(done));
    }).detach();
}

void ChatForm::InitIms()
{
    const ImsSession& session = ImsSession::Instance();
    if (!session.IsLoggedIn()) {
        return;
    }

    auto logger = spdlog::get("im_file_logger");
    if (logger) {
        logger->info("IMS 模式初始化，userId: {}, username: {}", session.UserId(), session.Username());
    }

    // 1) 顶部自身信息：用户名 + UserID（IMS 登录响应没有昵称字段）
    ui::Label* selfLabel = dynamic_cast<ui::Label*>(FindControl(_T("self_user_label")));
    if (selfLabel != nullptr) {
        const std::string text = session.Username() + " " + std::to_string(session.UserId());
        selfLabel->SetText(ui::StringConvert::UTF8ToT(text));
    }

    // 2) 好友/群聊页签切换
    ui::TabBox* contactsListPages = dynamic_cast<ui::TabBox*>(FindControl(_T("contacts_list_pages")));
    ui::Option* tabFriend = dynamic_cast<ui::Option*>(FindControl(_T("tab_friend")));
    ui::Option* tabGroup = dynamic_cast<ui::Option*>(FindControl(_T("tab_group")));
    if (contactsListPages != nullptr) {
        contactsListPages->SelectItem(_T("friend_list"));
    }
    if (tabFriend != nullptr) {
        tabFriend->Selected(true, false);
        tabFriend->AttachClick([contactsListPages](const ui::EventArgs&) {
            if (contactsListPages != nullptr) {
                contactsListPages->SelectItem(_T("friend_list"));
            }
            return true;
        });
    }
    if (tabGroup != nullptr) {
        tabGroup->Selected(false, false);
        tabGroup->AttachClick([contactsListPages](const ui::EventArgs&) {
            if (contactsListPages != nullptr) {
                contactsListPages->SelectItem(_T("group_list"));
            }
            return true;
        });
    }

    // 3) TCP 客户端：回调 + 启动
    TcpClient& tcp = TcpClient::Instance();
    tcp.SetMessageCallback([this](const ImsMessage& msg) { OnTcpMessage(msg); });
    tcp.SetLoginCallback([this](bool success, const std::string& error, bool firstLogin) {
        OnTcpLoginResult(success, error, firstLogin);
    });
    tcp.SetDisconnectedCallback([this]() { OnTcpDisconnected(); });
    tcp.Start(session.Token());

    // 4) 初始好友列表与申请列表
    RefreshFriendList();
    RefreshFriendRequests();

    // 5) 发送按钮
    ui::Button* btnSend = dynamic_cast<ui::Button*>(FindControl(_T("btn_send")));
    if (btnSend != nullptr) {
        btnSend->AttachClick([this](const ui::EventArgs&) {
            SendCurrentMessage();
            return true;
        });
    }

    // 6) 聊天区“更多”按钮 → 当前好友操作面板
    ui::Button* btnMore = dynamic_cast<ui::Button*>(FindControl(_T("btn_more")));
    if (btnMore != nullptr) {
        btnMore->AttachClick([this](const ui::EventArgs&) {
            if (m_currentPeer != 0) {
                ShowFriendOpPanel(m_currentPeer);
            }
            return true;
        });
    }

    // 7) 添加好友面板
    ui::Button* btnAddContact = dynamic_cast<ui::Button*>(FindControl(_T("btn_add_contact")));
    ui::Box* addFriendPanel = dynamic_cast<ui::Box*>(FindControl(_T("add_friend_panel")));
    ui::Button* btnCloseAddFriend = dynamic_cast<ui::Button*>(FindControl(_T("btn_close_add_friend")));
    ui::RichEdit2* addFriendSearchEdit = dynamic_cast<ui::RichEdit2*>(FindControl(_T("add_friend_search_edit")));
    ui::Button* btnSearchFriend = dynamic_cast<ui::Button*>(FindControl(_T("btn_search_friend")));
    ui::Label* addFriendHint = dynamic_cast<ui::Label*>(FindControl(_T("add_friend_hint")));
    ui::Box* addFriendResult = dynamic_cast<ui::Box*>(FindControl(_T("add_friend_result")));
    ui::Label* addFriendUserId = dynamic_cast<ui::Label*>(FindControl(_T("add_friend_user_id")));
    ui::Button* btnAddFriend = dynamic_cast<ui::Button*>(FindControl(_T("btn_add_friend")));
    ui::Label* addFriendAdded = dynamic_cast<ui::Label*>(FindControl(_T("add_friend_added")));
    auto searchedUserId = std::make_shared<uint64_t>(0);

    if ((btnAddContact != nullptr) && (addFriendPanel != nullptr)) {
        btnAddContact->AttachClick([=](const ui::EventArgs&) {
            HideOverlayPanels();
            addFriendPanel->SetVisible(true);
            if (addFriendSearchEdit != nullptr) {
                addFriendSearchEdit->SetText(_T(""));
            }
            if (addFriendResult != nullptr) {
                addFriendResult->SetVisible(false);
            }
            if (addFriendHint != nullptr) {
                addFriendHint->SetText(_T(""));
            }
            if (btnAddFriend != nullptr) {
                btnAddFriend->SetVisible(true);
                btnAddFriend->SetEnabled(true);
                btnAddFriend->SetText(_T("添加"));
            }
            if (addFriendAdded != nullptr) {
                addFriendAdded->SetVisible(false);
            }
            *searchedUserId = 0;
            return true;
        });
    }

    if ((btnCloseAddFriend != nullptr) && (addFriendPanel != nullptr)) {
        btnCloseAddFriend->AttachClick([addFriendPanel](const ui::EventArgs&) {
            addFriendPanel->SetVisible(false);
            return true;
        });
    }

    if (btnSearchFriend != nullptr) {
        btnSearchFriend->AttachClick([=](const ui::EventArgs&) {
            if (addFriendSearchEdit == nullptr) {
                return true;
            }
            const std::string keyword = ImsTrim(ui::StringConvert::TToUTF8(addFriendSearchEdit->GetText()));
            if (keyword.empty()) {
                if (addFriendHint != nullptr) {
                    addFriendHint->SetText(_T("请输入用户名/昵称/用户ID"));
                }
                return true;
            }
            if (addFriendHint != nullptr) {
                addFriendHint->SetText(_T("搜索中..."));
            }
            if (addFriendResult != nullptr) {
                addFriendResult->SetVisible(false);
            }
            *searchedUserId = 0;

            auto ctx = std::make_shared<std::pair<HttpClient::ApiResult, std::vector<SearchUser>>>();
            RunAsync([ctx, keyword]() {
                ctx->first = HttpClient::Instance().SearchUsers(keyword, 20, ctx->second);
            }, [=]() {
                const HttpClient::ApiResult& result = ctx->first;
                if (!result.ok) {
                    if (result.unauthorized) {
                        HandleApiUnauthorized(result.error);
                        return;
                    }
                    if (addFriendHint != nullptr) {
                        addFriendHint->SetText(ui::StringConvert::UTF8ToT(result.error));
                    }
                    return;
                }
                if (ctx->second.empty()) {
                    if (addFriendHint != nullptr) {
                        addFriendHint->SetText(_T("未找到该用户"));
                    }
                    return;
                }
                const SearchUser& user = ctx->second.front();
                *searchedUserId = user.userId;
                if (addFriendHint != nullptr) {
                    const std::string hint = "找到 " + std::to_string(ctx->second.size()) + " 个用户，显示第一个";
                    addFriendHint->SetText(ui::StringConvert::UTF8ToT(hint));
                }
                if (addFriendUserId != nullptr) {
                    const std::string text = user.DisplayName() + " (" + std::to_string(user.userId) + ")";
                    addFriendUserId->SetText(ui::StringConvert::UTF8ToT(text));
                }
                if (addFriendResult != nullptr) {
                    addFriendResult->SetVisible(true);
                }
                if (btnAddFriend != nullptr) {
                    btnAddFriend->SetVisible(true);
                    btnAddFriend->SetEnabled(true);
                }
                if (addFriendAdded != nullptr) {
                    addFriendAdded->SetVisible(false);
                }
            });
            return true;
        });
    }

    if (btnAddFriend != nullptr) {
        btnAddFriend->AttachClick([=](const ui::EventArgs&) {
            if (*searchedUserId == 0) {
                if (addFriendHint != nullptr) {
                    addFriendHint->SetText(_T("请先搜索用户"));
                }
                return true;
            }
            btnAddFriend->SetEnabled(false);
            if (addFriendHint != nullptr) {
                addFriendHint->SetText(_T("发送申请中..."));
            }
            const uint64_t toUserId = *searchedUserId;
            auto ctx = std::make_shared<std::pair<HttpClient::ApiResult, FriendRequestInfo>>();
            RunAsync([ctx, toUserId]() {
                ctx->first = HttpClient::Instance().SendFriendRequest(toUserId, "你好，我想加你为好友", ctx->second);
            }, [=]() {
                const HttpClient::ApiResult& result = ctx->first;
                if (result.unauthorized) {
                    HandleApiUnauthorized(result.error);
                    return;
                }
                if (result.ok) {
                    if (btnAddFriend != nullptr) {
                        btnAddFriend->SetVisible(false);
                    }
                    if (addFriendAdded != nullptr) {
                        addFriendAdded->SetVisible(true);
                    }
                    if (addFriendHint != nullptr) {
                        addFriendHint->SetText(_T("申请已发送，等待对方验证"));
                    }
                    auto log = spdlog::get("im_file_logger");
                    if (log) {
                        log->info("已发送好友申请，toUserId: {}, requestId: {}", toUserId, ctx->second.requestId);
                    }
                }
                else {
                    // 服务端业务校验失败：文案原样提示（不能加自己/重复申请/已被拉黑等）
                    btnAddFriend->SetEnabled(true);
                    if (addFriendHint != nullptr) {
                        addFriendHint->SetText(ui::StringConvert::UTF8ToT(result.error));
                    }
                }
            });
            return true;
        });
    }

    // 8) 新朋友入口与申请列表面板
    ui::Button* btnNewFriends = dynamic_cast<ui::Button*>(FindControl(_T("btn_new_friends")));
    ui::Box* requestPanel = dynamic_cast<ui::Box*>(FindControl(_T("request_panel")));
    ui::Button* btnCloseRequests = dynamic_cast<ui::Button*>(FindControl(_T("btn_close_requests")));
    if ((btnNewFriends != nullptr) && (requestPanel != nullptr)) {
        btnNewFriends->AttachClick([=](const ui::EventArgs&) {
            HideOverlayPanels();
            requestPanel->SetVisible(true);
            RefreshFriendRequests();
            return true;
        });
    }
    if ((btnCloseRequests != nullptr) && (requestPanel != nullptr)) {
        btnCloseRequests->AttachClick([requestPanel](const ui::EventArgs&) {
            requestPanel->SetVisible(false);
            return true;
        });
    }

    // 9) 好友操作面板
    ui::Box* friendOpPanel = dynamic_cast<ui::Box*>(FindControl(_T("friend_op_panel")));
    ui::Button* btnCloseFriendOp = dynamic_cast<ui::Button*>(FindControl(_T("btn_close_friend_op")));
    if ((btnCloseFriendOp != nullptr) && (friendOpPanel != nullptr)) {
        btnCloseFriendOp->AttachClick([friendOpPanel](const ui::EventArgs&) {
            friendOpPanel->SetVisible(false);
            return true;
        });
    }

    ui::Button* btnSaveRemark = dynamic_cast<ui::Button*>(FindControl(_T("btn_save_remark")));
    if (btnSaveRemark != nullptr) {
        btnSaveRemark->AttachClick([this](const ui::EventArgs&) {
            if (m_opPanelFriendId == 0) {
                return true;
            }
            ui::RichEdit2* remarkEdit = dynamic_cast<ui::RichEdit2*>(FindControl(_T("remark_edit")));
            ui::Label* hint = dynamic_cast<ui::Label*>(FindControl(_T("friend_op_hint")));
            const std::string remark = (remarkEdit != nullptr)
                ? ImsTrim(ui::StringConvert::TToUTF8(remarkEdit->GetText())) : std::string();
            if (remark.size() > 64) {
                if (hint != nullptr) {
                    hint->SetText(_T("备注最大 64 字节"));
                }
                return true;
            }
            const uint64_t friendId = m_opPanelFriendId;
            auto ctx = std::make_shared<HttpClient::ApiResult>();
            RunAsync([ctx, friendId, remark]() {
                *ctx = HttpClient::Instance().SetFriendRemark(friendId, remark);
            }, [this, ctx, hint, friendId]() {
                if (ctx->unauthorized) {
                    HandleApiUnauthorized(ctx->error);
                    return;
                }
                if (hint != nullptr) {
                    hint->SetText(ui::StringConvert::UTF8ToT(ctx->ok ? std::string("备注已保存") : ctx->error));
                }
                if (ctx->ok) {
                    RefreshFriendList();
                }
            });
            return true;
        });
    }

    ui::Button* btnDelFriend = dynamic_cast<ui::Button*>(FindControl(_T("btn_del_friend")));
    if (btnDelFriend != nullptr) {
        btnDelFriend->AttachClick([this](const ui::EventArgs&) {
            if (m_opPanelFriendId == 0) {
                return true;
            }
            ui::Label* hint = dynamic_cast<ui::Label*>(FindControl(_T("friend_op_hint")));
            const uint64_t friendId = m_opPanelFriendId;
            auto ctx = std::make_shared<HttpClient::ApiResult>();
            RunAsync([ctx, friendId]() {
                *ctx = HttpClient::Instance().DeleteFriend(friendId);
            }, [this, ctx, hint, friendId]() {
                if (ctx->unauthorized) {
                    HandleApiUnauthorized(ctx->error);
                    return;
                }
                if (hint != nullptr) {
                    hint->SetText(ui::StringConvert::UTF8ToT(ctx->ok ? std::string("已删除好友") : ctx->error));
                }
                if (ctx->ok) {
                    if (m_currentPeer == friendId) {
                        m_currentPeer = 0;
                        ui::Label* titleLabel = dynamic_cast<ui::Label*>(FindControl(_T("chat_title_label")));
                        if (titleLabel != nullptr) {
                            titleLabel->SetText(_T("请选择好友"));
                        }
                        ui::VListBox* chatList = dynamic_cast<ui::VListBox*>(FindControl(_T("chat_message_list")));
                        if (chatList != nullptr) {
                            chatList->RemoveAllItems();
                        }
                    }
                    RefreshFriendList();
                    if (hint != nullptr) {
                        auto opPanel = dynamic_cast<ui::Box*>(FindControl(_T("friend_op_panel")));
                        if (opPanel != nullptr) {
                            opPanel->SetVisible(false);
                        }
                    }
                }
            });
            return true;
        });
    }

    ui::Button* btnBlockFriend = dynamic_cast<ui::Button*>(FindControl(_T("btn_block_friend")));
    if (btnBlockFriend != nullptr) {
        btnBlockFriend->AttachClick([this](const ui::EventArgs&) {
            if (m_opPanelFriendId == 0) {
                return true;
            }
            ui::Label* hint = dynamic_cast<ui::Label*>(FindControl(_T("friend_op_hint")));
            const uint64_t friendId = m_opPanelFriendId;
            auto ctx = std::make_shared<HttpClient::ApiResult>();
            RunAsync([ctx, friendId]() {
                *ctx = HttpClient::Instance().BlockFriend(friendId);
            }, [this, ctx, hint, friendId]() {
                if (ctx->unauthorized) {
                    HandleApiUnauthorized(ctx->error);
                    return;
                }
                if (hint != nullptr) {
                    hint->SetText(ui::StringConvert::UTF8ToT(ctx->ok ? std::string("已拉黑（对方不会收到通知）") : ctx->error));
                }
                if (ctx->ok) {
                    if (m_currentPeer == friendId) {
                        m_currentPeer = 0;
                    }
                    RefreshFriendList();
                }
            });
            return true;
        });
    }
}

// ---------------------------------------------------------------------------
// TCP 事件入口（网络线程 → UI 线程）
// ---------------------------------------------------------------------------
void ChatForm::OnTcpLoginResult(bool success, const std::string& error, bool firstLogin)
{
    PostToUi([this, success, error, firstLogin]() {
        auto logger = spdlog::get("im_file_logger");
        if (success) {
            if (logger) {
                logger->info("TCP 登录成功（{}）", firstLogin ? "首次" : "重连");
            }
            // 登录后全量拉取好友与申请，保证最终一致（列表变化事件不持久化）
            RefreshFriendList();
            RefreshFriendRequests();
        }
        else {
            if (logger) {
                logger->error("TCP 登录失败: {}", error);
            }
            HandleApiUnauthorized("登录已过期，请重新登录");
        }
    });
}

void ChatForm::OnTcpDisconnected()
{
    PostToUi([]() {
        auto logger = spdlog::get("im_file_logger");
        if (logger) {
            logger->warn("TCP 连接断开（可能是网络异常或账号在其它设备登录），客户端自动重连中");
        }
    });
}

void ChatForm::OnTcpMessage(const ImsMessage& msg)
{
    // 投递回执在线程上直接发送，无需切回 UI 线程
    if ((msg.type == static_cast<int>(ImsMsgType::Text)) || (msg.type == static_cast<int>(ImsMsgType::Image))) {
        if ((msg.from != 0) && (msg.conversation_id != 0) && (msg.seq != 0)) {
            ImsMessage ack;
            ack.type = static_cast<int>(ImsMsgType::Ack);
            ack.conversation_id = msg.conversation_id;
            ack.seq = msg.seq;
            ack.content = "delivered";
            TcpClient::Instance().Send(ack);
        }
    }

    ImsMessage copy = msg;
    PostToUi([this, copy]() {
        switch (copy.type) {
        case static_cast<int>(ImsMsgType::Text):
        case static_cast<int>(ImsMsgType::Image):
            HandleChatMessage(copy, false);
            break;
        case static_cast<int>(ImsMsgType::HistoryResp):
            HandleHistoryResp(copy);
            break;
        case static_cast<int>(ImsMsgType::SendAck):
            HandleSendAck(copy);
            break;
        case static_cast<int>(ImsMsgType::Error):
            HandleTcpError(copy);
            break;
        case static_cast<int>(ImsMsgType::FriendRequest):
        case static_cast<int>(ImsMsgType::FriendRequestResult):
        case static_cast<int>(ImsMsgType::FriendListChanged):
            HandleFriendEvent(copy);
            break;
        default:
            break;
        }
    });
}

// ---------------------------------------------------------------------------
// 单聊
// ---------------------------------------------------------------------------
void ChatForm::HandleChatMessage(const ImsMessage& msg, bool fromHistory)
{
    (void)fromHistory;
    const uint64_t selfId = ImsSelfId();
    if ((msg.from == 0) || (msg.from == selfId)) {
        return;
    }
    const uint64_t peerId = msg.from;
    PeerState& state = m_peers[peerId];
    const std::pair<uint64_t, uint64_t> key(msg.conversation_id, msg.seq);
    if (state.seen.count(key) != 0) {
        return; // conversation_id + seq 去重
    }
    state.seen.insert(key);

    PeerState::Item item;
    item.fromSelf = false;
    item.sender = PeerDisplayName(peerId);
    item.text = msg.content;
    item.conversation_id = msg.conversation_id;
    item.seq = msg.seq;
    item.ts = msg.ts;
    state.items.push_back(item);
    if ((state.minSeq == 0) || (msg.seq < state.minSeq)) {
        state.minSeq = msg.seq;
    }

    EnsureConversationRow(peerId, item.text);

    if (peerId == m_currentPeer) {
        AppendChatBubble(false, item.sender, item.text);
        // 正在查看该会话：立即确认已读
        ImsMessage readAck;
        readAck.type = static_cast<int>(ImsMsgType::Ack);
        readAck.conversation_id = msg.conversation_id;
        readAck.seq = msg.seq;
        TcpClient::Instance().Send(readAck);
    }
    else {
        state.unread++;
        RenderConversationList();
    }
}

void ChatForm::HandleSendAck(const ImsMessage& msg)
{
    if (m_pendingSends.empty()) {
        return;
    }
    const PendingSend pending = m_pendingSends.front();
    m_pendingSends.erase(m_pendingSends.begin());

    if (pending.bubble != nullptr) {
        const std::string suffix = (msg.content == "duplicate") ? "  [已发送(去重)]" : "  [已发送]";
        pending.bubble->SetText(ui::StringConvert::UTF8ToT(pending.text + suffix));
    }

    PeerState& state = m_peers[pending.peerId];
    const std::pair<uint64_t, uint64_t> key(msg.conversation_id, msg.seq);
    if (state.seen.count(key) == 0) {
        state.seen.insert(key);
        PeerState::Item item;
        item.fromSelf = true;
        item.text = pending.text;
        item.conversation_id = msg.conversation_id;
        item.seq = msg.seq;
        item.ts = msg.ts;
        state.items.push_back(item);
        if ((state.minSeq == 0) || (msg.seq < state.minSeq)) {
            state.minSeq = msg.seq;
        }
    }
    auto logger = spdlog::get("im_file_logger");
    if (logger) {
        logger->info("SendAck: serverMsgId={}, conv={}, seq={}, {}", msg.msg_id, msg.conversation_id, msg.seq, msg.content);
    }
}

void ChatForm::HandleHistoryResp(const ImsMessage& msg)
{
    if (m_historyFifo.empty()) {
        auto logger = spdlog::get("im_file_logger");
        if (logger) {
            logger->warn("收到 HistoryResp 但没有等待中的历史请求，忽略");
        }
        return;
    }
    const uint64_t peerId = m_historyFifo.front();
    m_historyFifo.erase(m_historyFifo.begin());

    std::vector<ImsHistoryMessage> history;
    std::string err;
    if (!ParseImsHistory(msg.content, history, err)) {
        auto logger = spdlog::get("im_file_logger");
        if (logger) {
            logger->error("历史消息解析失败: {}", err);
        }
        return;
    }
    std::stable_sort(history.begin(), history.end(),
        [](const ImsHistoryMessage& left, const ImsHistoryMessage& right) {
            return left.seq < right.seq;
        });

    const uint64_t selfId = ImsSelfId();
    PeerState& state = m_peers[peerId];
    bool hasNewItems = false;
    for (const ImsHistoryMessage& m : history) {
        const std::pair<uint64_t, uint64_t> key(m.conversation_id, m.seq);
        if (state.seen.count(key) != 0) {
            continue; // 与实时消息去重
        }
        state.seen.insert(key);
        PeerState::Item item;
        item.fromSelf = (m.from == selfId);
        item.sender = item.fromSelf ? std::string() : PeerDisplayName(peerId);
        item.text = m.content;
        item.conversation_id = m.conversation_id;
        item.seq = m.seq;
        item.ts = m.ts;
        state.items.push_back(item);
        if ((state.minSeq == 0) || (m.seq < state.minSeq)) {
            state.minSeq = m.seq;
        }
        hasNewItems = true;
    }
    state.historyLoaded = true;

    if (peerId == m_currentPeer) {
        RenderPeerMessages();
        MarkPeerRead(peerId);
    }
    else if (hasNewItems && !state.items.empty()) {
        EnsureConversationRow(peerId, state.items.back().text);
    }

    auto logger = spdlog::get("im_file_logger");
    if (logger) {
        logger->info("历史消息加载完成，peer: {}, 本页: {} 条", peerId, history.size());
    }
}

void ChatForm::HandleTcpError(const ImsMessage& msg)
{
    auto logger = spdlog::get("im_file_logger");
    if (logger) {
        logger->error("服务端错误通知: {}", msg.content);
    }
    if (!m_pendingSends.empty()) {
        const PendingSend pending = m_pendingSends.front();
        m_pendingSends.erase(m_pendingSends.begin());
        if (pending.bubble != nullptr) {
            const std::string text = pending.text + "  [发送失败: " + msg.content + "]";
            pending.bubble->SetText(ui::StringConvert::UTF8ToT(text));
        }
    }
}

void ChatForm::MarkPeerRead(uint64_t peerId)
{
    auto iter = m_peers.find(peerId);
    if (iter == m_peers.end()) {
        return;
    }
    PeerState& state = iter->second;
    state.unread = 0;
    uint64_t conv = 0;
    uint64_t maxSeq = 0;
    for (const PeerState::Item& item : state.items) {
        if (!item.fromSelf && (item.conversation_id != 0) && (item.seq > maxSeq)) {
            conv = item.conversation_id;
            maxSeq = item.seq;
        }
    }
    if ((conv != 0) && (maxSeq != 0)) {
        ImsMessage readAck;
        readAck.type = static_cast<int>(ImsMsgType::Ack);
        readAck.conversation_id = conv;
        readAck.seq = maxSeq;
        TcpClient::Instance().Send(readAck);
    }
    RenderConversationList();
}

void ChatForm::RequestHistory(uint64_t peerId, uint64_t beforeSeq)
{
    ImsMessage req;
    req.type = static_cast<int>(ImsMsgType::HistoryReq);
    req.to = peerId;
    req.seq = beforeSeq;
    req.content = "20";
    m_historyFifo.push_back(peerId);
    TcpClient::Instance().Send(req);
}

ui::Label* ChatForm::AppendChatBubbleWithLabel(bool fromSelf, const std::string& sender, const std::string& text)
{
    ui::VListBox* messageList = dynamic_cast<ui::VListBox*>(FindControl(_T("chat_message_list")));
    if (messageList == nullptr) {
        return nullptr;
    }

    ui::ListBoxItemH* item = new ui::ListBoxItemH(this);
    item->SetAttribute(_T("width"), _T("stretch"));
    item->SetAttribute(_T("height"), _T("auto"));
    item->SetAttribute(_T("padding"), _T("0,2,0,2"));

    ui::Control* spacer = new ui::Control(this);
    spacer->SetAttribute(_T("width"), _T("stretch"));
    spacer->SetAttribute(_T("height"), _T("auto"));

    ui::Label* bubble = new ui::Label(this);
    std::string displayText = text;
    if (!sender.empty() && !fromSelf) {
        displayText = sender + ": " + text;
    }
    bubble->SetAttribute(_T("width"), _T("auto"));
    bubble->SetAttribute(_T("height"), _T("auto"));
    bubble->SetAttribute(_T("padding"), _T("10,5,10,5"));
    bubble->SetAttribute(_T("border_round"), _T("8,8"));
    bubble->SetAttribute(_T("bkcolor"), fromSelf ? _T("#FFD9ECFD") : _T("#FFF2F4F7"));
    bubble->SetAttribute(_T("normal_text_color"), _T("#FF1F2329"));
    bubble->SetAttribute(_T("font"), _T("system_regular_12"));
    bubble->SetAttribute(_T("text_align"), _T("left,vcenter"));
    bubble->SetAttribute(_T("mouse_enabled"), _T("false"));
    bubble->SetAttribute(_T("end_ellipsis_char"), _T(""));
    bubble->SetAttribute(_T("single_line"), _T("false"));
    bubble->SetAttribute(_T("text"), ui::StringConvert::UTF8ToT(displayText));

    if (fromSelf) {
        item->AddItem(spacer);
        item->AddItem(bubble);
    }
    else {
        item->AddItem(bubble);
        item->AddItem(spacer);
    }
    messageList->AddItem(item);
    const size_t count = messageList->GetItemCount();
    if (count > 0) {
        messageList->EnsureVisible(count - 1);
    }
    messageList->EndDown();
    return bubble;
}

void ChatForm::AppendChatBubble(bool fromSelf, const std::string& sender, const std::string& text)
{
    AppendChatBubbleWithLabel(fromSelf, sender, text);
}

void ChatForm::RenderPeerMessages()
{
    ui::VListBox* messageList = dynamic_cast<ui::VListBox*>(FindControl(_T("chat_message_list")));
    if (messageList == nullptr) {
        return;
    }
    messageList->RemoveAllItems();
    if (m_currentPeer == 0) {
        return;
    }
    auto iter = m_peers.find(m_currentPeer);
    if (iter == m_peers.end()) {
        return;
    }
    PeerState& state = iter->second;
    std::vector<PeerState::Item> items = state.items;
    std::stable_sort(items.begin(), items.end(),
        [](const PeerState::Item& left, const PeerState::Item& right) {
            return left.seq < right.seq;
        });
    for (const PeerState::Item& item : items) {
        AppendChatBubble(item.fromSelf, item.sender, item.text);
    }
    const size_t count = messageList->GetItemCount();
    if (count > 0) {
        messageList->EnsureVisible(count - 1);
    }
    messageList->EndDown();
}

void ChatForm::SendCurrentMessage()
{
    if (m_currentPeer == 0) {
        auto logger = spdlog::get("im_file_logger");
        if (logger) {
            logger->warn("未选择好友，无法发送消息");
        }
        return;
    }
    ui::RichEdit2* messageEdit = dynamic_cast<ui::RichEdit2*>(FindControl(_T("message_edit")));
    if (messageEdit == nullptr) {
        return;
    }
    const std::string text = ImsTrim(ui::StringConvert::TToUTF8(messageEdit->GetText()));
    if (text.empty()) {
        return;
    }
    if (!TcpClient::Instance().IsLoggedIn()) {
        AppendChatBubble(true, std::string(), text + "  [发送失败: 未连接服务器]");
        return;
    }

    messageEdit->SetText(_T(""));

    if (m_nextTempMsgId == 0) {
        // 以毫秒时间戳为基数，避免重启后与历史 msg_id 冲突造成服务端误去重
        const auto nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        m_nextTempMsgId = static_cast<uint64_t>(nowMs);
    }
    const uint64_t tempMsgId = ++m_nextTempMsgId;

    ui::Label* bubble = AppendChatBubbleWithLabel(true, std::string(), text + "  [发送中...]");
    PendingSend pending;
    pending.bubble = bubble;
    pending.peerId = m_currentPeer;
    pending.text = text;
    m_pendingSends.push_back(pending);

    ImsMessage msg;
    msg.type = static_cast<int>(ImsMsgType::Text);
    msg.to = m_currentPeer;
    msg.msg_id = tempMsgId;
    msg.content = text;
    if (!TcpClient::Instance().Send(msg)) {
        HandleTcpError(ImsMessage());
    }
    EnsureConversationRow(m_currentPeer, text);
}

// ---------------------------------------------------------------------------
// 会话列表
// ---------------------------------------------------------------------------
void ChatForm::EnsureConversationRow(uint64_t peerId, const std::string& text)
{
    auto iter = std::find(m_conversationOrder.begin(), m_conversationOrder.end(), peerId);
    if (iter != m_conversationOrder.end()) {
        if (iter != m_conversationOrder.begin()) {
            m_conversationOrder.erase(iter);
            m_conversationOrder.insert(m_conversationOrder.begin(), peerId);
        }
    }
    else {
        m_conversationOrder.insert(m_conversationOrder.begin(), peerId);
    }
    m_lastSnippet[peerId] = text;
    RenderConversationList();
}

void ChatForm::RenderConversationList()
{
    ui::VListBox* conversationList = dynamic_cast<ui::VListBox*>(FindControl(_T("message_list")));
    if (conversationList == nullptr) {
        return;
    }
    conversationList->RemoveAllItems();

    const uint64_t selfId = ImsSelfId();
    for (uint64_t peerId : m_conversationOrder) {
        auto stateIter = m_peers.find(peerId);
        if (stateIter == m_peers.end()) {
            continue;
        }
        PeerState& state = stateIter->second;
        std::string snippet;
        std::string clock;
        if (!state.items.empty()) {
            const PeerState::Item& last = state.items.back();
            snippet = (last.fromSelf ? "我: " : "") + last.text;
            clock = ImsFormatClock(last.ts);
        }
        auto snippetIter = m_lastSnippet.find(peerId);
        if (snippet.empty() && snippetIter != m_lastSnippet.end()) {
            snippet = snippetIter->second;
        }

        ui::ListBoxItemH* item = new ui::ListBoxItemH(this);
        item->SetAttribute(_T("width"), _T("stretch"));
        item->SetAttribute(_T("height"), _T("60"));
        item->SetAttribute(_T("margin"), _T("6,1,6,1"));
        item->SetAttribute(_T("padding"), _T("10,0,10,0"));
        item->SetAttribute(_T("border_round"), _T("6,6"));
        item->SetAttribute(_T("hovered_color"), _T("#FFF2F7FC"));
        item->SetAttribute(_T("pressed_color"), _T("#FFE6F1FB"));
        item->SetAttribute(_T("selected_normal_color"), _T("#FFE3F1FD"));
        item->SetAttribute(_T("fade_hovered"), _T("false"));

        ui::VBox* textBox = new ui::VBox(this);
        textBox->SetAttribute(_T("width"), _T("stretch"));
        textBox->SetAttribute(_T("height"), _T("stretch"));
        textBox->SetAttribute(_T("mouse_enabled"), _T("false"));

        std::string nameText = PeerDisplayName(peerId);
        if (state.unread > 0) {
            nameText += " (" + std::to_string(state.unread) + ")";
        }
        ui::Label* nameLabel = new ui::Label(this);
        nameLabel->SetAttribute(_T("height"), _T("26"));
        nameLabel->SetAttribute(_T("margin"), _T("0,6,0,0"));
        nameLabel->SetAttribute(_T("valign"), _T("center"));
        nameLabel->SetAttribute(_T("font"), _T("system_regular_13"));
        nameLabel->SetAttribute(_T("normal_text_color"), state.unread > 0 ? _T("#FFE0483D") : _T("#FF1F2329"));
        nameLabel->SetAttribute(_T("mouse_enabled"), _T("false"));
        nameLabel->SetAttribute(_T("text"), ui::StringConvert::UTF8ToT(nameText));

        ui::Label* snippetLabel = new ui::Label(this);
        snippetLabel->SetAttribute(_T("height"), _T("20"));
        snippetLabel->SetAttribute(_T("font"), _T("system_regular_11"));
        snippetLabel->SetAttribute(_T("normal_text_color"), _T("#FF9AA1AC"));
        snippetLabel->SetAttribute(_T("mouse_enabled"), _T("false"));
        const std::string snippetText = snippet + (clock.empty() ? std::string() : ("  " + clock));
        snippetLabel->SetAttribute(_T("text"), ui::StringConvert::UTF8ToT(snippetText));

        textBox->AddItem(nameLabel);
        textBox->AddItem(snippetLabel);
        item->AddItem(textBox);

        item->AttachClick([this, peerId](const ui::EventArgs&) {
            SelectPeer(peerId);
            return true;
        });

        conversationList->AddItem(item);
        (void)selfId;
    }
}

// ---------------------------------------------------------------------------
// 好友列表（HTTP GET /api/friends）
// ---------------------------------------------------------------------------
const FriendInfo* ChatForm::FindFriend(uint64_t peerId) const
{
    auto iter = m_friends.find(peerId);
    return (iter != m_friends.end()) ? &iter->second : nullptr;
}

std::string ChatForm::PeerDisplayName(uint64_t peerId) const
{
    const FriendInfo* info = FindFriend(peerId);
    if (info != nullptr) {
        const std::string name = info->DisplayName();
        if (!name.empty()) {
            return name;
        }
    }
    auto iter = m_peers.find(peerId);
    if ((iter != m_peers.end()) && !iter->second.items.empty()) {
        // 保留已知 sender
    }
    return std::to_string(peerId);
}

void ChatForm::SelectPeer(uint64_t peerId)
{
    if (peerId == 0) {
        return;
    }
    m_currentPeer = peerId;

    ui::TabBox* mainPages = dynamic_cast<ui::TabBox*>(FindControl(_T("main_pages")));
    if (mainPages != nullptr) {
        mainPages->SelectItem(_T("chat_page"));
    }
    ui::Option* navChat = dynamic_cast<ui::Option*>(FindControl(_T("nav_chat")));
    if (navChat != nullptr) {
        navChat->Selected(true, false);
    }
    ui::Option* navContacts = dynamic_cast<ui::Option*>(FindControl(_T("nav_contacts")));
    if (navContacts != nullptr) {
        navContacts->Selected(false, false);
    }

    ui::Label* titleLabel = dynamic_cast<ui::Label*>(FindControl(_T("chat_title_label")));
    if (titleLabel != nullptr) {
        const std::string text = PeerDisplayName(peerId) + " (" + std::to_string(peerId) + ")";
        titleLabel->SetText(ui::StringConvert::UTF8ToT(text));
    }

    RenderPeerMessages();
    RequestHistory(peerId, 0);
    MarkPeerRead(peerId);
}

void ChatForm::RefreshFriendList()
{
    auto ctx = std::make_shared<std::pair<HttpClient::ApiResult, std::vector<FriendInfo>>>();
    RunAsync([ctx]() {
        ctx->first = HttpClient::Instance().GetFriends(ctx->second);
    }, [this, ctx]() {
        if (!ctx->first.ok) {
            if (ctx->first.unauthorized) {
                HandleApiUnauthorized(ctx->first.error);
                return;
            }
            auto logger = spdlog::get("im_file_logger");
            if (logger) {
                logger->error("加载好友列表失败: {}", ctx->first.error);
            }
            return;
        }
        RenderFriendList(ctx->second, std::string());
    });
}

void ChatForm::RenderFriendList(const std::vector<FriendInfo>& friends, const std::string& error)
{
    if (!error.empty()) {
        auto logger = spdlog::get("im_file_logger");
        if (logger) {
            logger->error("好友列表渲染: {}", error);
        }
    }

    m_friends.clear();
    for (const FriendInfo& info : friends) {
        m_friends[info.userId] = info;
    }

    ui::VListBox* friendList = dynamic_cast<ui::VListBox*>(FindControl(_T("friend_list")));
    if (friendList == nullptr) {
        return;
    }
    friendList->RemoveAllItems();

    for (const FriendInfo& info : friends) {
        const uint64_t peerId = info.userId;
        ui::ListBoxItemH* item = new ui::ListBoxItemH(this);
        item->SetAttribute(_T("width"), _T("stretch"));
        item->SetAttribute(_T("height"), _T("40"));
        item->SetAttribute(_T("padding"), _T("14,0,14,0"));
        item->SetAttribute(_T("border_round"), _T("6,6"));
        item->SetAttribute(_T("hovered_color"), _T("#FFF2F7FC"));
        item->SetAttribute(_T("pressed_color"), _T("#FFE6F1FB"));
        item->SetAttribute(_T("selected_normal_color"), _T("#FFE3F1FD"));
        item->SetAttribute(_T("fade_hovered"), _T("false"));

        ui::Label* nameLabel = new ui::Label(this);
        nameLabel->SetAttribute(_T("width"), _T("stretch"));
        nameLabel->SetAttribute(_T("height"), _T("auto"));
        nameLabel->SetAttribute(_T("valign"), _T("center"));
        nameLabel->SetAttribute(_T("font"), _T("system_regular_13"));
        nameLabel->SetAttribute(_T("normal_text_color"), _T("#FF1F2329"));
        nameLabel->SetAttribute(_T("mouse_enabled"), _T("false"));
        nameLabel->SetAttribute(_T("text"), ui::StringConvert::UTF8ToT(info.DisplayName() + " (" + std::to_string(info.userId) + ")"));

        ui::Button* opButton = new ui::Button(this);
        opButton->SetAttribute(_T("width"), _T("26"));
        opButton->SetAttribute(_T("height"), _T("26"));
        opButton->SetAttribute(_T("valign"), _T("center"));
        opButton->SetAttribute(_T("border_round"), _T("4,4"));
        opButton->SetAttribute(_T("normal_color"), _T("#00FFFFFF"));
        opButton->SetAttribute(_T("hovered_color"), _T("#FFE7EBF0"));
        opButton->SetAttribute(_T("pressed_color"), _T("#FFDDE3EA"));
        opButton->SetAttribute(_T("normal_text_color"), _T("#FF7C8593"));
        opButton->SetAttribute(_T("font"), _T("system_regular_14"));
        opButton->SetAttribute(_T("text"), _T("⋯"));
        opButton->SetAttribute(_T("cursor_type"), _T("hand"));
        opButton->AttachClick([this, peerId](const ui::EventArgs&) {
            ShowFriendOpPanel(peerId);
            return true;
        });

        item->AttachClick([this, peerId](const ui::EventArgs&) {
            SelectPeer(peerId);
            return true;
        });

        item->AddItem(nameLabel);
        item->AddItem(opButton);
        friendList->AddItem(item);
    }

    auto logger = spdlog::get("im_file_logger");
    if (logger) {
        logger->info("好友列表加载完成，好友数量: {}", friends.size());
    }
    RenderConversationList();
}

// ---------------------------------------------------------------------------
// 好友事件（130/131/132）
// ---------------------------------------------------------------------------
void ChatForm::HandleFriendEvent(const ImsMessage& msg)
{
    ImsFriendEvent event;
    if (!ParseImsFriendEvent(msg.content, event)) {
        auto logger = spdlog::get("im_file_logger");
        if (logger) {
            logger->warn("好友事件 content 解析失败: {}", msg.content);
        }
        return;
    }

    auto logger = spdlog::get("im_file_logger");
    if (msg.type == static_cast<int>(ImsMsgType::FriendRequest)) {
        // 130：新申请或登录补推 pending，按 requestId 去重
        const uint64_t requestId = (event.requestId != 0) ? event.requestId : msg.msg_id;
        bool exists = false;
        for (const FriendRequestInfo& info : m_incomingRequests) {
            if (info.requestId == requestId) {
                exists = true;
                break;
            }
        }
        if (!exists) {
            FriendRequestInfo info;
            info.requestId = requestId;
            info.requesterId = event.requesterId;
            info.targetId = event.targetId;
            info.requesterNickname = event.requesterName;
            info.message = event.message;
            info.status = 0;
            info.createdTs = event.ts;
            m_incomingRequests.push_back(info);
        }
        if (logger) {
            logger->info("收到好友申请 event={}, requestId={}, from={}", event.event, requestId, event.requesterName);
        }
        UpdateNewFriendsEntry(static_cast<int>(m_incomingRequests.size()));
        ui::Box* panel = dynamic_cast<ui::Box*>(FindControl(_T("request_panel")));
        if ((panel != nullptr) && panel->IsVisible()) {
            RefreshFriendRequests();
        }
    }
    else if (msg.type == static_cast<int>(ImsMsgType::FriendRequestResult)) {
        // 131：我发出的申请被通过/拒绝
        const bool accepted = (event.event == "accepted");
        m_outgoingRequestState[event.requestId] = accepted ? "已通过" : "已拒绝";
        if (logger) {
            logger->info("好友申请结果: requestId={}, {}", event.requestId, event.event);
        }
        if (accepted) {
            RefreshFriendList();
        }
    }
    else if (msg.type == static_cast<int>(ImsMsgType::FriendListChanged)) {
        // 132：好友列表变化（事件不持久化，统一全量重拉保证最终一致）
        if (logger) {
            logger->info("好友列表变化: action={}, friendId={}", event.action, event.friendId);
        }
        RefreshFriendList();
    }
}

void ChatForm::UpdateNewFriendsEntry(int count)
{
    ui::Button* btnNewFriends = dynamic_cast<ui::Button*>(FindControl(_T("btn_new_friends")));
    if (btnNewFriends == nullptr) {
        return;
    }
    if (count > 0) {
        btnNewFriends->SetVisible(true);
        btnNewFriends->SetText(ui::StringConvert::UTF8ToT("新朋友(" + std::to_string(count) + ")"));
    }
    else {
        btnNewFriends->SetVisible(false);
    }
}

void ChatForm::RefreshFriendRequests()
{
    auto ctx = std::make_shared<std::pair<HttpClient::ApiResult, std::vector<FriendRequestInfo>>>();
    RunAsync([ctx]() {
        ctx->first = HttpClient::Instance().GetFriendRequests("incoming", ctx->second);
    }, [this, ctx]() {
        if (!ctx->first.ok) {
            if (ctx->first.unauthorized) {
                HandleApiUnauthorized(ctx->first.error);
            }
            return;
        }
        // 只保留待处理(status=0)的 incoming 申请
        std::vector<FriendRequestInfo> pending;
        const uint64_t selfId = ImsSelfId();
        for (const FriendRequestInfo& info : ctx->second) {
            if ((info.status == 0) && (info.targetId == selfId)) {
                pending.push_back(info);
            }
        }
        m_incomingRequests = pending;
        UpdateNewFriendsEntry(static_cast<int>(pending.size()));
        ui::Box* panel = dynamic_cast<ui::Box*>(FindControl(_T("request_panel")));
        if ((panel != nullptr) && panel->IsVisible()) {
            RenderFriendRequests(pending, std::string());
        }
    });
}

void ChatForm::RenderFriendRequests(const std::vector<FriendRequestInfo>& requests, const std::string& error)
{
    ui::VListBox* requestList = dynamic_cast<ui::VListBox*>(FindControl(_T("request_list")));
    if (requestList == nullptr) {
        return;
    }
    requestList->RemoveAllItems();

    if (!error.empty()) {
        auto logger = spdlog::get("im_file_logger");
        if (logger) {
            logger->error("好友申请列表: {}", error);
        }
    }

    if (requests.empty()) {
        ui::ListBoxItemH* item = new ui::ListBoxItemH(this);
        item->SetAttribute(_T("width"), _T("stretch"));
        item->SetAttribute(_T("height"), _T("40"));
        ui::Label* label = new ui::Label(this);
        label->SetAttribute(_T("width"), _T("stretch"));
        label->SetAttribute(_T("height"), _T("auto"));
        label->SetAttribute(_T("valign"), _T("center"));
        label->SetAttribute(_T("halign"), _T("center"));
        label->SetAttribute(_T("font"), _T("system_regular_12"));
        label->SetAttribute(_T("normal_text_color"), _T("#FF9AA1AC"));
        label->SetAttribute(_T("text"), _T("暂无新的申请"));
        item->AddItem(label);
        requestList->AddItem(item);
        return;
    }

    for (const FriendRequestInfo& info : requests) {
        ui::ListBoxItemH* item = new ui::ListBoxItemH(this);
        item->SetAttribute(_T("width"), _T("stretch"));
        item->SetAttribute(_T("height"), _T("56"));
        item->SetAttribute(_T("padding"), _T("6,2,6,2"));

        ui::VBox* textBox = new ui::VBox(this);
        textBox->SetAttribute(_T("width"), _T("stretch"));
        textBox->SetAttribute(_T("height"), _T("stretch"));
        textBox->SetAttribute(_T("mouse_enabled"), _T("false"));

        const uint64_t selfId = ImsSelfId();
        std::string nameText = info.PeerName(selfId);
        if (nameText.empty()) {
            nameText = std::to_string(info.PeerId(selfId));
        }
        nameText += " " + ImsFormatClock(info.createdTs);
        ui::Label* nameLabel = new ui::Label(this);
        nameLabel->SetAttribute(_T("height"), _T("24"));
        nameLabel->SetAttribute(_T("font"), _T("system_regular_12"));
        nameLabel->SetAttribute(_T("normal_text_color"), _T("#FF1F2329"));
        nameLabel->SetAttribute(_T("mouse_enabled"), _T("false"));
        nameLabel->SetAttribute(_T("text"), ui::StringConvert::UTF8ToT(nameText));

        ui::Label* msgLabel = new ui::Label(this);
        msgLabel->SetAttribute(_T("height"), _T("20"));
        msgLabel->SetAttribute(_T("font"), _T("system_regular_11"));
        msgLabel->SetAttribute(_T("normal_text_color"), _T("#FF9AA1AC"));
        msgLabel->SetAttribute(_T("mouse_enabled"), _T("false"));
        msgLabel->SetAttribute(_T("text"), ui::StringConvert::UTF8ToT(info.message));

        textBox->AddItem(nameLabel);
        textBox->AddItem(msgLabel);
        item->AddItem(textBox);

        const uint64_t requestId = info.requestId;
        ui::Button* btnAccept = new ui::Button(this);
        btnAccept->SetAttribute(_T("width"), _T("44"));
        btnAccept->SetAttribute(_T("height"), _T("24"));
        btnAccept->SetAttribute(_T("valign"), _T("center"));
        btnAccept->SetAttribute(_T("border_round"), _T("4,4"));
        btnAccept->SetAttribute(_T("bkcolor"), _T("#FF5FB8F0"));
        btnAccept->SetAttribute(_T("normal_text_color"), _T("#FFFFFFFF"));
        btnAccept->SetAttribute(_T("font"), _T("system_regular_11"));
        btnAccept->SetAttribute(_T("text"), _T("通过"));
        btnAccept->SetAttribute(_T("cursor_type"), _T("hand"));
        btnAccept->AttachClick([this, requestId](const ui::EventArgs&) {
            auto ctx = std::make_shared<HttpClient::ApiResult>();
            RunAsync([ctx, requestId]() {
                *ctx = HttpClient::Instance().AcceptFriendRequest(requestId);
            }, [this, ctx, requestId]() {
                if (ctx->unauthorized) {
                    HandleApiUnauthorized(ctx->error);
                    return;
                }
                auto logger = spdlog::get("im_file_logger");
                if (logger) {
                    logger->info("通过好友申请 requestId={}, ok={}, err={}", requestId, ctx->ok, ctx->error);
                }
                RefreshFriendRequests();
                RefreshFriendList();
            });
            return true;
        });

        ui::Button* btnReject = new ui::Button(this);
        btnReject->SetAttribute(_T("width"), _T("44"));
        btnReject->SetAttribute(_T("height"), _T("24"));
        btnReject->SetAttribute(_T("valign"), _T("center"));
        btnReject->SetAttribute(_T("margin"), _T("6,0,0,0"));
        btnReject->SetAttribute(_T("border_round"), _T("4,4"));
        btnReject->SetAttribute(_T("bkcolor"), _T("#FFEFEFF4"));
        btnReject->SetAttribute(_T("normal_text_color"), _T("#FF5B5F6B"));
        btnReject->SetAttribute(_T("font"), _T("system_regular_11"));
        btnReject->SetAttribute(_T("text"), _T("拒绝"));
        btnReject->SetAttribute(_T("cursor_type"), _T("hand"));
        btnReject->AttachClick([this, requestId](const ui::EventArgs&) {
            auto ctx = std::make_shared<HttpClient::ApiResult>();
            RunAsync([ctx, requestId]() {
                *ctx = HttpClient::Instance().RejectFriendRequest(requestId);
            }, [this, ctx, requestId]() {
                if (ctx->unauthorized) {
                    HandleApiUnauthorized(ctx->error);
                    return;
                }
                auto logger = spdlog::get("im_file_logger");
                if (logger) {
                    logger->info("拒绝好友申请 requestId={}, ok={}, err={}", requestId, ctx->ok, ctx->error);
                }
                RefreshFriendRequests();
            });
            return true;
        });

        item->AddItem(btnAccept);
        item->AddItem(btnReject);
        requestList->AddItem(item);
    }
}

// ---------------------------------------------------------------------------
// 好友操作面板与浮层
// ---------------------------------------------------------------------------
void ChatForm::HideOverlayPanels()
{
    const DString names[] = { _T("add_friend_panel"), _T("request_panel"), _T("friend_op_panel") };
    for (const DString& name : names) {
        ui::Box* panel = dynamic_cast<ui::Box*>(FindControl(name));
        if (panel != nullptr) {
            panel->SetVisible(false);
        }
    }
}

void ChatForm::ShowFriendOpPanel(uint64_t friendId)
{
    m_opPanelFriendId = friendId;
    HideOverlayPanels();

    const FriendInfo* info = FindFriend(friendId);
    ui::Box* panel = dynamic_cast<ui::Box*>(FindControl(_T("friend_op_panel")));
    ui::Label* title = dynamic_cast<ui::Label*>(FindControl(_T("friend_op_title")));
    ui::RichEdit2* remarkEdit = dynamic_cast<ui::RichEdit2*>(FindControl(_T("remark_edit")));
    ui::Label* hint = dynamic_cast<ui::Label*>(FindControl(_T("friend_op_hint")));
    ui::Button* btnBlock = dynamic_cast<ui::Button*>(FindControl(_T("btn_block_friend")));

    if (title != nullptr) {
        std::string text = "好友: ";
        text += (info != nullptr) ? (info->DisplayName() + " (" + std::to_string(friendId) + ")")
                                  : std::to_string(friendId);
        title->SetText(ui::StringConvert::UTF8ToT(text));
    }
    if (remarkEdit != nullptr) {
        remarkEdit->SetText(ui::StringConvert::UTF8ToT(info != nullptr ? info->remark : std::string()));
    }
    if (hint != nullptr) {
        hint->SetText(_T(""));
    }
    if (btnBlock != nullptr) {
        btnBlock->SetText(_T("拉黑"));
    }
    if (panel != nullptr) {
        panel->SetVisible(true);
    }
}

void ChatForm::HandleApiUnauthorized(const std::string& error)
{
    auto logger = spdlog::get("im_file_logger");
    if (logger) {
        logger->error("鉴权失效: {}", error);
    }

    TcpClient::Instance().Stop();
    ImsSession::Instance().Clear();

    // 返回登录界面
    LoginForm* loginWindow = new LoginForm(_T("login"), _T(""), _T("login.xml"));
    if (loginWindow->CreateWnd(nullptr, ui::WindowCreateParam(_T("克铖多媒体"), true))) {
        PostQuitMsgWhenClosed(false);
        loginWindow->PostQuitMsgWhenClosed(true);
        loginWindow->ShowWindow(ui::kSW_SHOW_NORMAL);
        CloseWnd();
    }
    else {
        delete loginWindow;
        PostQuitMsgWhenClosed(true);
        CloseWnd();
    }
}

#endif // CXIM_IMS_CORE_SDK_VERSION

void ChatForm::ShowCustomWindow(const DString& class_name, const DString& theme_directory, const DString& layout_xml)
{
    ChatForm* window = new ChatForm(class_name, theme_directory, layout_xml);
    window->CreateWnd(nullptr, ui::WindowCreateParam(_T("chat"), true));
    window->PostQuitMsgWhenClosed(true);
    window->ShowWindow(ui::kSW_SHOW_NORMAL);

}
