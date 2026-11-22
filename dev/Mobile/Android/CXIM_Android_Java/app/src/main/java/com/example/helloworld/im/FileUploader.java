package com.example.helloworld.im;

import android.content.Context;
import android.os.Handler;
import android.os.Looper;
import android.webkit.MimeTypeMap;

import com.example.helloworld.im.model.MediaMeta;
import com.google.gson.JsonArray;
import com.google.gson.JsonObject;
import com.google.gson.JsonParser;
import com.google.gson.JsonSyntaxException;

import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.nio.charset.StandardCharsets;
import java.util.Arrays;
import java.util.HashMap;
import java.util.HashSet;
import java.util.Locale;
import java.util.Map;
import java.util.Set;

import okhttp3.MediaType;
import okhttp3.MultipartBody;
import okhttp3.RequestBody;

/**
 * 文件直传（协议 §4.15）。
 *
 * <p>控制面四步，二进制不过 IM 服务器：
 * <ol>
 *   <li>POST /api/files/upload-token，拿 fileId / key / uploadUrl / uptoken / ticket</li>
 *   <li>直传：mock 端点，或表单直传（≤4MB），或七牛分片 V2（&gt;4MB）</li>
 *   <li>POST /api/files/commit，服务端 stat 核验后登记，返回权威 url/size/qetag</li>
 *   <li>发 TCP 媒体消息，content 只带 file_id，由服务端重建元数据</li>
 * </ol>
 */
public class FileUploader {

    public interface UploadListener {
        void onProgress(long sent, long total);

        void onDone(boolean ok, String error, MediaMeta meta);
    }

    /** 扩展名白名单，与服务端闸门一致；不在名单里直接本地失败，不浪费一次往返 */
    private static final Set<String> ALLOWED_EXT = new HashSet<>(Arrays.asList(
            "jpg", "jpeg", "png", "gif", "webp", "bmp",
            "pdf", "txt", "md", "doc", "docx", "xls", "xlsx", "ppt", "pptx",
            "zip", "rar", "7z",
            "mp3", "mp4", "wav"));

    private final Context context;
    private final HttpClient http;
    private final Handler main = new Handler(Looper.getMainLooper());

    public FileUploader(Context context, HttpClient http) {
        this.context = context.getApplicationContext();
        this.http = http;
    }

    // ===================================================================
    //  上传
    // ===================================================================

    /** 异步上传一个本地文件；listener 回调在主线程 */
    public void upload(File file, String mime, String displayName, UploadListener listener) {
        final String name = displayName == null || displayName.isEmpty() ? file.getName() : displayName;
        final String type = (mime == null || mime.isEmpty()) ? guessMime(name) : mime;

        http.enqueue(() -> {
            try {
                doUpload(file, type, name, listener);
            } catch (Exception e) {
                fail(listener, e.getMessage() == null ? e.getClass().getSimpleName() : e.getMessage());
            }
        });
    }

