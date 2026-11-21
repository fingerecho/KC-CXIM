#include "httpclient.h"

#include <QJsonDocument>
#include <QDebug>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QTimer>

namespace {
// 公开接口(协议 2.4): 不附加 Bearer
const QStringList kPublicApis = {
    QStringLiteral("/api/register"),
    QStringLiteral("/api/login"),
    QStringLiteral("/api/token/refresh"),
};
} // namespace

HttpClient::HttpClient(Session *session, QObject *parent)
    : QObject(parent)
    , m_session(session)
    , m_nam(new QNetworkAccessManager(this))
{
}

bool HttpClient::isPublicApi(const QString &path)
{
    for (const QString &p : kPublicApis) {
        if (path == p)
            return true;
    }
    return false;
}

void HttpClient::get(const QString &path, const QUrlQuery &query,
                     std::function<void(const ApiResult &)> cb)
{
    send(QNetworkAccessManager::GetOperation, path, query, QJsonObject(), std::move(cb), 0);
}

void HttpClient::post(const QString &path, const QJsonObject &body,
                      std::function<void(const ApiResult &)> cb)
{
    send(QNetworkAccessManager::PostOperation, path, QUrlQuery(), body, std::move(cb), 0);
}

void HttpClient::patch(const QString &path, const QJsonObject &body,
                       std::function<void(const ApiResult &)> cb)
{
    send(QNetworkAccessManager::CustomOperation, path, QUrlQuery(), body, std::move(cb), 0);
}

void HttpClient::del(const QString &path, std::function<void(const ApiResult &)> cb)
{
    send(QNetworkAccessManager::DeleteOperation, path, QUrlQuery(), QJsonObject(),
         std::move(cb), 0);
}

void HttpClient::send(QNetworkAccessManager::Operation op, const QString &path,
                      const QUrlQuery &query, const QJsonObject &body,
                      std::function<void(const ApiResult &)> cb, int attempt)
{
    QUrl url(m_baseUrl + path);
    if (!query.isEmpty())
        url.setQuery(query);

    QNetworkRequest req(url);
    req.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
    req.setRawHeader("User-Agent", "CXIM-Qt/1.0");
    req.setTransferTimeout(15000);
    // 协议 2.4: 除公开接口外一律 Bearer
    if (!isPublicApi(path) && !m_session->accessToken().isEmpty())
        req.setRawHeader("Authorization",
                         QByteArray("Bearer ") + m_session->accessToken().toUtf8());

    QNetworkReply *reply = nullptr;
    const QByteArray payload = QJsonDocument(body).toJson(QJsonDocument::Compact);
    switch (op) {
    case QNetworkAccessManager::GetOperation:
        reply = m_nam->get(req);
        break;
    case QNetworkAccessManager::DeleteOperation:
        reply = m_nam->deleteResource(req);
        break;
    case QNetworkAccessManager::PostOperation:
        reply = m_nam->post(req, payload);
        break;
    case QNetworkAccessManager::CustomOperation: {   // PATCH
        req.setAttribute(QNetworkRequest::CustomVerbAttribute, QStringLiteral("PATCH"));
        reply = m_nam->sendCustomRequest(req, "PATCH", payload);
        break;
    }
    default:
        reply = m_nam->sendCustomRequest(req, "GET", QByteArray());
        break;
    }

    connect(reply, &QNetworkReply::finished, this,
            [this, reply, path, op, query, body, cb, attempt]() {
#ifdef CXIM_HTTP_TRACE
                qInfo("[http-trace] finished %s attempt=%d err=%d",
                      path.toUtf8().constData(), attempt, int(reply->error()));
#endif
                handleReply(reply, path, op, query, body, cb, attempt);
            });
}

void HttpClient::handleReply(QNetworkReply *reply, const QString &path,
                             QNetworkAccessManager::Operation op, const QUrlQuery &query,
                             const QJsonObject &body,
                             std::function<void(const ApiResult &)> cb, int attempt)
{
    reply->deleteLater();
    const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    const QByteArray raw = reply->readAll();

    // ---- 网络层错误(没拿到 HTTP 状态): 参考旧客户端, 仅此种情况自动重试 ----
    if (status == 0 && reply->error() != QNetworkReply::NoError) {
        if (attempt < 3) {
            QTimer::singleShot(200 + attempt * 100, this,
                               [this, op, path, query, body, cb, attempt]() {
                                   send(op, path, query, body, cb, attempt + 1);
                               });
            return;
        }
        ApiResult r;
        r.httpStatus = 0;
        r.message = reply->errorString();
        cb(r);
        return;
    }

    // ---- 统一响应解析(协议 2.2): {code, message, data} ----
    ApiResult r;
    r.httpStatus = status;
    const QJsonObject obj = QJsonDocument::fromJson(raw).object();
    r.code = obj.value(QStringLiteral("code")).toInt(-1);
    r.message = obj.value(QStringLiteral("message")).toString();
    if (r.message.isEmpty())
        r.message = obj.value(QStringLiteral("error")).toString();   // 兼容注册失败旧口径
    if (obj.contains(QStringLiteral("data")))
        r.data = obj.value(QStringLiteral("data"));
    r.ok = (status == 200 && r.code == 0);

    // ---- 401: 先静默刷新一次再重放(协议 3.3); 刷新失败才上抛 unauthorized ----
    if (status == 401 && !isPublicApi(path) && m_refreshHook
        && m_session->hasRefreshToken() && attempt == 0) {
        m_refreshHook([this, op, path, query, body, cb](bool refreshed) {
            if (refreshed) {
                send(op, path, query, body, cb, 1);
            } else {
                emit unauthorized();
                ApiResult fail;
                fail.httpStatus = 401;
                fail.message = QStringLiteral("invalid or expired token");
                cb(fail);
            }
        });
        return;
    }

    // ---- 429/503: 过载不是业务失败, 按 Retry-After 退避后重试(协议 2.6) ----
    if ((status == 429 || status == 503) && attempt < m_maxRetry) {
        const int retryAfter = reply->rawHeader("Retry-After").toInt();
        const int waitMs = qBound(200, (retryAfter > 0 ? retryAfter : 1) * 1000, 5000);
        QTimer::singleShot(waitMs, this, [this, op, path, query, body, cb, attempt]() {
            send(op, path, query, body, cb, attempt + 1);
        });
        return;
    }

    cb(r);
}

void HttpClient::rawRequest(const RawRequest &req,
                            std::function<void(int, QByteArray)> cb,
                            std::function<void(qint64, qint64)> progress)
{
    QNetworkRequest nr(QUrl(req.url));
    nr.setTransferTimeout(120000);   // 直传大文件放宽超时
    if (!req.contentType.isEmpty())
        nr.setHeader(QNetworkRequest::ContentTypeHeader, QString::fromUtf8(req.contentType));
    for (const auto &h : req.extraHeaders)
        nr.setRawHeader(h.first, h.second);

    QNetworkReply *reply = m_nam->sendCustomRequest(nr, req.method.toUtf8(), req.body);
    if (progress) {
        connect(reply, &QNetworkReply::uploadProgress, this,
                [progress](qint64 sent, qint64 total) { progress(sent, total); });
    }
    connect(reply, &QNetworkReply::finished, this, [reply, cb]() {
        reply->deleteLater();
        const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        cb(status, reply->readAll());
    });
}
