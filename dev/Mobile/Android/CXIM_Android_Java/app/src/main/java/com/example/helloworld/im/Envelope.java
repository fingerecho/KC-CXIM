package com.example.helloworld.im;

import com.google.gson.JsonObject;

/**
 * TCP Message 信封（协议 §5.2）。上行统一使用 {@link #toUplinkJson()}，
 * 全字段序列化，避免服务端解析歧义；下行用 {@link #fromJson(JsonObject)} 解析。
 */
public class Envelope {

    public long from;
    public long to;
    public int type;
    /** 上行时为客户端临时消息号（去重键），下行时为服务端消息 ID */
    public long msgId;
    public long ts;
    public long conversationId;
    /** 会话内单调序号；HistoryReq 时复用为 before_seq */
    public long seq;
    public String content = "";
    public boolean isGroup;

    public Envelope() {
    }

    public Envelope(int type) {
        this.type = type;
    }

    /** 构造一个上行信封（from/ts 由服务端按登录身份与服务器时间生成） */
    public static Envelope uplink(int type, long to, long msgId, long seq,
                                  String content, boolean isGroup) {
        Envelope e = new Envelope(type);
        e.from = 0;
        e.to = to;
        e.msgId = msgId;
        e.ts = 0;
        e.conversationId = 0;
        e.seq = seq;
        e.content = content == null ? "" : content;
        e.isGroup = isGroup;
        return e;
    }

    /** 全字段紧凑序列化，仅用于上行 */
    public JsonObject toUplinkJson() {
        JsonObject o = new JsonObject();
        o.addProperty("from", from);
        o.addProperty("to", to);
        o.addProperty("type", type);
        o.addProperty("msg_id", msgId);
        o.addProperty("ts", ts);
        o.addProperty("conversation_id", conversationId);
        o.addProperty("seq", seq);
        o.addProperty("content", content == null ? "" : content);
        o.addProperty("is_group", isGroup);
        return o;
    }

    public static Envelope fromJson(JsonObject o) {
        Envelope e = new Envelope();
        e.from = Json.num(o, "from");
        e.to = Json.num(o, "to");
        // type 是协议必填字段。缺省时不能兜底成 TEXT(1)，否则一条畸形帧会被当成
        // 「对端 ID 为 0 的文本消息」入库；0 会被分发层的 default 分支忽略掉。
        e.type = Json.integer(o, "type", 0);
        e.msgId = Json.num(o, "msg_id");
        e.ts = Json.num(o, "ts");
        e.conversationId = Json.num(o, "conversation_id");
        e.seq = Json.num(o, "seq");
        e.content = Json.str(o, "content");
        e.isGroup = Json.bool(o, "is_group", false);
        return e;
    }

    /** 链式设置会话 ID（回执必须带 conversation_id + seq，否则服务端静默忽略） */
    public Envelope withConversation(long conversationId) {
        this.conversationId = conversationId;
        return this;
    }

    /**
     * 单聊消息的“对方 ID”：服务端下行的 from/to 以会话身份为准，
     * 自己发的消息 from 是自己，故需要按登录身份翻转一次。
     */
    public long peerId(long selfId) {
        if (isGroup) {
            return to != 0 ? to : from;
        }
        return from == selfId ? to : from;
    }
}