    private void doUpload(File file, String mime, String name, UploadListener listener) throws IOException {
        String ext = extensionOf(name);
        if (!ALLOWED_EXT.contains(ext)) {
            fail(listener, "不支持的文件类型: ." + ext);
            return;
        }
        long size = file.length();
        if (size == 0) {
            fail(listener, "文件为空");
            return;
        }
        // 单文件大小上限由服务端闸门决定（FILE_MAX_SIZE_MB，默认 20MB，
        // 且写进上传凭证的 fsizeLimit 由七牛强制执行）。
        // 本地不重复写死数字，否则服务端调大上限时客户端会自己把人拦掉。

        // ---- 步骤 1：签发上传凭证 ----
        JsonObject req = new JsonObject();
        req.addProperty("name", name);
        req.addProperty("mime", mime);
        req.addProperty("size", size);
        ApiResult tokenResp = http.callSync("POST", "/api/files/upload-token", null, req, true);
        if (!tokenResp.ok) {
            fail(listener, tokenResp.errorText("申请上传凭证失败"));
            return;
        }
        JsonObject d = tokenResp.obj();
        String fileId = Json.str(d, "fileId");
        String key = Json.str(d, "key");
        String uploadUrl = Json.str(d, "uploadUrl");
        String uptoken = Json.str(d, "uptoken");
        String ticket = Json.str(d, "ticket");
        if (fileId.isEmpty() || ticket.isEmpty()) {
            fail(listener, "上传凭证字段缺失");
            return;
        }

        // ---- 步骤 2：直传 ----
        // 只有表单直传（≤4MB）才把整个文件读进内存，分片路径按区间边读边发，
        // 否则接近上限的文件会在上传途中就把堆吃光
        boolean ok;
        String uploadError;
        if (uploadUrl.contains("mock-put")) {
            // QINIU_ENABLED=0 的模拟存储：服务端只登记元数据，不搬运字节
            JsonObject mp = new JsonObject();
            mp.addProperty("file_id", fileId);
            mp.addProperty("ticket", ticket);
            ApiResult r = http.callSync("POST", "/api/files/mock-put", null, mp, true);
            ok = r.ok;
            uploadError = r.errorText("mock-put failed");
        } else if (size <= Config.FORM_UPLOAD_THRESHOLD) {
            ApiResult r = formUpload(absolute(uploadUrl), key, uptoken, name, mime, readAll(file));
            ok = r.ok;
            uploadError = r.ok ? "" : "直传失败 http=" + r.httpStatus + " " + r.message;
        } else {
            UploadOutcome o = multipartV2(absolute(uploadUrl), key, uptoken, name, mime,
                    file, size, listener);
            ok = o.ok;
            uploadError = o.error;
        }
        if (!ok) {
            fail(listener, uploadError);
            return;
        }
        main.post(() -> listener.onProgress(size, size));

        // ---- 步骤 3：commit 登记 ----
        JsonObject c = new JsonObject();
        c.addProperty("file_id", fileId);
        c.addProperty("ticket", ticket);
        ApiResult commit = http.callSync("POST", "/api/files/commit", null, c, true);
        if (!commit.ok) {
            fail(listener, commit.errorText("commit failed"));
            return;
        }
        JsonObject cd = commit.obj();
        MediaMeta meta = MediaMeta.fromJson(cd);
        if (meta.fileId.isEmpty()) {
            meta.fileId = fileId;
        }
        // commit 响应不含 name/mime，本地补全（消息里最终由服务端按 files 行重建）
        meta.name = name;
        meta.mime = mime;
        main.post(() -> listener.onDone(true, "", meta));
    }

    /** 表单直传：token / key / file 三段，不带 Bearer（这条连接的鉴权就是 uptoken 表单字段） */
    private ApiResult formUpload(String uploadUrl, String key, String uptoken, String name,
                                 String mime, byte[] bytes) {
        MultipartBody body = new MultipartBody.Builder()
                .setType(MultipartBody.FORM)
                .addFormDataPart("token", uptoken)
                .addFormDataPart("key", key)
                .addFormDataPart("file", name, RequestBody.create(bytes, MediaType.parse(mime)))
                .build();
        // 协议 §4.15.6 B：表单直传是 POST {uploadUrl}/{key}，key 同时也在路径里，
        // 只把 key 放表单字段是打不到对象的（桌面端 fileuploader.cpp 也是这么拼的）
        return http.rawPostBody(uploadUrl + "/" + key, body, null, false);
    }

