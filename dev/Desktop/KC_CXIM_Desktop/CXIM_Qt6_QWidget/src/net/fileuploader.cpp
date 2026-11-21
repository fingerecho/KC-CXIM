#include "fileuploader.h"

#include <QFile>
#include <QMimeDatabase>
#include <QJsonDocument>
#include <QUrl>

FileUploader::FileUploader(HttpClient *http, QObject *parent)
    : QObject(parent)
    , m_http(http)
{
}

void FileUploader::upload(const QString &filePath, ProgressFn progress, DoneFn done)
{
    QFileInfo info(filePath);
    if (!info.exists() || !info.isFile()) {
        Result r; r.error = QStringLiteral("file not readable"); done(r); return;
    }

    QFile f(filePath);
    if (!f.open(QIODevice::ReadOnly)) {
        Result r; r.error = QStringLiteral("file open failed"); done(r); return;
    }
    // 说明: IM 场景文件大小受服务端 FILE_MAX_SIZE 闸门约束, 整体读入内存换取
    // 实现简单; 若将来放开到 GB 级, 只需把分片路径改为边读边发, 接口不变
    const QByteArray bytes = f.readAll();
    f.close();

    QMimeDatabase db;
    QString mime = db.mimeTypeForFile(info).name();

    // ---- 步骤 1: 控制面签发上传凭证(协议 4.15.1) ----
    QJsonObject body;
    body[QStringLiteral("name")] = info.fileName();
    body[QStringLiteral("mime")] = mime;
    body[QStringLiteral("size")] = static_cast<double>(bytes.size());

    m_http->post(QStringLiteral("/api/files/upload-token"), body,
                 [this, info, mime, bytes, progress, done](const ApiResult &res) {
        if (!res.ok) {
            Result r; r.error = res.message.isEmpty()
                        ? QStringLiteral("upload-token failed (http %1)").arg(res.httpStatus)
                        : res.message;
            done(r);
            return;
        }
        stepDirectUpload(res.objectData(), info, mime, bytes, progress, done);
    });
}

void FileUploader::stepDirectUpload(const QJsonObject &tok, const QFileInfo &info,
                                    const QString &mime, const QByteArray &bytes,
                                    ProgressFn progress, DoneFn done)
{
    const QString uploadUrl = tok.value(QStringLiteral("uploadUrl")).toString();

    // ---- mock 模式(QINIU_ENABLED=0): 直传端点就是服务器自己的 mock-put(协议 4.15.5) ----
    if (uploadUrl.contains(QStringLiteral("mock-put"))) {
        QJsonObject body;
        body[QStringLiteral("file_id")] = tok.value(QStringLiteral("fileId")).toString();
        body[QStringLiteral("ticket")]  = tok.value(QStringLiteral("ticket")).toString();
        m_http->post(QStringLiteral("/api/files/mock-put"), body,
                     [this, tok, name = info.fileName(), mime, done](const ApiResult &res) {
            if (!res.ok) {
                Result r; r.error = res.message.isEmpty() ? QStringLiteral("mock-put failed")
                                                          : res.message;
                done(r);
                return;
            }
            stepCommit(tok, name, mime, done);
        });
        return;
    }

    if (bytes.size() <= kFormThresholdBytes)
        uploadForm(tok, info.fileName(), mime, bytes, done);   // 小文件省一次往返
    else
        uploadResumableV2(tok, info.fileName(), mime, bytes, progress, done);
}

