#ifndef CXIM_ADDDIALOGS_H
#define CXIM_ADDDIALOGS_H

#include <QDialog>

class ImService;

/**
 * @file adddialogs.h
 * @brief 好友相关弹窗契约(实现见 adddialogs.cpp)
 *
 * AddFriendDialog(对照旧版交互 + 协议 4.4/4.7):
 *  - 顶部搜索框(占位"输入用户ID或用户名")+ 搜索按钮;
 *  - 结果列表每行: 头像 | ID + 用户名/昵称 | 操作按钮"添加";
 *  - 点击添加 → sendRequest(toUserId, "你好，我想加你为好友");
 *    成功后按钮变为不可点击的"已添加"; 服务端报"already friends"同样置灰;
 *  - 关闭后主界面好友列表无需立即刷新(申请未通过前不产生好友)。
 *
 * FriendRequestsDialog(协议 4.6/4.9):
 *  - 打开时 listRequests("incoming") 只显示 status==0;
 *  - 每行: 申请人头像/名/ID + 附言 + 通过/拒绝按钮;
 *  - 通过 → acceptRequest, 行内变为"已添加"(随后 TCP 132 事件会触发主界面全量刷新);
 *    拒绝 → rejectRequest, 行消失;
 *  - 空列表显示占位文本"暂无新的好友申请"。
 */
class AddFriendDialog : public QDialog
{
    Q_OBJECT
public:
    explicit AddFriendDialog(ImService *im, QWidget *parent = nullptr);

private:
    void doSearch();
    ImService *m_im;
    class QLineEdit *m_search = nullptr;
    class QVBoxLayout *m_resultLayout = nullptr;
};

class FriendRequestsDialog : public QDialog
{
    Q_OBJECT
public:
    explicit FriendRequestsDialog(ImService *im, QWidget *parent = nullptr);

private:
    void reload();
    ImService *m_im;
    class QVBoxLayout *m_listLayout = nullptr;
};

#endif // CXIM_ADDDIALOGS_H
