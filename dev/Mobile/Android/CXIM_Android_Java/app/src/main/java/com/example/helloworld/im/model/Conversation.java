package com.example.helloworld.im.model;

/**
 * 会话聚合项（纯客户端概念，服务端没有“会话列表”接口）。
 * 键规则与桌面端 ChatService::convKey 一致：单聊 s:peerId，群聊 g:peerId。
 */
public class Conversation {

    public long peerId;
    public boolean isGroup;
    /** 标题由通讯录解析（好友备注/昵称 或 群名），服务层不填 */
    public String title = "";
    public String avatarUrl = "";
    public ChatMessage lastMessage;
    public boolean hasLast;
    public int unread;

    public Conversation() {
    }

    public Conversation(long peerId, boolean isGroup) {
        this.peerId = peerId;
        this.isGroup = isGroup;
    }

    public String key() {
        return keyOf(peerId, isGroup);
    }

    public static String keyOf(long peerId, boolean isGroup) {
        return (isGroup ? "g:" : "s:") + peerId;
    }

    public long lastTs() {
        return hasLast && lastMessage != null ? lastMessage.ts : 0;
    }

    public String previewText() {
        return hasLast && lastMessage != null ? lastMessage.preview() : "";
    }
}