// ---------------------------------------------------------------------------
// 表单直传(协议 4.15.6-B): POST {uploadUrl}/{key}, multipart: token/key/file
// ---------------------------------------------------------------------------
void FileUploader::uploadForm(const QJsonObject &tok, const QString &name,
                              const QString &mime, const QByteArray &bytes, DoneFn done)
{
    const QString uptoken = tok.value(QStringLiteral("uptoken")).toString();
    const QString key     = tok.value(QStringLiteral("key")).toString();
    const QString url     = tok.value(QStringLiteral("uploadUrl")).toString()
                            + QStringLiteral("/") + QUrl::toPercentEncoding(key);

    static const QByteArray boundary = "----cxim.qtx.boundary";
    QByteArray body;
    body.reserve(bytes.size() + 512);
    auto addField = [&body](const char *field, const QString &value) {
        body += "--" + boundary + "\r\n";
        body += "Content-Disposition: form-data; name=\"" + QByteArray(field) + "\"\r\n\r\n";
        body += value.toUtf8() + "\r\n";
    };
    addField("token", uptoken);
    addField("key", key);
    // file 段: 带 filename 与该片 Content-Type(协议 4.15.6-B)
    body += "--" + boundary + "\r\n";
    body += "Content-Disposition: form-data; name=\"file\"; filename=\""
            + name.toUtf8() + "\"\r\n";
    body += "Content-Type: " + mime.toUtf8() + "\r\n\r\n";
    body += bytes;
    body += "\r\n--" + boundary + "--\r\n";

    HttpClient::RawRequest req;
    req.method = QStringLiteral("POST");
    req.url = url;
    req.body = body;
    req.contentType = "multipart/form-data; boundary=" + boundary;

    m_http->rawRequest(req, [this, tok, name, mime, done](int status, QByteArray resp) {
        if (status != 200) {
            // 直传失败定位口径见协议 4.15.7/4.15.9: 401 先怀疑凭证
            Result r;
            r.error = QStringLiteral("direct upload failed http=%1 body=%2")
                          .arg(status).arg(QString::fromUtf8(resp.left(200)));
            done(r);
            return;
        }
        stepCommit(tok, name, mime, done);
    });
}

