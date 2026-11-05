#include "MainThread.h"
#include "ChatForm.h"
#include "LoginForm.h"
#include "TencentImService.h"

MainThread::MainThread() :
    FrameworkThread(_T("MainThread"), ui::kThreadUI)
{
}

MainThread::~MainThread()
{
}

bool MainThread::OnInit()
{
    //初始化全局资源, 使用本地文件夹作为资源
    ui::FilePath resourcePath = ui::GlobalManager::GetResourceRootPath(false);
    ui::GlobalManager::Instance().Startup(ui::LocalFilesResParam(resourcePath));

#ifdef TENCENT_CLOUD_IM_SDK_VERSION
    TencentImService::Instance().Initialize();
#endif

    // 一个仿微信的布局示例
    //ChatForm::ShowCustomWindow(_T("basic_layout"), _T(""), _T("wechat.xml"));

    // 一个仿登录窗口的布局示例
    //ChatForm::ShowCustomWindow(_T("login"), _T(""), _T("login.xml"));

     // 创建一个默认带有阴影的居中窗口
    //LoginForm* window = new LoginForm(_T("login"), _T(""), _T("login.xml"));
    //if (!window->CreateWnd(nullptr, ui::WindowCreateParam(_T("克铖多媒体"), true))) {
    //    delete window;
    //    return false;
    //}
    //window->PostQuitMsgWhenClosed(true);
    //window->ShowWindow(ui::kSW_SHOW_NORMAL);

    LoginForm* window = new LoginForm(_T("login"), _T(""), _T("login.xml"));
    if (!window->CreateWnd(nullptr, ui::WindowCreateParam(_T("克铖多媒体"), true))) {
        delete window;
        return false;
    }
    window->PostQuitMsgWhenClosed(true);
    window->ShowWindow(ui::kSW_SHOW_NORMAL);

    return true;
}

void MainThread::OnCleanup()
{
#ifdef TENCENT_CLOUD_IM_SDK_VERSION
    TencentImService::Instance().Uninitialize();
#endif
    ui::GlobalManager::Instance().Shutdown();
}
