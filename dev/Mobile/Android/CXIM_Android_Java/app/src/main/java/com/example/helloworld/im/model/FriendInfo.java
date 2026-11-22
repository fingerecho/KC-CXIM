package com.example.helloworld.im.model;

import com.example.helloworld.im.Json;
import com.google.gson.JsonObject;

/** 好友项（协议 §4.5） */
public class FriendInfo {
    public static final int STATUS_FRIEND = 1;
    public static final int STATUS_BLOCKED = 2;

    public long userId;
    public String username = "";
    public String nickname = "";
    public String avatarUrl = "";
    /** 我给对方设置的备注，只影响自己的列表 */
    public String remark = "";
    public int status = STATUS_FRIEND;

    public static FriendInfo fromJson(JsonObject o) {
        FriendInfo f = new FriendInfo();
        f.userId = Json.num(o, "userId");
        f.username = Json.str(o, "username");
        f.nickname = Json.str(o, "nickname");
        f.avatarUrl = Json.str(o, "avatarUrl");
        f.remark = Json.str(o, "remark");
        f.status = Json.integer(o, "status", STATUS_FRIEND);
        return f;
    }

    /** 备注优先 */
    public String displayName() {
        if (!remark.isEmpty()) {
            return remark;
        }
        if (!nickname.isEmpty()) {
            return nickname;
        }
        if (!username.isEmpty()) {
            return username;
        }
        return String.valueOf(userId);
    }
}