// ---------------------------------------------------------------------------
// 分片上传 V2(协议 4.15.6-A): init → PUT part × N → complete
// ---------------------------------------------------------------------------
void FileUploader::uploadResumableV2(const QJsonObject &tok, const QString &name,
                                     const QString &mime, const QByteArray &bytes,
                                     ProgressFn progress, DoneFn done)
{
    const QString uptoken  = tok.value(QStringLiteral("uptoken")).toString();
    const QString key      = tok.value(QStringLiteral("key")).toString();
    const QString uploadUrl = tok.value(QStringLiteral("uploadUrl")).toString();

    // bucket 从 uptoken 第三段 policy 的 scope("bucket:key") 解出, 无需额外配置
    const QStringList tokParts = uptoken.split(QLatin1Char(':'));
    if (tokParts.size() < 3) {
        Result r; r.error = QStringLiteral("malformed uptoken"); done(r); return;
    }
    const QByteArray policy = QByteArray::fromBase64(
        tokParts.at(2).toUtf8(), QByteArray::Base64UrlEncoding);
    const QString scope = QJsonDocument::fromJson(policy).object()
                              .value(QStringLiteral("scope")).toString();
    const QString bucket = scope.section(QLatin1Char(':'), 0, 0);
    if (bucket.isEmpty()) {
        Result r; r.error = QStringLiteral("cannot extract bucket from uptoken"); done(r); return;
    }

    // entry = urlsafe base64(key), 保留 '=' 填充(Qt6 toBase64 默认保留,
    // 与服务端 entryBase64 同口径, 协议 4.15.6)
    const QString entry = QString::fromLatin1(
        key.toUtf8().toBase64(QByteArray::Base64UrlEncoding));
    const QString objectBase = QStringLiteral("%1/buckets/%2/objects/%3")
                                   .arg(uploadUrl, bucket, entry);

    QList<QPair<QByteArray, QByteArray>> authHeaders = {
        {QByteArray("Authorization"), QByteArray("UpToken ") + uptoken.toUtf8()},
    };

    // ---- init ----
    HttpClient::RawRequest initReq;
    initReq.method = QStringLiteral("POST");
    initReq.url = objectBase + QStringLiteral("/uploads");
    initReq.contentType = "application/octet-stream";
    initReq.extraHeaders = authHeaders;

    m_http->rawRequest(initReq,
        [this, objectBase, authHeaders, uptoken, tok, name, mime, bytes, progress, done]
        (int status, QByteArray resp) {
        if (status != 200) {
            Result r;
            r.error = QStringLiteral("resumable init failed http=%1 body=%2")
                          .arg(status).arg(QString::fromUtf8(resp.left(200)));
            done(r);
            return;
        }
        const QString uploadId = QJsonDocument::fromJson(resp).object()
                                     .value(QStringLiteral("uploadId")).toString();
        if (uploadId.isEmpty()) {
            Result r; r.error = QStringLiteral("resumable init: no uploadId"); done(r); return;
        }

        // ---- 逐片 PUT(顺序异步链), 片号从 1 起; 0 字节也要 1 片(协议 4.15.6) ----
        const int partCount = qMax(1, int((bytes.size() + kPartSize - 1) / kPartSize));
        struct State { int part = 1; qint64 sent = 0; QJsonArray parts; };
        auto st = std::make_shared<State>();

        // 递归 lambda: 用 shared_ptr 持有 std::function 自身, 避免拷贝捕获爆炸
        auto sendPart = std::make_shared<std::function<void()>>();
        *sendPart = [this, st, sendPart, objectBase, uploadId, authHeaders,
                     uptoken, tok, name, mime, bytes, partCount, progress, done]() {
            const int idx = st->part;                                    // 1-based
            const int offset = (idx - 1) * kPartSize;
            const QByteArray chunk = bytes.mid(offset, kPartSize);

            HttpClient::RawRequest pr;
            pr.method = QStringLiteral("PUT");
            pr.url = objectBase + QStringLiteral("/uploads/%1/%2").arg(uploadId).arg(idx);
            pr.body = chunk;
            pr.contentType = "application/octet-stream";
            pr.extraHeaders = authHeaders;

            const qint64 baseSent = st->sent;
            m_http->rawRequest(pr,
                [this, st, sendPart, uptoken, objectBase, uploadId, authHeaders,
                 tok, name, mime, chunk, idx, partCount, bytes, progress, done,
                 baseSent](int status, QByteArray resp) {
                if (status != 200) {
                    Result r;
                    r.error = QStringLiteral("part %1 failed http=%2 body=%3")
                                  .arg(idx).arg(status)
                                  .arg(QString::fromUtf8(resp.left(200)));
                    done(r);
                    return;
                }
                // 记录该片 etag, complete 时提交
                const QJsonObject p = QJsonDocument::fromJson(resp).object();
                QJsonObject partObj;
                partObj[QStringLiteral("partNumber")] = idx;
                partObj[QStringLiteral("etag")] = p.value(QStringLiteral("etag")).toString();
                st->parts.append(partObj);

                st->sent = baseSent + chunk.size();
                if (progress)
                    progress(st->sent, bytes.size());

                if (st->part < partCount) {
                    ++st->part;
                    (*sendPart)();          // 继续下一片
                    return;
                }

                // ---- complete(协议 4.15.6-A 第 3 步) ----
                QJsonObject cbody;
                cbody[QStringLiteral("parts")] = st->parts;
                cbody[QStringLiteral("mimeType")] = mime;
                cbody[QStringLiteral("fname")] = name;
                HttpClient::RawRequest cr;
                cr.method = QStringLiteral("POST");
                cr.url = objectBase + QStringLiteral("/uploads/%1").arg(uploadId);
                cr.body = QJsonDocument(cbody).toJson(QJsonDocument::Compact);
                cr.contentType = "application/json";
                cr.extraHeaders = authHeaders;

                // complete 成功后走 commit
                m_http->rawRequest(cr,
                    [this, tok, name, mime, done](int s2, QByteArray resp2) {
                    if (s2 != 200) {
                        Result r;
                        r.error = QStringLiteral("complete failed http=%1 body=%2")
                                      .arg(s2).arg(QString::fromUtf8(resp2.left(200)));
                        done(r);
                        return;
                    }
                    stepCommit(tok, name, mime, done);
                });
            }
            , [progress, baseSent, bytes](qint64 sent, qint64) {
                if (progress)
                    progress(baseSent + sent, bytes.size());
            });
        };
        (*sendPart)();
    });
}

// ---------------------------------------------------------------------------
// commit(协议 4.15.2): 服务端 stat 核验后登记, 返回权威 fileId/url/size/qetag
// ---------------------------------------------------------------------------
void FileUploader::stepCommit(const QJsonObject &tok, const QString &name,
                              const QString &mime, DoneFn done)
{
    QJsonObject body;
    body[QStringLiteral("file_id")] = tok.value(QStringLiteral("fileId")).toString();
    body[QStringLiteral("ticket")]  = tok.value(QStringLiteral("ticket")).toString();

    m_http->post(QStringLiteral("/api/files/commit"), body,
                 [name, mime, done](const ApiResult &res) {
        Result r;
        if (!res.ok) {
            r.error = res.message.isEmpty()
                          ? QStringLiteral("commit failed (http %1)").arg(res.httpStatus)
                          : res.message;
            done(r);
            return;
        }
        r.ok = true;
        r.meta = MediaMeta::fromJson(res.objectData());
        r.meta.name = name;         // commit 响应不含 name/mime, 本地补全用于展示
        r.meta.mime = mime;
        done(r);
    });
}