    /** 分片上传 V2：init → 逐片 PUT → complete（协议 §4.15.6 A） */
    private UploadOutcome multipartV2(String uploadUrl, String key, String uptoken, String name,
                                      String mime, File file, long fileSize,
                                      UploadListener listener) {
        String bucket = extractBucket(uptoken);
        if (bucket == null) {
            return UploadOutcome.fail("malformed uptoken");
        }
        String entry = base64UrlEncode(key.getBytes(StandardCharsets.UTF_8));
        String objectBase = uploadUrl + "/buckets/" + bucket + "/objects/" + entry;

        Map<String, String> auth = new HashMap<>();
        auth.put("Authorization", "UpToken " + uptoken);

        // init
        ApiResult init = http.rawPost(objectBase + "/uploads", "application/octet-stream",
                new byte[0], auth, false);
        if (!init.ok) {
            return UploadOutcome.fail("init failed http=" + init.httpStatus + " " + init.message);
        }
        String uploadId = Json.str(init.obj(), "uploadId");
        if (uploadId.isEmpty()) {
            return UploadOutcome.fail("init 未返回 uploadId");
        }

        // 逐片：partNumber 从 1 起，片大小合法区间 1MB~1GB，0 字节也要发 1 片
        int partSize = Config.PART_SIZE;
        int partCount = Math.max(1, (int) ((fileSize + partSize - 1) / partSize));
        JsonArray parts = new JsonArray();
        long uploaded = 0;
        // 逐片按需从文件读取，不把整个文件驻留内存
        try (java.io.RandomAccessFile raf = new java.io.RandomAccessFile(file, "r")) {
            for (int i = 0; i < partCount; i++) {
                long from = (long) i * partSize;
                int len = (int) Math.min(partSize, fileSize - from);
                if (len < 0) {
                    len = 0;
                }
                byte[] chunk = new byte[len];
                if (len > 0) {
                    // 用 seek + readFully(byte[],off,len)：Android 的 RandomAccessFile
                    // 没有带 long 位置的 readFully 重载
                    raf.seek(from);
                    raf.readFully(chunk, 0, len);
                }
                ApiResult put = http.rawPut(objectBase + "/uploads/" + uploadId + "/" + (i + 1),
                        "application/octet-stream", chunk, auth);
                if (!put.ok) {
                    return UploadOutcome.fail("part " + (i + 1) + " failed http=" + put.httpStatus
                            + " " + put.message);
                }
                String etag = Json.str(put.obj(), "etag");
                if (etag.isEmpty()) {
                    return UploadOutcome.fail("part " + (i + 1) + " 未返回 etag");
                }
                JsonObject p = new JsonObject();
                p.addProperty("partNumber", i + 1);
                p.addProperty("etag", etag);
                parts.add(p);
                uploaded += len;
                final long sent = uploaded;
                final long total = fileSize;
                main.post(() -> listener.onProgress(Math.min(sent, total), total));
            }
        } catch (IOException e) {
            return UploadOutcome.fail("读取文件失败: " + e.getMessage());
        }

        // complete
        JsonObject complete = new JsonObject();
        complete.add("parts", parts);
        complete.addProperty("mimeType", mime);
        complete.addProperty("fname", name);
        ApiResult done = http.rawPost(objectBase + "/uploads/" + uploadId, "application/json",
                complete.toString().getBytes(StandardCharsets.UTF_8), auth, false);
        if (!done.ok) {
            return UploadOutcome.fail("complete failed http=" + done.httpStatus + " " + done.message);
        }
        return UploadOutcome.ok();
    }

    private void fail(UploadListener listener, String error) {
        main.post(() -> listener.onDone(false, error, null));
    }

    /** 从 uptoken 第三段（urlsafe base64 的 policy）解出 bucket，客户端无需任何七牛配置 */
    static String extractBucket(String uptoken) {
        if (uptoken == null) {
            return null;
        }
        String[] seg = uptoken.split(":");
        if (seg.length < 3) {
            return null;
        }
        try {
            String policy = new String(android.util.Base64.decode(seg[2], android.util.Base64.URL_SAFE),
                    StandardCharsets.UTF_8);
            JsonObject o = JsonParser.parseString(policy).getAsJsonObject();
            String scope = Json.str(o, "scope");
            int idx = scope.indexOf(':');
            return idx > 0 ? scope.substring(0, idx) : null;
        } catch (JsonSyntaxException | IllegalArgumentException e) {
            return null;
        }
    }

