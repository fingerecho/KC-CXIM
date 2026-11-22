package com.example.helloworld.im.model;

import com.example.helloworld.im.Json;
import com.google.gson.JsonObject;

/** 群成员（协议 §4.14.2 的 members 数组项） */
public class GroupMember {
    public long userId;
    public String username = "";
    public String nickname = "";
    public String avatarUrl = "";
    public String groupNickname = "";
    public int role = GroupInfo.ROLE_MEMBER;
    public long joinedTs;

    public static GroupMember fromJson(JsonObject o) {
        GroupMember m = new GroupMember();
        m.userId = Json.num(o, "userId");
        m.username = Json.str(o, "username");
        m.nickname = Json.str(o, "nickname");
        m.avatarUrl = Json.str(o, "avatarUrl");
        m.groupNickname = Json.str(o, "groupNickname");
        m.role = Json.integer(o, "role", GroupInfo.ROLE_MEMBER);
        m.joinedTs = Json.num(o, "joinedTs");
        return m;
    }

    /** 群内昵称优先 */
    public String displayName() {
        if (!groupNickname.isEmpty()) {
            return groupNickname;
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
