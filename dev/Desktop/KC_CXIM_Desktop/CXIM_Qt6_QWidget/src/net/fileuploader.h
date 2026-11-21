#ifndef CXIM_FILEUPLOADER_H
#define CXIM_FILEUPLOADER_H

#include <QObject>
#include <QFileInfo>
#include "httpclient.h"
#include "../core/imstypes.h"

/**
 * @file fileuploader.h
 * @brief 图片/文件七牛云直传(协议 4.15): 控制面走 IM 服务器, 数据面直连对象存储
 *
 * 完整链路(协议 4.15 时序):
 *   1. POST /api/files/upload-token  → {fileId, key, uploadUrl, uptoken, ticket}
 *   2. 直传字节到 uploadUrl:
 *        - mock 模式(uploadUrl 含 mock-put): POST {base}/api/files/mock-put
 *        - 小文件(< FORM_THRESHOLD): 表单直传 POST {uploadUrl}/{key} (multipart)
 *        - 大文件: 分片上传 V2 init → parts(4MB) → complete
 *   3. POST /api/files/commit {file_id, ticket} → {fileId, url, size, qetag}
 *   4. 上层用 fileId 组装 TCP 媒体消息(协议 5.8: content 只放 {"file_id":...})
 *
 * 依赖方向: 只依赖 HttpClient(net 层内部), 不认识 service/UI。
 */
class FileUploader : public QObject
{
    Q_OBJECT
public:
    struct Result {
        bool ok = false;
        QString error;
        MediaMeta meta;       // 成功时含 fileId/url/size/qetag(name/mime 本地补全)
    };
    using ProgressFn = std::function<void(qint64 sent, qint64 total)>;
    using DoneFn = std::function<void(const Result &)>;

    FileUploader(HttpClient *http, QObject *parent = nullptr);

    /// mime 为空时按扩展名自动推断(QMimeDatabase)
    void upload(const QString &filePath, ProgressFn progress, DoneFn done);

    /// 下载换取入口(302 → 短时效签名直链); 交给 QDesktopServices/QNetworkAccessManager 处理
    static QString downloadUrl(const QString &serverUrl) { return serverUrl; }

private:
    static const qint64 kFormThresholdBytes = 4 * 1024 * 1024; // 表单/分片切换阈值
    static const int kPartSize = 4 * 1024 * 1024;              // 分片 4MB(协议 4.15.6)

    void stepDirectUpload(const QJsonObject &tok, const QFileInfo &info,
                          const QString &mime, const QByteArray &fileBytes,
                          ProgressFn progress, DoneFn done);
    void stepCommit(const QJsonObject &tok, const QString &name, const QString &mime,
                    DoneFn done);
    void uploadForm(const QJsonObject &tok, const QString &name, const QString &mime,
                    const QByteArray &bytes, DoneFn done);
    void uploadResumableV2(const QJsonObject &tok, const QString &name, const QString &mime,
                           const QByteArray &bytes, ProgressFn progress, DoneFn done);

    HttpClient *m_http;
};

#endif // CXIM_FILEUPLOADER_H
