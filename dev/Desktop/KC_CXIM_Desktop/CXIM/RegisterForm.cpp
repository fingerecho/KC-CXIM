#include "RegisterForm.h"
#include "HttpClient.h"

#include <algorithm>
#include <cctype>
#include <string>

#include <spdlog/spdlog.h>

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

std::string GetEditText(ui::RichEdit2* edit)
{
    if (edit == nullptr) {
        return std::string();
    }
    return ui::StringConvert::TToUTF8(edit->GetText());
}

std::string GetTrimmedEditText(ui::RichEdit2* edit)
{
    return TrimString(GetEditText(edit));
}

std::string ExtractJsonString(const std::string& json, const std::string& key)
{
    const std::string keyToken = "\"" + key + "\"";
    const size_t keyPos = json.find(keyToken);
    if (keyPos == std::string::npos) {
        return std::string();
    }
    const size_t colonPos = json.find(':', keyPos + keyToken.size());
    if (colonPos == std::string::npos) {
        return std::string();
    }
    const size_t quotePos = json.find('"', colonPos + 1);
    if (quotePos == std::string::npos) {
        return std::string();
    }

    std::string result;
    for (size_t i = quotePos + 1; i < json.size(); ++i) {
        const char ch = json[i];
        if (ch == '\\' && (i + 1) < json.size()) {
            const char next = json[++i];
            switch (next) {
            case 'n':
                result += '\n';
                break;
            case 'r':
                result += '\r';
                break;
            case 't':
                result += '\t';
                break;
            default:
                result += next;
                break;
            }
            continue;
        }
        if (ch == '"') {
            break;
        }
        result += ch;
    }
    return result;
}

} // namespace

RegisterForm::RegisterForm(const DString& class_name, const DString& theme_directory, const DString& layout_xml,
                           ui::Window* parent_window) :
    m_alive(std::make_shared<bool>(true)),
    m_themeDirectory(theme_directory),
    m_layoutXml(layout_xml),
    m_parentWindow(parent_window)
{
}

RegisterForm::~RegisterForm()
{
    if (m_alive) {
        *m_alive = false;
    }
}

DString RegisterForm::GetSkinFolder()
{
    return m_themeDirectory;
}

DString RegisterForm::GetSkinFile()
{
    return m_layoutXml;
}

void RegisterForm::OnInitWindow()
{
    ui::RichEdit2* userEdit = dynamic_cast<ui::RichEdit2*>(FindControl(_T("username")));
    ui::RichEdit2* passwordEdit = dynamic_cast<ui::RichEdit2*>(FindControl(_T("password")));
    ui::RichEdit2* confirmEdit = dynamic_cast<ui::RichEdit2*>(FindControl(_T("confirm_password")));
    ui::Label* errorLabel = dynamic_cast<ui::Label*>(FindControl(_T("register_error")));
    ui::Button* btnCancel = dynamic_cast<ui::Button*>(FindControl(_T("btn_cancel")));
    ui::Button* btnComplete = dynamic_cast<ui::Button*>(FindControl(_T("btn_complete")));

    auto alive = m_alive;

    if ((passwordEdit != nullptr) && (confirmEdit != nullptr) && (errorLabel != nullptr)) {
        const auto refreshPasswordHint = [passwordEdit, confirmEdit, errorLabel]() {
            const std::string password = ui::StringConvert::TToUTF8(passwordEdit->GetText());
            const std::string confirm = ui::StringConvert::TToUTF8(confirmEdit->GetText());
            if (!confirm.empty() && (password != confirm)) {
                errorLabel->SetText(_T("两次密码输入不匹配"));
                errorLabel->SetVisible(true);
            }
            else {
                errorLabel->SetVisible(false);
            }
        };

        passwordEdit->AttachTextChanged([refreshPasswordHint](const ui::EventArgs&) {
            refreshPasswordHint();
            return true;
        });
        confirmEdit->AttachTextChanged([refreshPasswordHint](const ui::EventArgs&) {
            refreshPasswordHint();
            return true;
        });
    }

    if (btnCancel != nullptr) {
        btnCancel->AttachClick([this, alive](const ui::EventArgs&) {
            if (!*alive) {
                return true;
            }
            m_exiting = true;
            CloseWnd();
            return true;
        });
    }

    if (btnComplete != nullptr) {
        btnComplete->AttachClick([=](const ui::EventArgs&) {
            if (!*alive) {
                return true;
            }

            const std::string username = GetTrimmedEditText(userEdit);
            const std::string password = GetEditText(passwordEdit);
            const std::string confirm = GetEditText(confirmEdit);

            const auto showError = [errorLabel](const DString& text) {
                if (errorLabel != nullptr) {
                    errorLabel->SetText(text);
                    errorLabel->SetVisible(true);
                }
            };

            if (username.empty()) {
                showError(_T("请输入用户名"));
                return true;
            }
            if (password.empty()) {
                showError(_T("请输入密码"));
                return true;
            }
            if (password != confirm) {
                showError(_T("两次密码输入不匹配"));
                return true;
            }

            btnComplete->SetEnabled(false);
            btnComplete->SetText(_T("注册中..."));
            if (errorLabel != nullptr) {
                errorLabel->SetVisible(false);
            }

            auto logger = spdlog::get("im_file_logger");
            if (logger) {
                logger->info("开始注册账号: {}", username);
            }

            int statusCode = 0;
            std::string responseBody;
            const bool requestOk = HttpClient::Instance().Register(username, password, statusCode, responseBody);

            if (!*alive) {
                return true;
            }

            btnComplete->SetEnabled(true);
            btnComplete->SetText(_T("完成"));

            if (requestOk && (statusCode == 200)) {
                if (logger) {
                    logger->info("注册成功，账号: {}", username);
                }
                CloseWnd();
            }
            else {
                const std::string errorText = ExtractJsonString(responseBody, "error");
                if (logger) {
                    logger->warn("注册失败，账号: {}, status: {}, body: {}", username, statusCode, responseBody);
                }
                if (errorText.empty()) {
                    showError(_T("注册失败"));
                }
                else {
                    showError(_T("注册失败: ") + ui::StringConvert::UTF8ToT(errorText));
                }
            }
            return true;
        });
    }
}

void RegisterForm::OnCloseWindow()
{
    ui::Window* parent = m_parentWindow;
    if (m_exiting) {
        if (parent != nullptr) {
            parent->PostQuitMsgWhenClosed(true);
            parent->CloseWnd();
        }
        else {
            ui::WindowBase::PostQuitMsg(0);
        }
    }
    else if (parent != nullptr) {
        parent->ShowWindow(ui::kSW_SHOW_NORMAL);
    }
    ui::WindowImplBase::OnCloseWindow();
}

void RegisterForm::ShowCustomWindow(const DString& class_name, const DString& theme_directory,
                                    const DString& layout_xml, ui::Window* parent)
{
    RegisterForm* window = new RegisterForm(class_name, theme_directory, layout_xml, parent);
    // 注册窗口作为独立的顶层窗口创建，避免父窗口隐藏后任务栏图标消失、窗口沉底
    if (!window->CreateWnd(nullptr, ui::WindowCreateParam(_T("注册账号"), true))) {
        delete window;
        return;
    }
    window->ShowWindow(ui::kSW_SHOW_NORMAL);
    if (parent != nullptr) {
        parent->ShowWindow(ui::kSW_HIDE);
    }
}
