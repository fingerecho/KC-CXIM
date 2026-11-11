#ifndef EXAMPLES_LAYOUTS_FORM_H_
#define EXAMPLES_LAYOUTS_FORM_H_

// duilib
#include "duilib/duilib.h"

#include "CXIMAppConfig.h"

#ifdef TENCENT_CLOUD_IM_SDK_VERSION
#include "TIMCloud.h"
#endif //!TENCENT_CLOUD_IM_SDK_VERSION


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