    static String base64UrlEncode(byte[] data) {
        return android.util.Base64.encodeToString(data,
                android.util.Base64.URL_SAFE | android.util.Base64.NO_WRAP);
    }

    // ===================================================================
    //  下载
    // ===================================================================

    /**
     * 下载媒体。消息里的 url 是服务器换取入口（GET /api/files/{uuid}），
     * 带 Bearer 请求后服务端 302 到短时效签名直链，OkHttp 自动跟随。
     */
    public byte[] download(String url) throws IOException {
        return http.getBytes(absolute(url), true);
    }

    /** 下载到目标文件，返回是否成功 */
    public boolean downloadTo(String url, File dest) {
        try {
            byte[] bytes = download(url);
            File parent = dest.getParentFile();
            if (parent != null && !parent.exists() && !parent.mkdirs()) {
                return false;
            }
            try (FileOutputStream fos = new FileOutputStream(dest)) {
                fos.write(bytes);
            }
            return true;
        } catch (IOException e) {
            return false;
        }
    }

    // ===================================================================
    //  工具
    // ===================================================================

    /** uploadUrl 在 mock 模式下是相对路径，需要补上 base */
    private String absolute(String url) {
        if (url == null) {
            return "";
        }
        if (url.startsWith("http://") || url.startsWith("https://")) {
            return url;
        }
        String base = Config.httpBaseUrl(context);
        return url.startsWith("/") ? base + url : base + "/" + url;
    }

    private static byte[] readAll(File f) throws IOException {
        byte[] out = new byte[(int) f.length()];
        try (FileInputStream fis = new FileInputStream(f)) {
            int got = 0;
            while (got < out.length) {
                int r = fis.read(out, got, out.length - got);
                if (r < 0) {
                    break;
                }
                got += r;
            }
        }
        return out;
    }

    /** 按扩展名猜 MIME，猜不出就给二进制流 */
    public static String guessMime(String fileName) {
        String ext = extensionOf(fileName);
        if (ext.isEmpty()) {
            return "application/octet-stream";
        }
        String mime = MimeTypeMap.getSingleton().getMimeTypeFromExtension(ext);
        if (mime != null && !mime.isEmpty()) {
            return mime;
        }
        switch (ext) {
            case "md":
                return "text/markdown";
            case "7z":
                return "application/x-7z-compressed";
            case "rar":
                return "application/x-rar-compressed";
            case "txt":
                return "text/plain";
            default:
                return "application/octet-stream";
        }
    }

    public static String extensionOf(String fileName) {
        if (fileName == null) {
            return "";
        }
        int i = fileName.lastIndexOf('.');
        if (i < 0 || i == fileName.length() - 1) {
            return "";
        }
        return fileName.substring(i + 1).toLowerCase(Locale.US);
    }

    public static boolean isImage(String fileNameOrMime) {
        if (fileNameOrMime == null) {
            return false;
        }
        String s = fileNameOrMime.toLowerCase(Locale.US);
        if (s.startsWith("image/")) {
            return true;
        }
        String ext = extensionOf(s);
        return ext.equals("jpg") || ext.equals("jpeg") || ext.equals("png")
                || ext.equals("gif") || ext.equals("webp") || ext.equals("bmp");
    }

    /** 文件大小人性化显示（B/KB/MB/GB，保留 1 位小数） */
    public static String humanSize(long bytes) {
        if (bytes < 1024) {
            return bytes + "B";
        }
        String[] units = {"KB", "MB", "GB", "TB"};
        double v = bytes;
        int i = -1;
        while (v >= 1024 && i < units.length - 1) {
            v /= 1024;
            i++;
        }
        return String.format(Locale.US, "%.1f%s", v, units[i]);
    }

    private static class UploadOutcome {
        final boolean ok;
        final String error;

        UploadOutcome(boolean ok, String error) {
            this.ok = ok;
            this.error = error;
        }

        static UploadOutcome ok() {
            return new UploadOutcome(true, "");
        }

        static UploadOutcome fail(String e) {
            return new UploadOutcome(false, e);
        }
    }
}
