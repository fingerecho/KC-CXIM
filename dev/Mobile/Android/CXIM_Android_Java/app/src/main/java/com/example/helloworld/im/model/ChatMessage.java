package com.example.helloworld.im.model;

import com.example.helloworld.im.Envelope;
import com.example.helloworld.im.Json;
import com.example.helloworld.im.MsgType;
import com.google.gson.JsonObject;
import com.google.gson.JsonParser;
import com.google.gson.JsonSyntaxException;

/**
 * 一条聊天消息 = 协议信封 + 本地状态。
 * 本地状态（outgoing / status / errorText）不上线，只服务于气泡渲染与失败重发。
 */
public class ChatMessage {

    /** 消息在客户端的状态机（对齐桌面端 MessageStatus） */
    public enum Status {
        /** 已入本地队列，未收到 SendAck */
        SENDING,
        /** SendAck content == "sent" */
        SENT,
        /** SendAck content == "duplicate"，去重命中，等价成功 */
        DUPLICATE,
        /** 失败，可用同一 clientMsgId 重发 */
        FAILED
    }

    public long msgId;
    /** 客户端生成的去重键；重试必须保持不变 */
    public long clientMsgId;
    public long conversationId;
    public long seq;
    public long ts;
    public long fromUid;
    public long toUid;
    public int type = MsgType.TEXT;
    public String content = "";
    public boolean isGroup;

    public Status status = Status.SENDING;
    public boolean outgoing;
    public String errorText = "";

    /** 群聊气泡上方的发送者名，由 UI 侧从通讯录解析后填充 */
    public String senderName = "";

    private MediaMeta cachedMeta;

    public static ChatMessage from(Envelope e, long selfId) {
        ChatMessage m = new ChatMessage();
        m.msgId = e.msgId;
        m.conversationId = e.conversationId;
        m.seq = e.seq;
        m.ts = e.ts;
        m.fromUid = e.from;
        m.toUid = e.to;
        m.type = e.type;
        m.content = e.content;
        m.isGroup = e.isGroup;
        m.outgoing = e.from == selfId;
        m.clientMsgId = m.outgoing ? e.msgId : 0;
        // 历史与在线下行都视为已送达
        m.status = Status.SENT;
        return m;
    }

    /** 会话列表摘要文本：图片/文件带类型前缀 */
    public String preview() {
        if (type == MsgType.IMAGE || type == MsgType.FILE) {
            MediaMeta mm = meta();
            String prefix = type == MsgType.IMAGE ? "[图片] " : "[文件] ";
            if (mm != null && !mm.name.isEmpty()) {
                return prefix + mm.name;
            }
            return type == MsgType.IMAGE ? "[图片]" : "[文件]";
        }
        return content == null ? "" : content;
    }

    /**
     * 媒体元数据。content 在 type=2/4 时是一段 JSON 字符串，需要二次解析
     * （协议 §6.3 硬性边界）；自己刚发出时只有 file_id，没有 url。
     */
    public MediaMeta meta() {
        if (type != MsgType.IMAGE && type != MsgType.FILE) {
            return null;
        }
        if (cachedMeta != null) {
            return cachedMeta;
        }
        try {
            JsonObject o = JsonParser.parseString(content == null ? "{}" : content).getAsJsonObject();
            cachedMeta = MediaMeta.fromJson(o);
        } catch (JsonSyntaxException | IllegalStateException | ClassCastException e) {
            cachedMeta = null;
        }
        return cachedMeta;
    }

    /** 上传完成后本地先把元数据补进去，避免气泡一直显示“加载中” */
    public void attachLocalMeta(MediaMeta meta) {
        if (meta == null) {
            return;
        }
        this.content = meta.toJson().toString();
        this.cachedMeta = meta;
    }

    public long peerId(long selfId) {
        if (isGroup) {
            return toUid != 0 ? toUid : fromUid;
        }
        return fromUid == selfId ? toUid : fromUid;
    }

    public String convKey(long selfId) {
        return Conversation.keyOf(peerId(selfId), isGroup);
    }

    public boolean isMedia() {
        return type == MsgType.IMAGE || type == MsgType.FILE;
    }

    /** 状态文案，气泡右侧小字 */
    public String statusText() {
        switch (status) {
            case SENDING:
                return "发送中";
            case SENT:
                return "已发送";
            case DUPLICATE:
                return "已发送";
            case FAILED:
            default:
                return errorText.isEmpty() ? "发送失败" : "发送失败";
        }
    }
}
