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
                contactsListPages->SelectItem(_T("group_list_page"));
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
        tabGroup->AttachClick([this, contactsListPages](const ui::EventArgs&) {
            if (contactsListPages != nullptr) {
                // 页面子控件名是 group_list_page（内层列表才是 group_list）
                contactsListPages->SelectItem(_T("group_list_page"));
            }
            RefreshGroupList();
            RefreshGroupRequests();
            RenderGroupList();
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

    // 6) 聊天区"更多"按钮 → 当前会话的操作面板（群→群管理，单聊→好友操作）
    ui::Button* btnMore = dynamic_cast<ui::Button*>(FindControl(_T("btn_more")));
    if (btnMore != nullptr) {
        btnMore->AttachClick([this](const ui::EventArgs&) {
            if (m_currentGroup != 0) {
                ShowGroupOpMenu(m_currentGroup);
            }
            else if (m_currentPeer != 0) {
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

    // 10) 群聊 UI 接线（wechat.xml 新增控件，缺失时安全跳过）
    {
        // 成员按钮（聊天区工具栏/标题栏）→ 群成员侧栏
        ui::Button* btnMembers = dynamic_cast<ui::Button*>(FindControl(_T("btn_members")));
        if (btnMembers != nullptr) {
            btnMembers->AttachClick([this](const ui::EventArgs&) {
                if (m_currentGroup != 0) {
                    ShowGroupMemberPanel(m_currentGroup);
                }
                return true;
            });
        }

        // 新建群聊入口（群聊页签底部）
        ui::Button* btnNewGroup = dynamic_cast<ui::Button*>(FindControl(_T("btn_new_group")));
        if (btnNewGroup != nullptr) {
            btnNewGroup->AttachClick([this](const ui::EventArgs&) {
                ShowCreateGroupDialog();
                return true;
            });
        }

        // 入群申请入口（红点）
        ui::Button* btnGroupRequests = dynamic_cast<ui::Button*>(FindControl(_T("btn_group_requests")));
        if (btnGroupRequests != nullptr) {
            btnGroupRequests->SetVisible(false);
            btnGroupRequests->AttachClick([this](const ui::EventArgs&) {
                ShowJoinRequestPanel();
                return true;
            });
        }

        // 建群/邀请面板
        ui::Box* createPanel = dynamic_cast<ui::Box*>(FindControl(_T("create_group_panel")));
        if (createPanel != nullptr) {
            createPanel->SetVisible(false);
        }
        ui::Button* btnCloseCreate = dynamic_cast<ui::Button*>(FindControl(_T("btn_close_create_group")));
        if (btnCloseCreate != nullptr) {
            btnCloseCreate->AttachClick([this](const ui::EventArgs&) {
                ui::Box* p = dynamic_cast<ui::Box*>(FindControl(_T("create_group_panel")));
                if (p != nullptr) {
                    p->SetVisible(false);
                }
                m_inviteTargetGroupId = 0;
                return true;
            });
        }
        ui::Button* btnCreateGroup = dynamic_cast<ui::Button*>(FindControl(_T("btn_create_group")));
        if (btnCreateGroup != nullptr) {
            btnCreateGroup->SetText(_T("创建"));
            // 只在此处绑定一次：根据 m_inviteTargetGroupId 分派“建群/邀请”，
            // 避免每次打开对话框重复 AttachClick 导致一次点击触发多次创建（群列表重复）。
            btnCreateGroup->AttachClick([this](const ui::EventArgs&) {
                OnCreateDialogConfirm();
                return true;
            });
        }

        // 群成员面板
        ui::Box* memberPanel = dynamic_cast<ui::Box*>(FindControl(_T("group_member_panel")));
        if (memberPanel != nullptr) {
            memberPanel->SetVisible(false);
        }
        ui::Button* btnCloseMember = dynamic_cast<ui::Button*>(FindControl(_T("btn_close_group_member")));
        if (btnCloseMember != nullptr) {
            btnCloseMember->AttachClick([this](const ui::EventArgs&) {
                ui::Box* p = dynamic_cast<ui::Box*>(FindControl(_T("group_member_panel")));
                if (p != nullptr) {
                    p->SetVisible(false);
                }
                return true;
            });
        }

        // 入群申请面板
        ui::Box* joinPanel = dynamic_cast<ui::Box*>(FindControl(_T("join_request_panel")));
        if (joinPanel != nullptr) {
            joinPanel->SetVisible(false);
        }
        ui::Button* btnCloseJoin = dynamic_cast<ui::Button*>(FindControl(_T("btn_close_join_request")));
        if (btnCloseJoin != nullptr) {
            btnCloseJoin->AttachClick([this](const ui::EventArgs&) {
                ui::Box* p = dynamic_cast<ui::Box*>(FindControl(_T("join_request_panel")));
                if (p != nullptr) {
                    p->SetVisible(false);
                }
                return true;
            });
        }

        // 群管理菜单（group_op_panel）
        ui::Box* opPanel = dynamic_cast<ui::Box*>(FindControl(_T("group_op_panel")));
        if (opPanel != nullptr) {
            opPanel->SetVisible(false);
        }
        ui::Button* btnCloseOp = dynamic_cast<ui::Button*>(FindControl(_T("btn_close_group_op")));
        if (btnCloseOp != nullptr) {
            btnCloseOp->AttachClick([this](const ui::EventArgs&) {
                ui::Box* p = dynamic_cast<ui::Box*>(FindControl(_T("group_op_panel")));
                if (p != nullptr) {
                    p->SetVisible(false);
                }
                return true;
            });
        }
        // 管理动作按钮：先隐藏，由 ShowGroupOpMenu 按角色显示；点击行为按 myRole 由 DoGroupMenuAction 兜底
        struct OpDef { const DString* name; const char* action; };
        const DString nUpdate = _T("btn_group_update"), nInvite = _T("btn_group_invite"),
                      nRemove = _T("btn_group_remove"), nAdmin = _T("btn_group_admin"),
                      nTransfer = _T("btn_group_transfer"), nDismiss = _T("btn_group_dismiss"),
                      nQuit = _T("btn_group_quit");
        const OpDef ops[] = {
            { &nUpdate, "update" }, { &nInvite, "invite" }, { &nRemove, "remove" },
            { &nAdmin, "admin" }, { &nTransfer, "transfer" }, { &nDismiss, "dismiss" }, { &nQuit, "quit" }
        };
        for (const OpDef& op : ops) {
            ui::Button* b = dynamic_cast<ui::Button*>(FindControl(*op.name));
            if (b == nullptr) {
                continue;
            }
            b->SetVisible(false);
            const std::string action = op.action;
            b->AttachClick([this, action](const ui::EventArgs&) {
                const uint64_t gid = m_opPanelGroupId;
                if (gid == 0) {
                    return true;
                }
                // 需要输入的动作先打开对应子面板；其余直接执行
                if (action == "update") {
                    HideOverlayPanels();
                    ui::Box* p = dynamic_cast<ui::Box*>(FindControl(_T("group_update_panel")));
                    if (p != nullptr) {
                        p->SetVisible(true);
                        ui::RichEdit2* nameEdit = dynamic_cast<ui::RichEdit2*>(FindControl(_T("group_name_edit")));
                        if ((nameEdit != nullptr) && (FindGroupInfo(gid) != nullptr)) {
                            nameEdit->SetText(ui::StringConvert::UTF8ToT(FindGroupInfo(gid)->name));
                        }
                    }
                }
                else if (action == "remove") {
                    HideOverlayPanels();
                    ui::Box* p = dynamic_cast<ui::Box*>(FindControl(_T("group_remove_panel")));
                    if (p != nullptr) {
                        p->SetVisible(true);
                    }
                }
                else if (action == "admin") {
                    HideOverlayPanels();
                    ui::Box* p = dynamic_cast<ui::Box*>(FindControl(_T("group_admin_panel")));
                    if (p != nullptr) {
                        p->SetVisible(true);
                    }
                }
                else if (action == "transfer") {
                    HideOverlayPanels();
                    ui::Box* p = dynamic_cast<ui::Box*>(FindControl(_T("group_transfer_panel")));
                    if (p != nullptr) {
                        p->SetVisible(true);
                    }
                }
                else {
                    DoGroupMenuAction(gid, action);
                }
                return true;
            });
        }

        // 子面板“确定”按钮 + 关闭按钮
        struct SubDef { const DString* okName; const DString* closeName; const DString* panelName; const char* action; };
        const DString okUpdate = _T("btn_group_update_save"), okRemove = _T("btn_group_remove_ok"),
                      okAdmin = _T("btn_group_admin_ok"), okTransfer = _T("btn_group_transfer_ok");
        const DString closeUpd = _T("btn_close_group_update"), closeRem = _T("btn_close_group_remove"),
                      closeAdm = _T("btn_close_group_admin"), closeTr = _T("btn_close_group_transfer");
        const DString pUpd = _T("group_update_panel"), pRem = _T("group_remove_panel"),
                      pAdm = _T("group_admin_panel"), pTr = _T("group_transfer_panel");
        const SubDef subs[] = {
            { &okUpdate, &closeUpd, &pUpd, "update" }, { &okRemove, &closeRem, &pRem, "remove" },
            { &okAdmin, &closeAdm, &pAdm, "admin" }, { &okTransfer, &closeTr, &pTr, "transfer" }
        };
        for (const SubDef& s : subs) {
            ui::Box* panel = dynamic_cast<ui::Box*>(FindControl(*s.panelName));
            if (panel != nullptr) {
                panel->SetVisible(false);
            }
            ui::Button* closeBtn = dynamic_cast<ui::Button*>(FindControl(*s.closeName));
            if (closeBtn != nullptr) {
                const DString pn = *s.panelName;
                closeBtn->AttachClick([this, pn](const ui::EventArgs&) {
                    ui::Box* p = dynamic_cast<ui::Box*>(FindControl(pn));
                    if (p != nullptr) {
                        p->SetVisible(false);
                    }
                    return true;
                });
            }
            ui::Button* okBtn = dynamic_cast<ui::Button*>(FindControl(*s.okName));
            if (okBtn != nullptr) {
                const std::string action = s.action;
                okBtn->AttachClick([this, action](const ui::EventArgs&) {
                    const uint64_t gid = m_opPanelGroupId;
                    if (gid != 0) {
                        DoGroupMenuAction(gid, action);
                    }
                    return true;
                });
            }
        }
    }

    // 11) 登录后刷新群列表与入群申请红点
    RefreshGroupList();
    RefreshGroupRequests();
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
            // 群聊一致性总策略（提示词11.5）：每次登录成功（含重连）都全量刷新群列表，
            // 再对当前打开的群逐个拉详情；事件只作为增量刷新触发器。
            RefreshGroupList();
            RefreshGroupRequests();
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
    // 投递回执在线程上直接发送，无需切回 UI 线程（单聊/群聊规则一致，带 conversation_id+seq）
    if ((msg.type == static_cast<int>(ImsMsgType::Text)) || (msg.type == static_cast<int>(ImsMsgType::Image))) {
        if ((msg.conversation_id != 0) && (msg.seq != 0)) {
            ImsMessage ack;
            ack.type = static_cast<int>(ImsMsgType::Ack);
            ack.conversation_id = msg.conversation_id;
            ack.seq = msg.seq;
            ack.is_group = msg.is_group;
            ack.content = "delivered";
            TcpClient::Instance().Send(ack);
        }
    }

    ImsMessage copy = msg;
    PostToUi([this, copy]() {
        const bool isGroup = copy.is_group;
        switch (copy.type) {
        case static_cast<int>(ImsMsgType::Text):
        case static_cast<int>(ImsMsgType::Image):
            if (isGroup) {
                HandleGroupChatMessage(copy);   // 群消息：to=groupId, from=发送者
            }
            else {
                HandleChatMessage(copy, false);
            }
            break;
        case static_cast<int>(ImsMsgType::HistoryResp):
            if (isGroup) {
                HandleGroupHistoryResp(copy);
            }
            else {
                HandleHistoryResp(copy);
            }
            break;
        case static_cast<int>(ImsMsgType::SendAck):
            if (isGroup) {
                HandleGroupSendAck(copy);
            }
            else {
                HandleSendAck(copy);
            }
            break;
        case static_cast<int>(ImsMsgType::Error):
            HandleTcpError(copy);
            break;
        case static_cast<int>(ImsMsgType::FriendRequest):
        case static_cast<int>(ImsMsgType::FriendRequestResult):
        case static_cast<int>(ImsMsgType::FriendListChanged):
            HandleFriendEvent(copy);
            break;
        case static_cast<int>(ImsMsgType::GroupMemberChanged):
        case static_cast<int>(ImsMsgType::GroupDismissed):
        case static_cast<int>(ImsMsgType::GroupInfoChanged):
        case static_cast<int>(ImsMsgType::GroupRequest):
        case static_cast<int>(ImsMsgType::GroupRequestResult):
            HandleGroupEvent(copy.type, copy);
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
    // 单聊 SendAck：弹出队列中第一条非群发送
    size_t idx = 0;
    bool found = false;
    for (; idx < m_pendingSends.size(); ++idx) {
        if (!m_pendingSends[idx].isGroup) {
            found = true;
            break;
        }
    }
    if (!found) {
        return;
    }
    const PendingSend pending = m_pendingSends[idx];
    m_pendingSends.erase(m_pendingSends.begin() + static_cast<long>(idx));

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
    const uint64_t peerId = m_historyFifo.front().id;
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
    req.is_group = false;
    HistoryKey key; key.isGroup = false; key.id = peerId;
    m_historyFifo.push_back(key);
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
    // 群会话：走群发送链路（type=1,is_group=true,to=groupId）
    if (m_currentGroup != 0) {
        ui::RichEdit2* messageEdit = dynamic_cast<ui::RichEdit2*>(FindControl(_T("message_edit")));
        if (messageEdit == nullptr) {
            return;
        }
        const std::string text = ImsTrim(ui::StringConvert::TToUTF8(messageEdit->GetText()));
        if (text.empty()) {
            return;
        }
        messageEdit->SetText(_T(""));
        SendCurrentGroupMessage(m_currentGroup, text);
        return;
    }

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
// 会话列表（合并单聊 + 群聊，按最后消息时间排序，新消息置顶）
// ---------------------------------------------------------------------------
void ChatForm::EnsureConversationRow(uint64_t peerId, const std::string& text)
{
    const std::string key = ConvKey(false, peerId);
    auto iter = std::find(m_conversationOrder.begin(), m_conversationOrder.end(), key);
    if (iter != m_conversationOrder.end()) {
        if (iter != m_conversationOrder.begin()) {
            m_conversationOrder.erase(iter);
            m_conversationOrder.insert(m_conversationOrder.begin(), key);
        }
    }
    else {
        m_conversationOrder.insert(m_conversationOrder.begin(), key);
    }
    m_lastSnippet[key] = text;
    RenderConversationList();
}

namespace {

/** 会话行描述（单聊/群聊统一） */
struct ConvRow {
    bool isGroup = false;
    uint64_t id = 0;
    std::string key;
    std::string title;
    std::string snippet;
    uint64_t lastTs = 0;
    int unread = 0;
};

} // namespace

void ChatForm::RenderConversationList()
{
    ui::VListBox* conversationList = dynamic_cast<ui::VListBox*>(FindControl(_T("message_list")));
    if (conversationList == nullptr) {
        return;
    }
    conversationList->RemoveAllItems();

    const uint64_t selfId = ImsSelfId();
    std::vector<ConvRow> rows;

    // 单聊会话
    for (const auto& kv : m_peers) {
        const PeerState& state = kv.second;
        if (state.items.empty()) {
            continue;
        }
        ConvRow row;
        row.isGroup = false;
        row.id = kv.first;
        row.key = ConvKey(false, kv.first);
        row.title = PeerDisplayName(kv.first);
        const PeerState::Item& last = state.items.back();
        row.snippet = (last.fromSelf ? "我: " : "") + last.text;
        row.lastTs = last.ts;
        row.unread = state.unread;
        rows.push_back(row);
    }

    // 群聊会话（提示词12.3：群会话行显示群图标+群名+"[群] xxx"摘要，未读角标）
    for (const auto& kv : m_groups) {
        const GroupState& state = kv.second;
        if (state.removed || state.info.status != 0) {
            continue;   // 已不在该群/已解散：不展示
        }
        ConvRow row;
        row.isGroup = true;
        row.id = kv.first;
        row.key = ConvKey(true, kv.first);
        row.title = GroupDisplayName(kv.first);
        if (!state.items.empty()) {
            const GroupState::Item& last = state.items.back();
            row.snippet = "[群] " + last.text;
            row.lastTs = last.ts;
        }
        else {
            row.snippet = "[群] ";
            row.lastTs = state.info.createdTs;
        }
        row.unread = state.unread;
        (void)selfId;
        rows.push_back(row);
    }

    // 置顶/排序：先按本地会话顺序（新消息置顶），未登记的排在其后按时间倒序
    std::vector<ConvRow> ordered;
    ordered.reserve(rows.size());
    for (const std::string& key : m_conversationOrder) {
        for (ConvRow& row : rows) {
            if (row.key == key) {
                ordered.push_back(row);
                row.key.clear();   // 标记已消费
                break;
            }
        }
    }
    std::vector<ConvRow> rest;
    for (ConvRow& row : rows) {
        if (!row.key.empty()) {
            rest.push_back(row);
        }
    }
    std::stable_sort(rest.begin(), rest.end(),
        [](const ConvRow& a, const ConvRow& b) { return a.lastTs > b.lastTs; });
    for (const ConvRow& row : rest) {
        ordered.push_back(row);
    }

    for (const ConvRow& row : ordered) {
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

        // 群聊头像占位（单聊留空）
        if (row.isGroup) {
            ui::Control* icon = new ui::Control(this);
            icon->SetAttribute(_T("width"), _T("40"));
            icon->SetAttribute(_T("height"), _T("40"));
            icon->SetAttribute(_T("valign"), _T("center"));
            icon->SetAttribute(_T("mouse_enabled"), _T("false"));
            icon->SetAttribute(_T("normal_image"), _T("file='wechat_icon_group.svg' width='34' height='34' valign='center' halign='center'"));
            item->AddItem(icon);
        }

        ui::VBox* textBox = new ui::VBox(this);
        textBox->SetAttribute(_T("width"), _T("stretch"));
        textBox->SetAttribute(_T("height"), _T("stretch"));
        textBox->SetAttribute(_T("mouse_enabled"), _T("false"));

        std::string nameText = row.title;
        if (row.unread > 0) {
            nameText += " (" + std::to_string(row.unread) + ")";
        }
        ui::Label* nameLabel = new ui::Label(this);
        nameLabel->SetAttribute(_T("height"), _T("26"));
        nameLabel->SetAttribute(_T("margin"), _T("0,6,0,0"));
        nameLabel->SetAttribute(_T("valign"), _T("center"));
        nameLabel->SetAttribute(_T("font"), _T("system_regular_13"));
        nameLabel->SetAttribute(_T("normal_text_color"), row.unread > 0 ? _T("#FFE0483D") : _T("#FF1F2329"));
        nameLabel->SetAttribute(_T("mouse_enabled"), _T("false"));
        nameLabel->SetAttribute(_T("text"), ui::StringConvert::UTF8ToT(nameText));

        ui::Label* snippetLabel = new ui::Label(this);
        snippetLabel->SetAttribute(_T("height"), _T("20"));
        snippetLabel->SetAttribute(_T("font"), _T("system_regular_11"));
        snippetLabel->SetAttribute(_T("normal_text_color"), _T("#FF9AA1AC"));
        snippetLabel->SetAttribute(_T("mouse_enabled"), _T("false"));
        const std::string clock = ImsFormatClock(row.lastTs);
        const std::string snippetText = row.snippet + (clock.empty() ? std::string() : ("  " + clock));
        snippetLabel->SetAttribute(_T("text"), ui::StringConvert::UTF8ToT(snippetText));

        textBox->AddItem(nameLabel);
        textBox->AddItem(snippetLabel);
        item->AddItem(textBox);

        const bool isGroup = row.isGroup;
        const uint64_t id = row.id;
        item->AttachClick([this, isGroup, id](const ui::EventArgs&) {
            if (isGroup) {
                SelectGroup(id);
            }
            else {
                SelectPeer(id);
            }
            return true;
        });

        conversationList->AddItem(item);
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
    m_currentGroup = 0;   // 与群会话互斥

    // 单聊不显示群公告条
    ui::Label* announceLabel = dynamic_cast<ui::Label*>(FindControl(_T("chat_announcement_label")));
    if (announceLabel != nullptr) {
        announceLabel->SetVisible(false);
    }

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
    const DString names[] = {
        _T("add_friend_panel"), _T("request_panel"), _T("friend_op_panel"),
        _T("create_group_panel"), _T("group_member_panel"), _T("group_op_panel"),
        _T("join_request_panel"), _T("group_update_panel"), _T("group_remove_panel"),
        _T("group_admin_panel"), _T("group_transfer_panel")
    };
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

// ===========================================================================
// 群聊：会话模型 / TCP 收发 / 事件 / UI（提示词 10、11、12）
// ===========================================================================

/** 群气泡（发送者昵称在气泡上方，多行显示） */
void ChatForm::AppendGroupBubble(bool fromSelf, const std::string& senderName, const std::string& text,
                                 ui::Label** outLabel)
{
    ui::VListBox* messageList = dynamic_cast<ui::VListBox*>(FindControl(_T("chat_message_list")));
    if (messageList == nullptr) {
        return;
    }
    ui::ListBoxItemH* item = new ui::ListBoxItemH(this);
    item->SetAttribute(_T("width"), _T("stretch"));
    item->SetAttribute(_T("height"), _T("auto"));
    item->SetAttribute(_T("padding"), _T("0,2,0,2"));

    ui::Control* spacer = new ui::Control(this);
    spacer->SetAttribute(_T("width"), _T("stretch"));
    spacer->SetAttribute(_T("height"), _T("auto"));

    ui::VBox* col = new ui::VBox(this);
    col->SetAttribute(_T("width"), _T("auto"));
    col->SetAttribute(_T("height"), _T("auto"));

    if (!fromSelf && !senderName.empty()) {
        ui::Label* name = new ui::Label(this);
        name->SetAttribute(_T("width"), _T("auto"));
        name->SetAttribute(_T("height"), _T("16"));
        name->SetAttribute(_T("font"), _T("system_regular_10"));
        name->SetAttribute(_T("normal_text_color"), _T("#FF9AA1AC"));
        name->SetAttribute(_T("mouse_enabled"), _T("false"));
        name->SetAttribute(_T("text"), ui::StringConvert::UTF8ToT(senderName));
        col->AddItem(name);
    }

    ui::Label* bubble = new ui::Label(this);
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
    bubble->SetAttribute(_T("text"), ui::StringConvert::UTF8ToT(text));
    col->AddItem(bubble);

    if (fromSelf) {
        item->AddItem(spacer);
        item->AddItem(col);
    }
    else {
        item->AddItem(col);
        item->AddItem(spacer);
    }
    messageList->AddItem(item);
    const size_t count = messageList->GetItemCount();
    if (count > 0) {
        messageList->EnsureVisible(count - 1);
    }
    messageList->EndDown();
    if (outLabel != nullptr) {
        *outLabel = bubble;
    }
}

const ImsGroupInfo* ChatForm::FindGroupInfo(uint64_t groupId) const
{
    auto iter = m_groups.find(groupId);
    if (iter != m_groups.end()) {
        return &iter->second.info;
    }
    for (const ImsGroupInfo& g : m_groupList) {
        if (g.groupId == groupId) {
            return &g;
        }
    }
    return nullptr;
}

std::string ChatForm::GroupDisplayName(uint64_t groupId) const
{
    const ImsGroupInfo* info = FindGroupInfo(groupId);
    if (info != nullptr && !info->name.empty()) {
        return info->name;
    }
    return "群" + std::to_string(groupId);
}

std::string ChatForm::GroupMemberName(uint64_t groupId, uint64_t userId) const
{
    auto iter = m_groups.find(groupId);
    if (iter != m_groups.end()) {
        auto nameIter = iter->second.memberNames.find(userId);
        if (nameIter != iter->second.memberNames.end() && !nameIter->second.empty()) {
            return nameIter->second;
        }
    }
    // 取不到先显示 userId（提示词10.3）
    return std::to_string(userId);
}

void ChatForm::SelectGroup(uint64_t groupId)
{
    if (groupId == 0) {
        return;
    }
    m_currentGroup = groupId;
    m_currentPeer = 0;   // 与单聊会话互斥

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
        titleLabel->SetText(ui::StringConvert::UTF8ToT(GroupDisplayName(groupId)));
    }
    UpdateGroupAnnouncementBar();

    // 确保本地有该群会话（即便事件先到但还没进列表）
    GroupState& gs = m_groups[groupId];
    if (gs.info.groupId == 0) {
        const ImsGroupInfo* info = FindGroupInfo(groupId);
        if (info != nullptr) {
            gs.info = *info;
        }
        else {
            gs.info.groupId = groupId;
        }
    }

    RenderGroupMessages(groupId);
    RequestGroupHistory(groupId, 0);
    MarkGroupRead(groupId);

    auto logger = spdlog::get("im_file_logger");
    if (logger) {
        logger->info("打开群会话 groupId={}", groupId);
    }
}

void ChatForm::UpdateGroupAnnouncementBar()
{
    ui::Label* announceLabel = dynamic_cast<ui::Label*>(FindControl(_T("chat_announcement_label")));
    if (announceLabel == nullptr) {
        return;
    }
    if (m_currentGroup == 0) {
        announceLabel->SetVisible(false);
        return;
    }
    const ImsGroupInfo* info = FindGroupInfo(m_currentGroup);
    if ((info != nullptr) && !info->announcement.empty()) {
        announceLabel->SetVisible(true);
        announceLabel->SetText(ui::StringConvert::UTF8ToT("公告: " + info->announcement));
    }
    else {
        announceLabel->SetVisible(false);
    }
}

void ChatForm::RequestGroupHistory(uint64_t groupId, uint64_t beforeSeq)
{
    ImsMessage req;
    req.type = static_cast<int>(ImsMsgType::HistoryReq);
    req.to = groupId;              // 群聊：to = groupId
    req.seq = beforeSeq;           // seq = before_seq（0=最新）
    req.content = "20";
    req.is_group = true;
    HistoryKey key; key.isGroup = true; key.id = groupId;
    m_historyFifo.push_back(key);
    TcpClient::Instance().Send(req);
}

void ChatForm::RenderGroupMessages(uint64_t groupId)
{
    ui::VListBox* messageList = dynamic_cast<ui::VListBox*>(FindControl(_T("chat_message_list")));
    if (messageList == nullptr) {
        return;
    }
    messageList->RemoveAllItems();
    auto iter = m_groups.find(groupId);
    if (iter == m_groups.end()) {
        return;
    }
    GroupState& state = iter->second;
    std::vector<GroupState::Item> items = state.items;
    std::stable_sort(items.begin(), items.end(),
        [](const GroupState::Item& left, const GroupState::Item& right) {
            return left.seq < right.seq;   // seq 升序，最新在底部
        });
    for (const GroupState::Item& item : items) {
        const std::string sender = item.fromSelf ? std::string() : GroupMemberName(groupId, item.senderId);
        AppendGroupBubble(item.fromSelf, sender, item.text, nullptr);
    }
    const size_t count = messageList->GetItemCount();
    if (count > 0) {
        messageList->EnsureVisible(count - 1);
    }
    messageList->EndDown();
}

void ChatForm::MarkGroupRead(uint64_t groupId)
{
    auto iter = m_groups.find(groupId);
    if (iter == m_groups.end()) {
        return;
    }
    GroupState& state = iter->second;
    state.unread = 0;
    uint64_t conv = 0;
    uint64_t maxSeq = 0;
    for (const GroupState::Item& item : state.items) {
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
        readAck.is_group = true;      // 空 content=已读，带 conversation_id+seq（提示词10.3）
        TcpClient::Instance().Send(readAck);
    }
    RenderConversationList();
}

void ChatForm::HandleGroupChatMessage(const ImsMessage& msg)
{
    const uint64_t groupId = msg.to;   // 群消息：to=groupId
    if (groupId == 0) {
        return;
    }
    GroupState& state = m_groups[groupId];
    if (state.removed || (state.info.status != 0)) {
        return;   // 已不在该群：不再渲染后续消息（提示词10.5）
    }
    const std::pair<uint64_t, uint64_t> key(msg.conversation_id, msg.seq);
    if (state.seen.count(key) != 0) {
        return;   // 离线补推/重连重复，按 (conversation_id, seq) 去重
    }
    state.seen.insert(key);

    GroupState::Item item;
    item.fromSelf = false;
    item.senderId = msg.from;
    item.sender = GroupMemberName(groupId, msg.from);
    item.text = msg.content;
    item.conversation_id = msg.conversation_id;
    item.seq = msg.seq;
    item.ts = msg.ts;
    state.items.push_back(item);
    if ((state.minSeq == 0) || (msg.seq < state.minSeq)) {
        state.minSeq = msg.seq;
    }

    if (groupId == m_currentGroup) {
        AppendGroupBubble(false, item.sender, item.text, nullptr);
        ImsMessage readAck;
        readAck.type = static_cast<int>(ImsMsgType::Ack);
        readAck.conversation_id = msg.conversation_id;
        readAck.seq = msg.seq;
        readAck.is_group = true;
        TcpClient::Instance().Send(readAck);
    }
    else {
        state.unread++;
    }
    EnsureConversationGroupRow(groupId, item.text);
}

void ChatForm::HandleGroupSendAck(const ImsMessage& msg)
{
    // 找到队列中第一条群发送
    size_t idx = 0;
    bool found = false;
    for (; idx < m_pendingSends.size(); ++idx) {
        if (m_pendingSends[idx].isGroup) {
            found = true;
            break;
        }
    }
    if (!found) {
        return;
    }
    const PendingSend pending = m_pendingSends[idx];
    m_pendingSends.erase(m_pendingSends.begin() + static_cast<long>(idx));

    if (pending.bubble != nullptr) {
        const std::string suffix = (msg.content == "duplicate") ? "  [已发送(去重)]" : "  [已发送]";
        pending.bubble->SetText(ui::StringConvert::UTF8ToT(pending.text + suffix));
    }

    GroupState& state = m_groups[pending.groupId];
    const std::pair<uint64_t, uint64_t> key(msg.conversation_id, msg.seq);
    if (state.seen.count(key) == 0) {
        state.seen.insert(key);
        GroupState::Item item;
        item.fromSelf = true;
        item.senderId = ImsSelfId();
        item.text = pending.text;
        item.conversation_id = msg.conversation_id;
        item.seq = msg.seq;
        item.ts = msg.ts;
        state.items.push_back(item);
        if ((state.minSeq == 0) || (msg.seq < state.minSeq)) {
            state.minSeq = msg.seq;
        }
    }
    EnsureConversationGroupRow(pending.groupId, pending.text);

    auto logger = spdlog::get("im_file_logger");
    if (logger) {
        logger->info("群 SendAck: conv={}, seq={}, {}", msg.conversation_id, msg.seq, msg.content);
    }
}

void ChatForm::HandleGroupHistoryResp(const ImsMessage& msg)
{
    if (m_historyFifo.empty()) {
        return;
    }
    const uint64_t groupId = m_historyFifo.front().id;
    m_historyFifo.erase(m_historyFifo.begin());

    std::vector<ImsHistoryMessage> history;
    std::string err;
    if (!ParseImsHistory(msg.content, history, err)) {
        auto logger = spdlog::get("im_file_logger");
        if (logger) {
            logger->error("群历史解析失败: {}", err);
        }
        return;
    }
    std::stable_sort(history.begin(), history.end(),
        [](const ImsHistoryMessage& left, const ImsHistoryMessage& right) {
            return left.seq < right.seq;
        });

    const uint64_t selfId = ImsSelfId();
    GroupState& state = m_groups[groupId];
    bool hasNewItems = false;
    for (const ImsHistoryMessage& m : history) {
        const std::pair<uint64_t, uint64_t> key(m.conversation_id, m.seq);
        if (state.seen.count(key) != 0) {
            continue;
        }
        state.seen.insert(key);
        GroupState::Item item;
        item.fromSelf = (m.from == selfId);
        item.senderId = m.from;
        item.text = m.content;
        item.conversation_id = m.conversation_id;
        item.seq = m.seq;
        item.ts = m.ts;
        state.items.push_back(item);
        if ((state.minSeq == 0) || (m.seq < state.minSeq)) {
            state.minSeq = m.seq;   // 上拉翻页用当前最小 seq
        }
        hasNewItems = true;
    }
    state.historyLoaded = true;

    if (groupId == m_currentGroup) {
        RenderGroupMessages(groupId);
        MarkGroupRead(groupId);
    }
    else if (hasNewItems) {
        EnsureConversationGroupRow(groupId, state.items.empty() ? std::string() : state.items.back().text);
    }
}

void ChatForm::SendCurrentGroupMessage(uint64_t groupId, const std::string& text)
{
    if (!TcpClient::Instance().IsLoggedIn()) {
        AppendGroupBubble(true, std::string(), text + "  [发送失败: 未连接服务器]", nullptr);
        return;
    }
    if (m_nextTempMsgId == 0) {
        const auto nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        m_nextTempMsgId = static_cast<uint64_t>(nowMs);
    }
    const uint64_t tempMsgId = ++m_nextTempMsgId;

    ui::Label* bubble = nullptr;
    AppendGroupBubble(true, std::string(), text + "  [发送中...]", &bubble);

    PendingSend pending;
    pending.bubble = bubble;
    pending.isGroup = true;
    pending.groupId = groupId;
    pending.text = text;
    m_pendingSends.push_back(pending);

    ImsMessage msg;
    msg.type = static_cast<int>(ImsMsgType::Text);
    msg.to = groupId;
    msg.is_group = true;
    msg.msg_id = tempMsgId;
    msg.content = text;
    if (!TcpClient::Instance().Send(msg)) {
        HandleTcpError(ImsMessage());
    }
    EnsureConversationGroupRow(groupId, text);
}

void ChatForm::EnsureConversationGroupRow(uint64_t groupId, const std::string& text)
{
    const std::string key = ConvKey(true, groupId);
    auto iter = std::find(m_conversationOrder.begin(), m_conversationOrder.end(), key);
    if (iter != m_conversationOrder.end()) {
        if (iter != m_conversationOrder.begin()) {
            m_conversationOrder.erase(iter);
            m_conversationOrder.insert(m_conversationOrder.begin(), key);
        }
    }
    else {
        m_conversationOrder.insert(m_conversationOrder.begin(), key);
    }
    m_lastSnippet[key] = text;
    RenderConversationList();
}

// ---------------------------------------------------------------------------
// 群事件处理（type=140~144）（提示词11）
// ---------------------------------------------------------------------------
void ChatForm::HandleGroupEvent(int type, const ImsMessage& msg)
{
    auto logger = spdlog::get("im_file_logger");

    if ((type == static_cast<int>(ImsMsgType::GroupMemberChanged)) ||
        (type == static_cast<int>(ImsMsgType::GroupDismissed)) ||
        (type == static_cast<int>(ImsMsgType::GroupInfoChanged))) {
        ImsGroupEvent e;
        if (!ParseImsGroupEvent(msg.content, e)) {
            if (logger) {
                logger->warn("群事件 content 解析失败: {}", msg.content);
            }
            return;
        }
        const uint64_t selfId = ImsSelfId();

        if (type == static_cast<int>(ImsMsgType::GroupDismissed)) {
            const std::string name = e.name.empty() ? GroupDisplayName(e.groupId) : e.name;
            if (logger) {
                logger->info("群解散 groupId={} name={}", e.groupId, name);
            }
            ui::VListBox* ml = dynamic_cast<ui::VListBox*>(FindControl(_T("chat_message_list")));
            if (m_currentGroup == e.groupId && ml != nullptr) {
                ml->RemoveAllItems();
                AppendGroupBubble(false, std::string(), "群《" + name + "》已被解散", nullptr);
            }
            auto g = m_groups.find(e.groupId);
            if (g != m_groups.end()) {
                g->second.removed = true;
                g->second.info.status = 1;
            }
            for (size_t i = 0; i < m_groupList.size(); ++i) {
                if (m_groupList[i].groupId == e.groupId) {
                    m_groupList.erase(m_groupList.begin() + static_cast<long>(i));
                    break;
                }
            }
            m_groups.erase(e.groupId);
            RenderGroupList();
            RenderConversationList();
            return;
        }

        if (type == static_cast<int>(ImsMsgType::GroupInfoChanged)) {
            auto g = m_groups.find(e.groupId);
            if (g != m_groups.end() && !e.name.empty()) {
                g->second.info.name = e.name;
            }
            if (m_currentGroup == e.groupId) {
                ui::Label* titleLabel = dynamic_cast<ui::Label*>(FindControl(_T("chat_title_label")));
                if (titleLabel != nullptr) {
                    titleLabel->SetText(ui::StringConvert::UTF8ToT(GroupDisplayName(e.groupId)));
                }
            }
            RefreshGroupDetail(e.groupId);
            RenderGroupList();
            RenderConversationList();
            return;
        }

        // 140 GroupMemberChanged
        const bool tracked = (m_groups.count(e.groupId) != 0);
        if ((e.action == "added") && (e.userId == selfId)) {
            if (logger) {
                logger->info("我被加入群 groupId={} operator={}", e.groupId, e.operatorId);
            }
            GroupState& gs = m_groups[e.groupId];
            if (gs.info.groupId == 0) {
                gs.info.groupId = e.groupId;
            }
            RefreshGroupDetail(e.groupId);
            RefreshGroupList();
        }
        else if (((e.action == "removed") || (e.action == "quit")) && (e.userId == selfId)) {
            if (logger) {
                logger->info("我已不在群 groupId={} action={}", e.groupId, e.action);
            }
            if (m_currentGroup == e.groupId) {
                ui::VListBox* ml = dynamic_cast<ui::VListBox*>(FindControl(_T("chat_message_list")));
                if (ml != nullptr) {
                    ml->RemoveAllItems();
                    AppendGroupBubble(false, std::string(), "你已不在该群", nullptr);
                }
            }
            m_groups.erase(e.groupId);
            for (size_t i = 0; i < m_groupList.size(); ++i) {
                if (m_groupList[i].groupId == e.groupId) {
                    m_groupList.erase(m_groupList.begin() + static_cast<long>(i));
                    break;
                }
            }
            RenderGroupList();
            RenderConversationList();
        }
        else if ((e.action == "transferred") || (e.action == "roleChanged")) {
            if (tracked || e.userId == selfId) {
                RefreshGroupDetail(e.groupId);
            }
            if (logger) {
                logger->info("群角色变化 groupId={} action={} user={}", e.groupId, e.action, e.userId);
            }
        }
        else if (tracked) {
            RefreshGroupDetail(e.groupId);
        }
        return;
    }

    if ((type == static_cast<int>(ImsMsgType::GroupRequest)) ||
        (type == static_cast<int>(ImsMsgType::GroupRequestResult))) {
        std::string event;
        ImsGroupRequestInfo info;
        if (!ParseImsGroupRequestEvent(msg.content, event, info)) {
            if (logger) {
                logger->warn("入群申请事件解析失败: {}", msg.content);
            }
            return;
        }
        if (info.requestId == 0) {
            info.requestId = msg.msg_id;   // msg_id 复用为 requestId
        }
        if (type == static_cast<int>(ImsMsgType::GroupRequest)) {
            m_groupRequestGroups.insert(info.groupId);
            if (logger) {
                logger->info("收到入群申请 event={} groupId={} requester={}", event, info.groupId, info.RequesterName());
            }
            UpdateGroupRequestRedDot();
            ui::Box* panel = dynamic_cast<ui::Box*>(FindControl(_T("join_request_panel")));
            if ((panel != nullptr) && panel->IsVisible()) {
                RefreshGroupRequests();
            }
        }
        else {
            const bool accepted = (event == "accepted");
            m_outgoingRequestState[info.requestId] = accepted ? "已通过" : "已拒绝";
            if (logger) {
                logger->info("入群申请结果 requestId={} event={}", info.requestId, event);
            }
            if (accepted) {
                GroupState& gs = m_groups[info.groupId];
                if (gs.info.groupId == 0) {
                    gs.info.groupId = info.groupId;
                }
                RefreshGroupList();
            }
        }
        return;
    }
}

// ---------------------------------------------------------------------------
// 群列表 / 群详情（HTTP）
// ---------------------------------------------------------------------------
void ChatForm::RefreshGroupList()
{
    auto ctx = std::make_shared<std::pair<HttpClient::ApiResult, std::vector<ImsGroupInfo>>>();
    RunAsync([ctx]() {
        ctx->first = HttpClient::Instance().GetGroups(ctx->second);
    }, [this, ctx]() {
        if (!ctx->first.ok) {
            if (ctx->first.unauthorized) {
                HandleApiUnauthorized(ctx->first.error);
            }
            return;
        }
        m_groupList = ctx->second;
        std::set<uint64_t> present;
        for (const ImsGroupInfo& g : m_groupList) {
            present.insert(g.groupId);
            GroupState& gs = m_groups[g.groupId];
            if (gs.info.groupId == 0) {
                gs.info = g;
            }
            else {
                gs.info.name = g.name;
                gs.info.announcement = g.announcement;
                gs.info.memberCount = g.memberCount;
                gs.info.myRole = g.myRole;
                gs.info.status = g.status;
                gs.info.conversationId = g.conversationId;
                gs.info.ownerId = g.ownerId;
                gs.info.memberLimit = g.memberLimit;
                gs.removed = false;
            }
        }
        // 不在群列表里的本地群（被移出/解散离线丢失时兜底）
        for (auto& kv : m_groups) {
            if ((present.count(kv.first) == 0) && !kv.second.removed) {
                kv.second.removed = true;
                if (m_currentGroup == kv.first) {
                    m_currentGroup = 0;
                }
            }
        }
        if (m_currentGroup != 0 && present.count(m_currentGroup) != 0) {
            RefreshGroupDetail(m_currentGroup);
        }
        RenderGroupList();
        RenderConversationList();
        auto logger = spdlog::get("im_file_logger");
        if (logger) {
            logger->info("群列表刷新完成，共 {} 个群", m_groupList.size());
        }
    });
}

void ChatForm::RenderGroupList()
{
    ui::VListBox* groupList = dynamic_cast<ui::VListBox*>(FindControl(_T("group_list")));
    if (groupList == nullptr) {
        return;
    }
    groupList->RemoveAllItems();

    for (const ImsGroupInfo& g : m_groupList) {
        if (g.status != 0) {
            continue;
        }
        const uint64_t gid = g.groupId;
        ui::ListBoxItemH* item = new ui::ListBoxItemH(this);
        item->SetAttribute(_T("width"), _T("stretch"));
        item->SetAttribute(_T("height"), _T("46"));
        item->SetAttribute(_T("padding"), _T("10,0,10,0"));
        item->SetAttribute(_T("border_round"), _T("6,6"));
        item->SetAttribute(_T("hovered_color"), _T("#FFF2F7FC"));
        item->SetAttribute(_T("pressed_color"), _T("#FFE6F1FB"));
        item->SetAttribute(_T("fade_hovered"), _T("false"));

        ui::Control* icon = new ui::Control(this);
        icon->SetAttribute(_T("width"), _T("34"));
        icon->SetAttribute(_T("height"), _T("34"));
        icon->SetAttribute(_T("valign"), _T("center"));
        icon->SetAttribute(_T("mouse_enabled"), _T("false"));
        icon->SetAttribute(_T("normal_image"), _T("file='wechat_icon_group.svg' width='30' height='30' valign='center' halign='center'"));

        ui::VBox* textBox = new ui::VBox(this);
        textBox->SetAttribute(_T("width"), _T("stretch"));
        textBox->SetAttribute(_T("height"), _T("stretch"));
        textBox->SetAttribute(_T("mouse_enabled"), _T("false"));

        ui::Label* nameLabel = new ui::Label(this);
        nameLabel->SetAttribute(_T("height"), _T("22"));
        nameLabel->SetAttribute(_T("margin"), _T("0,6,0,0"));
        nameLabel->SetAttribute(_T("font"), _T("system_regular_13"));
        nameLabel->SetAttribute(_T("normal_text_color"), _T("#FF1F2329"));
        nameLabel->SetAttribute(_T("mouse_enabled"), _T("false"));
        nameLabel->SetAttribute(_T("text"), ui::StringConvert::UTF8ToT(g.name));

        ui::Label* countLabel = new ui::Label(this);
        countLabel->SetAttribute(_T("height"), _T("18"));
        countLabel->SetAttribute(_T("font"), _T("system_regular_11"));
        countLabel->SetAttribute(_T("normal_text_color"), _T("#FF9AA1AC"));
        countLabel->SetAttribute(_T("mouse_enabled"), _T("false"));
        countLabel->SetAttribute(_T("text"), ui::StringConvert::UTF8ToT(std::to_string(g.memberCount) + " 人"));

        textBox->AddItem(nameLabel);
        textBox->AddItem(countLabel);
        item->AddItem(icon);
        item->AddItem(textBox);
        item->AttachClick([this, gid](const ui::EventArgs&) {
            SelectGroup(gid);
            return true;
        });
        groupList->AddItem(item);
    }
}

void ChatForm::RefreshGroupDetail(uint64_t groupId)
{
    auto ctx = std::make_shared<std::tuple<HttpClient::ApiResult, ImsGroupInfo, std::vector<ImsGroupMember>>>();
    RunAsync([ctx, groupId]() {
        ImsGroupInfo& g = std::get<1>(*ctx);
        std::vector<ImsGroupMember>& ms = std::get<2>(*ctx);
        std::get<0>(*ctx) = HttpClient::Instance().GetGroupDetail(groupId, g, ms);
    }, [this, ctx, groupId]() {
        const HttpClient::ApiResult& result = std::get<0>(*ctx);
        if (!result.ok) {
            if (result.unauthorized) {
                HandleApiUnauthorized(result.error);
            }
            auto logger = spdlog::get("im_file_logger");
            if (logger) {
                logger->warn("群详情刷新失败 groupId={} err={}", groupId, result.error);
            }
            return;
        }
        const ImsGroupInfo& g = std::get<1>(*ctx);
        const std::vector<ImsGroupMember>& ms = std::get<2>(*ctx);
        GroupState& gs = m_groups[groupId];
        gs.info = g;
        gs.memberNames.clear();
        for (const ImsGroupMember& m : ms) {
            gs.memberNames[m.userId] = m.DisplayName();
        }
        if (m_currentGroup == groupId) {
            ui::Label* titleLabel = dynamic_cast<ui::Label*>(FindControl(_T("chat_title_label")));
            if (titleLabel != nullptr) {
                titleLabel->SetText(ui::StringConvert::UTF8ToT(GroupDisplayName(groupId)));
            }
            UpdateGroupAnnouncementBar();
        }
        ui::Box* panel = dynamic_cast<ui::Box*>(FindControl(_T("group_member_panel")));
        if ((panel != nullptr) && panel->IsVisible()) {
            RenderGroupMembers(ms);
        }
        RenderGroupList();
    });
}

void ChatForm::ShowGroupMemberPanel(uint64_t groupId)
{
    HideOverlayPanels();
    ui::Box* panel = dynamic_cast<ui::Box*>(FindControl(_T("group_member_panel")));
    if (panel != nullptr) {
        panel->SetVisible(true);
    }
    auto g = m_groups.find(groupId);
    if (g != m_groups.end()) {
        std::vector<ImsGroupMember> ms;
        for (const auto& kv : g->second.memberNames) {
            ImsGroupMember m; m.userId = kv.first; m.groupNickname = kv.second; ms.push_back(m);
        }
        RenderGroupMembers(ms);
    }
    RefreshGroupDetail(groupId);
}

void ChatForm::RenderGroupMembers(const std::vector<ImsGroupMember>& members)
{
    ui::VListBox* list = dynamic_cast<ui::VListBox*>(FindControl(_T("group_member_list")));
    if (list == nullptr) {
        return;
    }
    list->RemoveAllItems();
    for (const ImsGroupMember& m : members) {
        ui::ListBoxItemH* item = new ui::ListBoxItemH(this);
        item->SetAttribute(_T("width"), _T("stretch"));
        item->SetAttribute(_T("height"), _T("36"));
        item->SetAttribute(_T("padding"), _T("8,0,8,0"));

        ui::Label* nameLabel = new ui::Label(this);
        nameLabel->SetAttribute(_T("width"), _T("stretch"));
        nameLabel->SetAttribute(_T("height"), _T("auto"));
        nameLabel->SetAttribute(_T("valign"), _T("center"));
        nameLabel->SetAttribute(_T("font"), _T("system_regular_12"));
        nameLabel->SetAttribute(_T("normal_text_color"), _T("#FF1F2329"));
        nameLabel->SetAttribute(_T("mouse_enabled"), _T("false"));
        std::string disp = !m.groupNickname.empty() ? m.groupNickname : m.DisplayName();
        nameLabel->SetAttribute(_T("text"), ui::StringConvert::UTF8ToT(disp));

        const char* roleText = "成员";
        if (m.role == 1) {
            roleText = "群主";
        }
        else if (m.role == 2) {
            roleText = "管理员";
        }
        ui::Label* roleLabel = new ui::Label(this);
        roleLabel->SetAttribute(_T("width"), _T("52"));
        roleLabel->SetAttribute(_T("height"), _T("auto"));
        roleLabel->SetAttribute(_T("valign"), _T("center"));
        roleLabel->SetAttribute(_T("font"), _T("system_regular_11"));
        roleLabel->SetAttribute(_T("normal_text_color"), _T("#FF2C9BD6"));
        roleLabel->SetAttribute(_T("mouse_enabled"), _T("false"));
        roleLabel->SetAttribute(_T("text"), ui::StringConvert::UTF8ToT(roleText));

        item->AddItem(nameLabel);
        item->AddItem(roleLabel);
        list->AddItem(item);
    }
}

void ChatForm::ShowGroupOpMenu(uint64_t groupId)
{
    m_opPanelGroupId = groupId;
    HideOverlayPanels();
    ui::Box* panel = dynamic_cast<ui::Box*>(FindControl(_T("group_op_panel")));
    if (panel == nullptr) {
        return;
    }
    panel->SetVisible(true);

    const ImsGroupInfo* info = FindGroupInfo(groupId);
    const int myRole = (info != nullptr) ? info->myRole : 0;

    ui::Label* title = dynamic_cast<ui::Label*>(FindControl(_T("group_op_title")));
    if (title != nullptr) {
        std::string roleText = "普通成员";
        if (myRole == 1) {
            roleText = "群主";
        }
        else if (myRole == 2) {
            roleText = "管理员";
        }
        title->SetText(ui::StringConvert::UTF8ToT("群管理 - " + GroupDisplayName(groupId) + "（" + roleText + "）"));
    }

    const auto show = [this](const DString& name, bool visible) {
        ui::Button* b = dynamic_cast<ui::Button*>(FindControl(name));
        if (b != nullptr) {
            b->SetVisible(visible);
        }
    };
    const bool owner = (myRole == 1);
    const bool admin = (myRole == 2);
    show(_T("btn_group_update"), owner || admin);
    show(_T("btn_group_invite"), owner || admin);
    show(_T("btn_group_remove"), owner || admin);
    show(_T("btn_group_admin"), owner);
    show(_T("btn_group_transfer"), owner);
    show(_T("btn_group_dismiss"), owner);
    show(_T("btn_group_quit"), !owner);
}

void ChatForm::DoGroupMenuAction(uint64_t groupId, const std::string& action)
{
    const ImsGroupInfo* info = FindGroupInfo(groupId);
    const int myRole = (info != nullptr) ? info->myRole : 0;
    auto logger = spdlog::get("im_file_logger");

    if ((action == "quit") && (myRole == 1)) {
        ui::Box* panel = dynamic_cast<ui::Box*>(FindControl(_T("group_op_panel")));
        ui::Label* hint = panel ? dynamic_cast<ui::Label*>(this->FindControl(_T("group_op_hint"))) : nullptr;
        if (hint != nullptr) {
            hint->SetText(_T("请先转让群主"));
        }
        return;
    }

    auto finish = [this, groupId, action, logger](const HttpClient::ApiResult& r, const std::string& okText) {
        if (r.unauthorized) {
            HandleApiUnauthorized(r.error);
            return;
        }
        ui::Box* panel = dynamic_cast<ui::Box*>(FindControl(_T("group_op_panel")));
        ui::Label* hint = panel ? dynamic_cast<ui::Label*>(this->FindControl(_T("group_op_hint"))) : nullptr;
        if (logger) {
            logger->info("群操作 {} groupId={} ok={} err={}", action, groupId, r.ok, r.error);
        }
        if (!r.ok) {
            if (hint != nullptr) {
                hint->SetText(ui::StringConvert::UTF8ToT(r.error));   // 400 文案原样提示
            }
            return;
        }
        if (hint != nullptr) {
            hint->SetText(ui::StringConvert::UTF8ToT(okText));
        }
        RefreshGroupDetail(groupId);
        RefreshGroupList();
    };

    if (action == "dismiss") {
        auto ctx = std::make_shared<HttpClient::ApiResult>();
        RunAsync([ctx, groupId]() { *ctx = HttpClient::Instance().DismissGroup(groupId); },
            [finish, ctx]() { finish(*ctx, "已解散该群"); });
    }
    else if (action == "quit") {
        auto ctx = std::make_shared<HttpClient::ApiResult>();
        RunAsync([ctx, groupId]() { *ctx = HttpClient::Instance().QuitGroup(groupId); },
            [this, finish, ctx, groupId]() {
                finish(*ctx, "已退出该群");
                if (ctx->ok) {
                    m_groups.erase(groupId);
                    if (m_currentGroup == groupId) {
                        m_currentGroup = 0;
                    }
                    RenderGroupList();
                    RenderConversationList();
                }
            });
    }
    else if (action == "update") {
        ui::Box* panel = dynamic_cast<ui::Box*>(FindControl(_T("group_update_panel")));
        if (panel == nullptr) {
            return;
        }
        ui::RichEdit2* nameEdit = dynamic_cast<ui::RichEdit2*>(this->FindControl(_T("group_name_edit")));
        ui::RichEdit2* annEdit = dynamic_cast<ui::RichEdit2*>(this->FindControl(_T("group_announcement_edit")));
        const std::string name = (nameEdit != nullptr) ? ImsTrim(ui::StringConvert::TToUTF8(nameEdit->GetText())) : std::string();
        const std::string ann = (annEdit != nullptr) ? ImsTrim(ui::StringConvert::TToUTF8(annEdit->GetText())) : std::string();
        if (name.size() > 128) {
            ui::Label* hint = dynamic_cast<ui::Label*>(this->FindControl(_T("group_update_hint")));
            if (hint != nullptr) {
                hint->SetText(_T("群名最大 128 字节"));
            }
            return;
        }
        auto ctx = std::make_shared<HttpClient::ApiResult>();
        RunAsync([ctx, groupId, name, ann]() {
            *ctx = HttpClient::Instance().UpdateGroup(groupId, name, ann, "", "");
        }, [this, finish, ctx, groupId]() {
            finish(*ctx, "群资料已更新");
            ui::Box* p = dynamic_cast<ui::Box*>(FindControl(_T("group_update_panel")));
            if (p != nullptr) {
                p->SetVisible(false);
            }
        });
    }
    else if (action == "invite") {
        ShowInviteDialog(groupId);
    }
    else if (action == "remove") {
        ui::Box* panel = dynamic_cast<ui::Box*>(FindControl(_T("group_remove_panel")));
        if (panel == nullptr) {
            return;
        }
        ui::RichEdit2* idEdit = dynamic_cast<ui::RichEdit2*>(this->FindControl(_T("group_remove_uid_edit")));
        const std::string idStr = (idEdit != nullptr) ? ImsTrim(ui::StringConvert::TToUTF8(idEdit->GetText())) : std::string();
        uint64_t uid = 0;
        try { uid = idStr.empty() ? 0 : std::stoull(idStr); } catch (...) { uid = 0; }
        if (uid == 0) {
            ui::Label* hint = dynamic_cast<ui::Label*>(this->FindControl(_T("group_remove_hint")));
            if (hint != nullptr) {
                hint->SetText(_T("请输入要移出成员的 userId"));
            }
            return;
        }
        auto ctx = std::make_shared<HttpClient::ApiResult>();
        RunAsync([ctx, groupId, uid]() { *ctx = HttpClient::Instance().RemoveMember(groupId, uid); },
            [this, finish, ctx, groupId]() {
                finish(*ctx, "已移出成员");
                ui::Box* p = dynamic_cast<ui::Box*>(FindControl(_T("group_remove_panel")));
                if (p != nullptr) {
                    p->SetVisible(false);
                }
            });
    }
    else if (action == "admin") {
        ui::Box* panel = dynamic_cast<ui::Box*>(FindControl(_T("group_admin_panel")));
        if (panel == nullptr) {
            return;
        }
        ui::RichEdit2* idEdit = dynamic_cast<ui::RichEdit2*>(this->FindControl(_T("group_admin_uid_edit")));
        ui::CheckBox* isAdmin = dynamic_cast<ui::CheckBox*>(this->FindControl(_T("group_admin_set_check")));
        const std::string idStr = (idEdit != nullptr) ? ImsTrim(ui::StringConvert::TToUTF8(idEdit->GetText())) : std::string();
        uint64_t uid = 0;
        try { uid = idStr.empty() ? 0 : std::stoull(idStr); } catch (...) { uid = 0; }
        const bool setAdmin = (isAdmin != nullptr) && isAdmin->IsSelected();
        if (uid == 0) {
            ui::Label* hint = dynamic_cast<ui::Label*>(this->FindControl(_T("group_admin_hint")));
            if (hint != nullptr) {
                hint->SetText(_T("请输入 userId"));
            }
            return;
        }
        auto ctx = std::make_shared<HttpClient::ApiResult>();
        RunAsync([ctx, groupId, uid, setAdmin]() { *ctx = HttpClient::Instance().SetAdmin(groupId, uid, setAdmin); },
            [this, finish, ctx, groupId]() {
                finish(*ctx, "管理员设置已更新");
                ui::Box* p = dynamic_cast<ui::Box*>(FindControl(_T("group_admin_panel")));
                if (p != nullptr) {
                    p->SetVisible(false);
                }
            });
    }
    else if (action == "transfer") {
        ui::Box* panel = dynamic_cast<ui::Box*>(FindControl(_T("group_transfer_panel")));
        if (panel == nullptr) {
            return;
        }
        ui::RichEdit2* idEdit = dynamic_cast<ui::RichEdit2*>(this->FindControl(_T("group_transfer_uid_edit")));
        const std::string idStr = (idEdit != nullptr) ? ImsTrim(ui::StringConvert::TToUTF8(idEdit->GetText())) : std::string();
        uint64_t uid = 0;
        try { uid = idStr.empty() ? 0 : std::stoull(idStr); } catch (...) { uid = 0; }
        if (uid == 0) {
            ui::Label* hint = dynamic_cast<ui::Label*>(this->FindControl(_T("group_transfer_hint")));
            if (hint != nullptr) {
                hint->SetText(_T("请输入新群主 userId"));
            }
            return;
        }
        auto ctx = std::make_shared<HttpClient::ApiResult>();
        RunAsync([ctx, groupId, uid]() { *ctx = HttpClient::Instance().TransferGroup(groupId, uid); },
            [this, finish, ctx, groupId]() {
                finish(*ctx, "已转让群主");
                ui::Box* p = dynamic_cast<ui::Box*>(FindControl(_T("group_transfer_panel")));
                if (p != nullptr) {
                    p->SetVisible(false);
                }
            });
    }
}

void ChatForm::ShowCreateGroupDialog()
{
    HideOverlayPanels();
    ui::Box* panel = dynamic_cast<ui::Box*>(FindControl(_T("create_group_panel")));
    if (panel == nullptr) {
        return;
    }
    m_inviteTargetGroupId = 0;   // 建群模式
    panel->SetVisible(true);
    ui::RichEdit2* nameEdit = dynamic_cast<ui::RichEdit2*>(this->FindControl(_T("new_group_name_edit")));
    if (nameEdit != nullptr) {
        nameEdit->SetVisible(true);
        nameEdit->SetText(_T(""));
    }
    ui::Button* btnCreate = dynamic_cast<ui::Button*>(this->FindControl(_T("btn_create_group")));
    if (btnCreate != nullptr) {
        btnCreate->SetText(_T("创建"));   // 点击回调只在 InitIms 绑定一次，这里不再 AttachClick
    }
    ui::Label* hint = dynamic_cast<ui::Label*>(this->FindControl(_T("create_group_hint")));
    if (hint != nullptr) {
        hint->SetText(_T("加载中..."));
    }
    RenderCreateGroupFriends();
    auto ctx = std::make_shared<std::pair<HttpClient::ApiResult, std::vector<FriendInfo>>>();
    RunAsync([ctx]() { ctx->first = HttpClient::Instance().GetFriends(ctx->second); },
        [this, ctx]() {
            if (ctx->first.ok) {
                RenderCreateGroupFriends(ctx->second);
            }
        });
}

void ChatForm::RenderCreateGroupFriends()
{
    std::vector<FriendInfo> fs;
    for (const auto& kv : m_friends) {
        fs.push_back(kv.second);
    }
    RenderCreateGroupFriends(fs);
}

void ChatForm::RenderCreateGroupFriends(const std::vector<FriendInfo>& friends)
{
    ui::VListBox* list = dynamic_cast<ui::VListBox*>(FindControl(_T("create_group_friend_list")));
    ui::Label* hint = dynamic_cast<ui::Label*>(FindControl(_T("create_group_hint")));
    if (list == nullptr) {
        return;
    }
    list->RemoveAllItems();
    if (m_createGroupSelected == nullptr) {
        m_createGroupSelected = std::make_shared<std::set<uint64_t>>();
    }
    m_createGroupSelected->clear();
    if (friends.empty()) {
        if (hint != nullptr) {
            hint->SetText(_T("请先添加好友（建群初始成员须为好友）"));
        }
        return;
    }
    if (hint != nullptr && m_inviteTargetGroupId == 0) {
        hint->SetText(_T("勾选成员并输入群名"));
    }
    for (const FriendInfo& f : friends) {
        const uint64_t uid = f.userId;
        ui::ListBoxItemH* item = new ui::ListBoxItemH(this);
        item->SetAttribute(_T("width"), _T("stretch"));
        item->SetAttribute(_T("height"), _T("34"));
        item->SetAttribute(_T("padding"), _T("8,0,8,0"));
        item->SetAttribute(_T("border_round"), _T("4,4"));
        item->SetAttribute(_T("hovered_color"), _T("#FFF2F7FC"));
        item->SetAttribute(_T("fade_hovered"), _T("false"));

        // 可见的勾选标记：☑/☐，点击整行切换（比不可见的 CheckBox 更易用）
        ui::Label* mark = new ui::Label(this);
        mark->SetAttribute(_T("width"), _T("24"));
        mark->SetAttribute(_T("height"), _T("auto"));
        mark->SetAttribute(_T("valign"), _T("center"));
        mark->SetAttribute(_T("halign"), _T("center"));
        mark->SetAttribute(_T("font"), _T("system_regular_14"));
        mark->SetAttribute(_T("normal_text_color"), _T("#FF2C9BD6"));
        mark->SetAttribute(_T("mouse_enabled"), _T("false"));
        mark->SetAttribute(_T("text"), _T("☐"));

        ui::Label* nameLabel = new ui::Label(this);
        nameLabel->SetAttribute(_T("width"), _T("stretch"));
        nameLabel->SetAttribute(_T("height"), _T("auto"));
        nameLabel->SetAttribute(_T("valign"), _T("center"));
        nameLabel->SetAttribute(_T("font"), _T("system_regular_12"));
        nameLabel->SetAttribute(_T("normal_text_color"), _T("#FF1F2329"));
        nameLabel->SetAttribute(_T("mouse_enabled"), _T("false"));
        nameLabel->SetAttribute(_T("text"), ui::StringConvert::UTF8ToT(f.DisplayName() + " (" + std::to_string(uid) + ")"));

        auto selected = m_createGroupSelected;
        item->AttachClick([selected, uid, mark](const ui::EventArgs&) {
            bool now = false;
            if (selected->count(uid) != 0) {
                selected->erase(uid);
            }
            else {
                selected->insert(uid);
                now = true;
            }
            mark->SetText(now ? _T("☑") : _T("☐"));
            return true;
        });

        item->AddItem(mark);
        item->AddItem(nameLabel);
        list->AddItem(item);
    }
}

void ChatForm::DoCreateGroup()
{
    ui::Box* panel = dynamic_cast<ui::Box*>(FindControl(_T("create_group_panel")));
    if (panel == nullptr) {
        return;
    }
    ui::RichEdit2* nameEdit = dynamic_cast<ui::RichEdit2*>(this->FindControl(_T("new_group_name_edit")));
    ui::Label* hint = dynamic_cast<ui::Label*>(this->FindControl(_T("create_group_hint")));
    const std::string name = (nameEdit != nullptr) ? ImsTrim(ui::StringConvert::TToUTF8(nameEdit->GetText())) : std::string();
    std::vector<uint64_t> ids;
    if (m_createGroupSelected != nullptr) {
        for (uint64_t uid : *m_createGroupSelected) {
            ids.push_back(uid);
        }
    }
    if (name.empty() || (name.size() > 128)) {
        if (hint != nullptr) {
            hint->SetText(_T("群名不能为空且 ≤128 字节"));
        }
        return;
    }
    if (ids.empty()) {
        if (hint != nullptr) {
            hint->SetText(_T("至少选择 1 位好友成员"));
        }
        return;
    }
    m_groupSubmitting = true;
    auto ctx = std::make_shared<std::pair<HttpClient::ApiResult, ImsGroupInfo>>();
    RunAsync([ctx, name, ids]() { ctx->first = HttpClient::Instance().CreateGroup(name, ids, ctx->second); },
        [this, ctx, panel]() {
            m_groupSubmitting = false;
            if (ctx->first.unauthorized) {
                HandleApiUnauthorized(ctx->first.error);
                return;
            }
            ui::Label* hint = dynamic_cast<ui::Label*>(this->FindControl(_T("create_group_hint")));
            if (!ctx->first.ok) {
                if (hint != nullptr) {
                    hint->SetText(ui::StringConvert::UTF8ToT(ctx->first.error));
                }
                return;
            }
            if (hint != nullptr) {
                hint->SetText(_T("建群成功"));
            }
            panel->SetVisible(false);
            const uint64_t gid = ctx->second.groupId;
            GroupState& gs = m_groups[gid];
            gs.info = ctx->second;
            RefreshGroupDetail(gid);
            RefreshGroupList();
            SelectGroup(gid);
        });
}

// 建群/邀请对话框“确定”按钮的统一入口（只在 InitIms 绑定一次，避免重复 AttachClick 造成重复创建）
void ChatForm::OnCreateDialogConfirm()
{
    if (m_groupSubmitting) {
        return;   // 防抖：一次提交未完成前忽略重复点击
    }
    if (m_inviteTargetGroupId == 0) {
        DoCreateGroup();
    }
    else {
        DoInviteSelected(m_inviteTargetGroupId);
    }
}

void ChatForm::DoInviteSelected(uint64_t groupId)
{
    std::vector<uint64_t> ids;
    if (m_createGroupSelected != nullptr) {
        for (uint64_t uid : *m_createGroupSelected) {
            ids.push_back(uid);
        }
    }
    ui::Box* p = dynamic_cast<ui::Box*>(FindControl(_T("create_group_panel")));
    ui::Label* h = p ? dynamic_cast<ui::Label*>(this->FindControl(_T("create_group_hint"))) : nullptr;
    if (ids.empty()) {
        if (h != nullptr) {
            h->SetText(_T("至少选择 1 位好友"));
        }
        return;
    }
    m_groupSubmitting = true;
    auto ctx = std::make_shared<HttpClient::ApiResult>();
    RunAsync([ctx, groupId, ids]() { *ctx = HttpClient::Instance().InviteMembers(groupId, ids); },
        [this, ctx, groupId, p]() {
            m_groupSubmitting = false;
            if (ctx->unauthorized) {
                HandleApiUnauthorized(ctx->error);
                return;
            }
            ui::Label* h2 = p ? dynamic_cast<ui::Label*>(this->FindControl(_T("create_group_hint"))) : nullptr;
            if (h2 != nullptr) {
                h2->SetText(ui::StringConvert::UTF8ToT(ctx->ok ? std::string("已邀请") : ctx->error));
            }
            if (ctx->ok) {
                if (p != nullptr) {
                    p->SetVisible(false);
                }
                m_inviteTargetGroupId = 0;
                RefreshGroupDetail(groupId);
                RefreshGroupList();
            }
        });
}

void ChatForm::ShowInviteDialog(uint64_t groupId)
{
    HideOverlayPanels();
    ui::Box* panel = dynamic_cast<ui::Box*>(FindControl(_T("create_group_panel")));
    if (panel == nullptr) {
        return;
    }
    m_inviteTargetGroupId = groupId;
    panel->SetVisible(true);
    ui::RichEdit2* nameEdit = dynamic_cast<ui::RichEdit2*>(this->FindControl(_T("new_group_name_edit")));
    if (nameEdit != nullptr) {
        nameEdit->SetVisible(false);
    }
    ui::Button* btnCreate = dynamic_cast<ui::Button*>(this->FindControl(_T("btn_create_group")));
    if (btnCreate != nullptr) {
        btnCreate->SetText(_T("邀请"));   // 点击回调只在 InitIms 绑定一次，这里不再 AttachClick
    }
    ui::Label* hint = dynamic_cast<ui::Label*>(this->FindControl(_T("create_group_hint")));
    if (hint != nullptr) {
        hint->SetText(_T("勾选要邀请的好友"));
    }
    RenderCreateGroupFriends();
}

void ChatForm::RefreshGroupRequests()
{
    auto ctx = std::make_shared<std::pair<HttpClient::ApiResult, std::vector<ImsGroupRequestInfo>>>();
    RunAsync([ctx]() { ctx->first = HttpClient::Instance().GetGroupRequests("incoming", ctx->second); },
        [this, ctx]() {
            if (!ctx->first.ok) {
                if (ctx->first.unauthorized) {
                    HandleApiUnauthorized(ctx->first.error);
                }
                return;
            }
            m_groupRequestGroups.clear();
            std::vector<ImsGroupRequestInfo> pending;
            for (const ImsGroupRequestInfo& r : ctx->second) {
                if (r.status == 0) {
                    pending.push_back(r);
                    m_groupRequestGroups.insert(r.groupId);
                }
            }
            UpdateGroupRequestRedDot();
            ui::Box* panel = dynamic_cast<ui::Box*>(FindControl(_T("join_request_panel")));
            if ((panel != nullptr) && panel->IsVisible()) {
                RenderJoinRequests(pending);
            }
        });
}

void ChatForm::UpdateGroupRequestRedDot()
{
    const int count = static_cast<int>(m_groupRequestGroups.size());
    ui::Button* btn = dynamic_cast<ui::Button*>(FindControl(_T("btn_group_requests")));
    if (btn == nullptr) {
        return;
    }
    if (count > 0) {
        btn->SetVisible(true);
        btn->SetText(ui::StringConvert::UTF8ToT("入群申请(" + std::to_string(count) + ")"));
    }
    else {
        btn->SetVisible(false);
    }
}

void ChatForm::ShowJoinRequestPanel()
{
    HideOverlayPanels();
    ui::Box* panel = dynamic_cast<ui::Box*>(FindControl(_T("join_request_panel")));
    if (panel != nullptr) {
        panel->SetVisible(true);
    }
    RefreshGroupRequests();
}

void ChatForm::RenderJoinRequests(const std::vector<ImsGroupRequestInfo>& requests)
{
    ui::VListBox* list = dynamic_cast<ui::VListBox*>(FindControl(_T("join_request_list")));
    if (list == nullptr) {
        return;
    }
    list->RemoveAllItems();
    if (requests.empty()) {
        ui::ListBoxItemH* item = new ui::ListBoxItemH(this);
        item->SetAttribute(_T("width"), _T("stretch"));
        item->SetAttribute(_T("height"), _T("40"));
        ui::Label* label = new ui::Label(this);
        label->SetAttribute(_T("width"), _T("stretch"));
        label->SetAttribute(_T("valign"), _T("center"));
        label->SetAttribute(_T("halign"), _T("center"));
        label->SetAttribute(_T("font"), _T("system_regular_12"));
        label->SetAttribute(_T("normal_text_color"), _T("#FF9AA1AC"));
        label->SetAttribute(_T("text"), _T("暂无入群申请"));
        item->AddItem(label);
        list->AddItem(item);
        return;
    }
    for (const ImsGroupRequestInfo& r : requests) {
        ui::ListBoxItemH* item = new ui::ListBoxItemH(this);
        item->SetAttribute(_T("width"), _T("stretch"));
        item->SetAttribute(_T("height"), _T("60"));
        item->SetAttribute(_T("padding"), _T("6,2,6,2"));

        ui::VBox* textBox = new ui::VBox(this);
        textBox->SetAttribute(_T("width"), _T("stretch"));
        textBox->SetAttribute(_T("height"), _T("stretch"));
        textBox->SetAttribute(_T("mouse_enabled"), _T("false"));

        const std::string name = r.RequesterName().empty() ? std::to_string(r.requesterId) : r.RequesterName();
        ui::Label* nameLabel = new ui::Label(this);
        nameLabel->SetAttribute(_T("height"), _T("22"));
        nameLabel->SetAttribute(_T("font"), _T("system_regular_12"));
        nameLabel->SetAttribute(_T("normal_text_color"), _T("#FF1F2329"));
        nameLabel->SetAttribute(_T("mouse_enabled"), _T("false"));
        nameLabel->SetAttribute(_T("text"),
            ui::StringConvert::UTF8ToT(name + " 申请加入《" + r.groupName + "》 " + ImsFormatClock(r.createdTs)));

        ui::Label* msgLabel = new ui::Label(this);
        msgLabel->SetAttribute(_T("height"), _T("20"));
        msgLabel->SetAttribute(_T("font"), _T("system_regular_11"));
        msgLabel->SetAttribute(_T("normal_text_color"), _T("#FF9AA1AC"));
        msgLabel->SetAttribute(_T("mouse_enabled"), _T("false"));
        msgLabel->SetAttribute(_T("text"), ui::StringConvert::UTF8ToT(r.message));

        textBox->AddItem(nameLabel);
        textBox->AddItem(msgLabel);
        item->AddItem(textBox);

        const uint64_t rid = r.requestId;
        ui::Button* accept = new ui::Button(this);
        accept->SetAttribute(_T("width"), _T("44"));
        accept->SetAttribute(_T("height"), _T("24"));
        accept->SetAttribute(_T("valign"), _T("center"));
        accept->SetAttribute(_T("border_round"), _T("4,4"));
        accept->SetAttribute(_T("bkcolor"), _T("#FF5FB8F0"));
        accept->SetAttribute(_T("normal_text_color"), _T("#FFFFFFFF"));
        accept->SetAttribute(_T("font"), _T("system_regular_11"));
        accept->SetAttribute(_T("text"), _T("通过"));
        accept->SetAttribute(_T("cursor_type"), _T("hand"));
        accept->AttachClick([this, rid](const ui::EventArgs&) {
            auto ctx = std::make_shared<HttpClient::ApiResult>();
            RunAsync([ctx, rid]() { *ctx = HttpClient::Instance().AcceptGroupRequest(rid); },
                [this, ctx, rid]() {
                    if (ctx->unauthorized) {
                        HandleApiUnauthorized(ctx->error);
                        return;
                    }
                    auto logger = spdlog::get("im_file_logger");
                    if (logger) {
                        logger->info("审批入群申请 accept rid={} ok={} err={}", rid, ctx->ok, ctx->error);
                    }
                    RefreshGroupRequests();
                    RefreshGroupList();
                });
            return true;
        });

        ui::Button* reject = new ui::Button(this);
        reject->SetAttribute(_T("width"), _T("44"));
        reject->SetAttribute(_T("height"), _T("24"));
        reject->SetAttribute(_T("valign"), _T("center"));
        reject->SetAttribute(_T("margin"), _T("6,0,0,0"));
        reject->SetAttribute(_T("border_round"), _T("4,4"));
        reject->SetAttribute(_T("bkcolor"), _T("#FFEFEFF4"));
        reject->SetAttribute(_T("normal_text_color"), _T("#FF5B5F6B"));
        reject->SetAttribute(_T("font"), _T("system_regular_11"));
        reject->SetAttribute(_T("text"), _T("拒绝"));
        reject->SetAttribute(_T("cursor_type"), _T("hand"));
        reject->AttachClick([this, rid](const ui::EventArgs&) {
            auto ctx = std::make_shared<HttpClient::ApiResult>();
            RunAsync([ctx, rid]() { *ctx = HttpClient::Instance().RejectGroupRequest(rid); },
                [this, ctx, rid]() {
                    if (ctx->unauthorized) {
                        HandleApiUnauthorized(ctx->error);
                        return;
                    }
                    RefreshGroupRequests();
                });
            return true;
        });

        item->AddItem(accept);
        item->AddItem(reject);
        list->AddItem(item);
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
