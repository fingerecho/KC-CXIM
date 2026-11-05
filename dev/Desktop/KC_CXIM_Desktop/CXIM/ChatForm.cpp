#include "ChatForm.h"
#include "TencentImService.h"

#include <algorithm>
#include <cctype>

ChatForm::ChatForm(const DString& class_name, const DString& theme_directory, const DString& layout_xml):
    m_alive(std::make_shared<bool>(true)),
    m_themeDirectory(theme_directory),
    m_layoutXml(layout_xml)
{
}


ChatForm::~ChatForm()
{
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
    m_version_label = dynamic_cast<ui::Label*>(FindControl(_T("my_version_label")));
    const char* labelText = TIMGetSDKVersion();
    if (m_version_label != nullptr) {
        m_version_label->SetText(ui::StringConvert::UTF8ToT(labelText));
    }

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
        navContacts->AttachClick([mainPages](const ui::EventArgs&) {
            if (mainPages != nullptr) {
                mainPages->SelectItem(_T("contacts_page"));
            }
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

void ChatForm::ShowCustomWindow(const DString& class_name, const DString& theme_directory, const DString& layout_xml)
{
    ChatForm* window = new ChatForm(class_name, theme_directory, layout_xml);
    window->CreateWnd(nullptr, ui::WindowCreateParam(_T("chat"), true));
    window->PostQuitMsgWhenClosed(true);
    window->ShowWindow(ui::kSW_SHOW_NORMAL);

}
