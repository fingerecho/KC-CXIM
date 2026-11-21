#ifndef CXIM_HTTPCLIENT_H
#define CXIM_HTTPCLIENT_H

#include <QObject>
#include <QNetworkAccessManager>
#include <QJsonObject>
#include <QUrlQuery>
#include <functional>

#include "../core/session.h"

/**
 * @file httpclient.h
 * @brief HTTP 控制面客户端(协议第 2/4 章): 统一响应解析 + Bearer 鉴权 + 退避重试
 *
 * 设计要点(UI 与功能分离的网络侧基础):
 *  - 所有接口以回调返回 ApiResult, 调用方只关心 code/message/data, 不接触 QNetworkReply;
 *  - 自动附加 Authorization: Bearer(公开接口除外);
 *  - 401 时通过注入的 refresh 钩子静默刷新一次并重放原请求(协议 3.3 推荐流程);
 *  - 429/503 按 Retry-After 退避重试, 不把过载当业务失败(协议 2.6);
 *  - 刷新钩子由 service 层注入, net 层不依赖任何业务类, 保持单向依赖。
 */
struct ApiResult
{
    bool ok = false;          // HTTP 成功且 code==0
    int httpStatus = 0;       // 0 表示网络层错误
    int code = -1;            // 业务码: 0 成功 1 失败
    QString message;          // 业务失败原因(服务端英文口径)
    QJsonValue data;          // 成功时的 data 字段

    QJsonObject objectData() const { return data.toObject(); }
};

class HttpClient : public QObject
{
    Q_OBJECT
public:
    explicit HttpClient(Session *session, QObject *parent = nullptr);

    void setBaseUrl(const QString &url) { m_baseUrl = url; }

    /// 注入 token 刷新执行器: 参数为完成回调(bool 成功), 由 AccountService 提供
    using RefreshFn = std::function<void(std::function<void(bool)>)>;
    void setRefreshHook(RefreshFn fn) { m_refreshHook = std::move(fn); }

    // ---- REST 动词封装(path 形如 "/api/friends", query 可选) ----
    void get(const QString &path, const QUrlQuery &query,
             std::function<void(const ApiResult &)> cb);
    void post(const QString &path, const QJsonObject &body,
              std::function<void(const ApiResult &)> cb);
    void patch(const QString &path, const QJsonObject &body,
               std::function<void(const ApiResult &)> cb);
    void del(const QString &path, std::function<void(const ApiResult &)> cb);

    /// 上传字节到外部端点(七牛直传数据面用, 不带 Bearer, 支持自定义头/ multipart)
    struct RawRequest {
        QString method = QStringLiteral("POST");
        QString url;                       // 绝对 URL
        QByteArray body;
        QByteArray contentType;            // multipart 时形如 "multipart/form-data; boundary=..."
        QList<QPair<QByteArray, QByteArray>> extraHeaders;
    };
    void rawRequest(const RawRequest &req, std::function<void(int status, QByteArray body)> cb,
                    std::function<void(qint64 sent, qint64 total)> progress = nullptr);

signals:
    /// token 彻底失效且刷新失败(401 兜底), UI 层监听后回登录页
    void unauthorized();

private:
    void send(QNetworkAccessManager::Operation op, const QString &path,
              const QUrlQuery &query, const QJsonObject &body,
              std::function<void(const ApiResult &)> cb, int attempt);
    void handleReply(QNetworkReply *reply, const QString &path,
                     QNetworkAccessManager::Operation op, const QUrlQuery &query,
                     const QJsonObject &body, std::function<void(const ApiResult &)> cb,
                     int attempt);

    static bool isPublicApi(const QString &path);

    Session *m_session;
    QNetworkAccessManager *m_nam;
    QString m_baseUrl;
    RefreshFn m_refreshHook;
    int m_maxRetry = 2;        // 429/503 自动重试次数
};

#endif // CXIM_HTTPCLIENT_H
