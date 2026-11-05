#include "LoginForm.h"
#include "ChatForm.h"
#include "TencentImService.h"

#ifdef TENCENT_CLOUD_IM_SDK_VERSION

#include <Windows.h>
#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <string>

namespace {

std::string TrimString(const std::string& value)
{
    std::string result = value;
    const auto notSpace = [](unsigned char ch) {
        return std::isspace(ch) == 0;
    };
    result.erase(result.begin(), std::find_if(result.begin(), result.end(), notSpace));
    result.erase(std::find_if(result.rbegin(), result.rend(), notSpace).base(), result.end());
    return result;
}

std::filesystem::path GetExecutableDirectory()
{
    wchar_t buffer[MAX_PATH] = { 0 };
    const DWORD length = ::GetModuleFileNameW(nullptr, buffer, MAX_PATH);
    if (length == 0) {
        return std::filesystem::path();
    }
    return std::filesystem::path(std::wstring(buffer, length)).parent_path();
}

bool FindUserSig(const std::string& userId, std::string& userSig)
{
    std::ifstream file(GetExecutableDirectory() / "UserSig.csv");
    if (!file.is_open()) {
        return false;
    }

    std::string line;
    while (std::getline(file, line)) {
        line = TrimString(line);
        if (line.empty() || (line[0] == '#')) {
            continue;
        }
        const size_t comma = line.find(',');
        if (comma == std::string::npos) {
            continue;
        }
        const std::string id = TrimString(line.substr(0, comma));
        const std::string sig = TrimString(line.substr(comma + 1));
        if ((id == userId) && !sig.empty()) {
            userSig = sig;
            return true;
        }
    }
    return false;
}

} // namespace

#endif // TENCENT_CLOUD_IM_SDK_VERSION

LoginForm::LoginForm(const DString& class_name, const DString& theme_directory, const DString& layout_xml) :
    m_alive(std::make_shared<bool>(true)),
    m_themeDirectory(theme_directory),
    m_layoutXml(layout_xml)
{
}


LoginForm::~LoginForm()
{
    if (m_alive) {
        *m_alive = false;
    }
}

DString LoginForm::GetSkinFolder()
{
    return m_themeDirectory;
}

DString LoginForm::GetSkinFile()
{
    return m_layoutXml;
}

void LoginForm::OnInitWindow()
{
#ifdef TENCENT_CLOUD_IM_SDK_VERSION
    ui::RichEdit2* userEdit = dynamic_cast<ui::RichEdit2*>(FindControl(_T("username")));
    ui::Button* btnLogin = dynamic_cast<ui::Button*>(FindControl(_T("btn_login")));
    ui::Label* errorLabel = dynamic_cast<ui::Label*>(FindControl(_T("login_error")));

    if ((btnLogin == nullptr) || (userEdit == nullptr)) {
        return;
    }

    auto alive = m_alive;
    btnLogin->AttachClick([=](const ui::EventArgs&) {
        if (!*alive) {
            return true;
        }

        if (errorLabel != nullptr) {
            errorLabel->SetVisible(false);
        }

        std::string userId = TrimString(ui::StringConvert::TToUTF8(userEdit->GetText()));
        if (userId.empty()) {
            if (errorLabel != nullptr) {
                errorLabel->SetText(_T("登录失败"));
                errorLabel->SetVisible(true);
            }
            return true;
        }

        std::string userSig;
        if (!FindUserSig(userId, userSig)) {
            if (errorLabel != nullptr) {
                errorLabel->SetText(_T("登录失败"));
                errorLabel->SetVisible(true);
            }
            auto logger = spdlog::get("im_file_logger");
            if (logger) {
                logger->warn("登录失败，UserSig.csv 中未找到账号: {}", userId);
            }
            return true;
        }

        btnLogin->SetEnabled(false);
        btnLogin->SetText(_T("登录中..."));
        auto logger = spdlog::get("im_file_logger");
        if (logger) {
            logger->info("开始登录腾讯云 IM，账号: {}", userId);
        }

        const int result = TencentImService::Instance().Login(userId, userSig,
            [=](int32_t code, const std::string& desc) {
                if (!*alive) {
                    return;
                }

                auto log = spdlog::get("im_file_logger");
                if (code == 0) {
                    if (log) {
                        log->info("登录成功，账号: {}", userId);
                    }

                    ChatForm* chatForm = new ChatForm(_T("basic_layout"), _T(""), _T("wechat.xml"));
                    if (chatForm->CreateWnd(nullptr, ui::WindowCreateParam(_T("克铖多媒体"), true))) {
                        chatForm->PostQuitMsgWhenClosed(true);
                        chatForm->ShowWindow(ui::kSW_SHOW_NORMAL);

                        if (btnLogin != nullptr) {
                            btnLogin->SetEnabled(true);
                            btnLogin->SetText(_T("登录"));
                        }
                        PostQuitMsgWhenClosed(false);
                        CloseWnd();
                    }
                    else {
                        delete chatForm;
                        if (btnLogin != nullptr) {
                            btnLogin->SetEnabled(true);
                            btnLogin->SetText(_T("登录"));
                        }
                        if (errorLabel != nullptr) {
                            errorLabel->SetText(_T("登录失败"));
                            errorLabel->SetVisible(true);
                        }
                    }
                }
                else {
                    if (log) {
                        log->error("登录失败，账号: {}, code: {}, desc: {}", userId, code, desc);
                    }
                    if (btnLogin != nullptr) {
                        btnLogin->SetEnabled(true);
                        btnLogin->SetText(_T("登录"));
                    }
                    if (errorLabel != nullptr) {
                        errorLabel->SetText(_T("登录失败"));
                        errorLabel->SetVisible(true);
                    }
                }
            });

        if (result != TIM_SUCC) {
            if (logger) {
                logger->error("TIMLogin 调用失败，账号: {}, ret: {}", userId, result);
            }
            btnLogin->SetEnabled(true);
            btnLogin->SetText(_T("登录"));
            if (errorLabel != nullptr) {
                errorLabel->SetText(_T("登录失败"));
                errorLabel->SetVisible(true);
            }
        }
        return true;
    });
#endif
}

void LoginForm::ShowCustomWindow(const DString& class_name, const DString& theme_directory, const DString& layout_xml)
{
    LoginForm* window = new LoginForm(class_name, theme_directory, layout_xml);
    window->CreateWnd(nullptr, ui::WindowCreateParam(_T("chat"), true));
    window->PostQuitMsgWhenClosed(true);
    window->ShowWindow(ui::kSW_SHOW_NORMAL);
}
