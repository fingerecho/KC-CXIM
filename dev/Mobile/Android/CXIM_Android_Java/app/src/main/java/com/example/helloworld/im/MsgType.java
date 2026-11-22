package com.example.helloworld.im;

/**
 * TCP 消息类型常量，对应协议文档 §5.3 与桌面端 imstypes.h 的 MsgType 命名空间。
 * 未知 type 上行会被服务端判定非法并直接断开连接，因此发送前必须使用这里的常量。
 */
public final class MsgType {

    private MsgType() {
    }

    // ===== 业务消息（双向） =====
    /** 文本消息 */
    public static final int TEXT = 1;
    /** 图片消息，content 为媒体元数据 JSON */
    public static final int IMAGE = 2;
    /** 心跳（仅上行） */
    public static final int HEARTBEAT = 3;
    /** 文件消息，content 为媒体元数据 JSON */
    public static final int FILE = 4;

    // ===== 连接控制 =====
    public static final int LOGIN = 100;
    public static final int LOGIN_ACK = 101;
    public static final int ERROR = 102;
    public static final int HISTORY_REQ = 110;
    public static final int HISTORY_RESP = 111;
    /** 投递 / 已读回执（仅上行） */
    public static final int ACK = 112;
    public static final int SEND_ACK = 113;

    // ===== 好友事件（仅下行） =====
    public static final int FRIEND_REQUEST = 130;
    public static final int FRIEND_REQUEST_RESULT = 131;
    public static final int FRIEND_LIST_CHANGED = 132;

    // ===== 群事件（仅下行） =====
    public static final int GROUP_MEMBER_CHANGED = 140;
    public static final int GROUP_DISMISSED = 141;
    public static final int GROUP_INFO_CHANGED = 142;
    public static final int GROUP_REQUEST = 143;
    public static final int GROUP_REQUEST_RESULT = 144;

    /** 该类型是否由服务端下行使用 */
    public static boolean isServerEvent(int type) {
        return type >= FRIEND_REQUEST && type <= GROUP_REQUEST_RESULT;
    }
}
