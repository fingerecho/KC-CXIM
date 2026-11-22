package com.example.helloworld.im.model;

import com.example.helloworld.im.Json;
import com.google.gson.JsonObject;

/**
 * 媒体元数据（协议 §5.8）。
 *
 * <p>上行只带 file_id；服务端做权属校验后用 files 行整体重建 content，
 * 所以下行一定带 url/name/size/mime/qetag，且 url 恒为服务器换取入口而非签名直链
 * ——签名直链带 deadline，写进消息会随过期而坏掉历史消息。
 */
public class MediaMeta {
    public String fileId = "";
    public String url = "";
    public String name = "";
    public long size;
    public String mime = "";
    public String qetag = "";

    public static MediaMeta fromJson(JsonObject o) {
        MediaMeta m = new MediaMeta();
        // commit 响应里是 fileId，消息元数据里是 file_id
        m.fileId = Json.str(o, "fileId");
        if (m.fileId.isEmpty()) {
            m.fileId = Json.str(o, "file_id");
        }
        m.url = Json.str(o, "url");
        m.name = Json.str(o, "name");
        m.size = Json.num(o, "size");
        m.mime = Json.str(o, "mime");
        m.qetag = Json.str(o, "qetag");
        return m;
    }

    /** 发送媒体消息时 content 只放这一个字段 */
    public static String uploadContent(String fileId) {
        JsonObject o = new JsonObject();
        o.addProperty("file_id", fileId);
        return o.toString();
    }

    public boolean isImage() {
        return mime.startsWith("image/");
    }

    public JsonObject toJson() {
        JsonObject o = new JsonObject();
        o.addProperty("file_id", fileId);
        if (!url.isEmpty()) {
            o.addProperty("url", url);
        }
        o.addProperty("name", name);
        o.addProperty("size", size);
        if (!mime.isEmpty()) {
            o.addProperty("mime", mime);
        }
        if (!qetag.isEmpty()) {
            o.addProperty("qetag", qetag);
        }
        return o;
    }
}
