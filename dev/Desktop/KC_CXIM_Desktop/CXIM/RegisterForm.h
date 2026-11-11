#ifndef CXIM_REGISTER_FORM_H_
#define CXIM_REGISTER_FORM_H_

// duilib
#include "duilib/duilib.h"

#include "CXIMAppConfig.h"

#include <memory>

class RegisterForm : public ui::WindowImplBase
{
public:
    RegisterForm(const DString& class_name, const DString& theme_directory, const DString& layout_xml,
                 ui::Window* parent_window = nullptr);
    virtual ~RegisterForm() override;

    /** 资源相关接口
     * GetSkinFolder 接口设置你要绘制的窗口皮肤资源路径
     * GetSkinFile 接口设置你要绘制的窗口的 xml 描述文件
     */
    virtual DString GetSkinFolder() override;
    virtual DString GetSkinFile() override;

    /** 当窗口创建完成以后调用此函数，供子类中做一些初始化的工作
    */
    virtual void OnInitWindow() override;

    /** 窗口关闭时恢复父窗口的显示
    */
    virtual void OnCloseWindow() override;

public:
    static void ShowCustomWindow(const DString& class_name, const DString& theme_directory, const DString& layout_xml,
                                 ui::Window* parent = nullptr);

private:
    std::shared_ptr<bool> m_alive;
    DString m_themeDirectory;
    DString m_layoutXml;
    ui::Window* m_parentWindow = nullptr;
    bool m_exiting = false;
};

#endif //CXIM_REGISTER_FORM_H_
