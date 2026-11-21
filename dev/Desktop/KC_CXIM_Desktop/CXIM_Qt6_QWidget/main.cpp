#include <QApplication>
#include <QPointer>

#include "src/service/imservice.h"
#include "src/ui/common/theme.h"
#include "src/ui/login/loginwindow.h"
#include "src/ui/main/mainwindow.h"

/**
 * @brief 应用入口: 组装根(ImService)生命周期 = 应用生命周期
 *
 * 登录页 ↔ 主窗口的切换只在这一个"控制器"里做, 两个窗口互相不认识:
 *  - LoginWindow::loginSucceeded → 创建/显示 MainWindow, 收起登录页;
 *  - MainWindow 关闭(点✕)或 sessionExpired(logoutRequested) → 断 TCP、清
 *    session, 回到登录页。QPointer 保证"先关窗再回登录"时不会踩到已析构对象。
 */
int main(int argc, char *argv[])
{
    QApplication app(argc, argv);
    QApplication::setApplicationName(QStringLiteral("CXIM_Qt6_QWidget"));
    QApplication::setOrganizationName(QStringLiteral("KC-CXIM"));
    app.setStyleSheet(Theme::appStyleSheet());

    ImService im;                       // 组装根: core+net+service 全部就绪

    LoginWindow login(&im);
    QPointer<MainWindow> mainWin;

    auto backToLogin = [&]() {
        im.shutdownDataPlane();
        im.account()->logout();
        if (mainWin)
            mainWin->close();           // WA_DeleteOnClose 下真正销毁
        login.show();
    };

    QObject::connect(&login, &LoginWindow::loginSucceeded, &app, [&]() {
        if (!mainWin) {
            mainWin = new MainWindow(&im);
            mainWin->setAttribute(Qt::WA_DeleteOnClose);
            // ✕ 直接关窗 与 401 过期(logoutRequested)统一回登录页
            QObject::connect(mainWin, &MainWindow::logoutRequested, &app, backToLogin);
            QObject::connect(mainWin, &QObject::destroyed, &app, [&]() {
                if (im.session()->isLoggedIn())
                    backToLogin();
            });
        }
        mainWin->show();
        login.close();
    });

    login.show();
    return QApplication::exec();
}
