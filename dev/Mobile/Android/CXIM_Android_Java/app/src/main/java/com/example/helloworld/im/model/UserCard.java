package com.example.helloworld.im.model;

import com.example.helloworld.im.Json;
import com.google.gson.JsonObject;

/** 用户公开资料（协议 §4.4 搜索结果 / §4.5 好友项的公共字段） */
public class UserCard {
    public long userId;
    public String username = "";
    public String nickname = "";
    public String avatarUrl = "";

    public static UserCard fromJson(JsonObject o) {
        UserCard u = new UserCard();
        u.userId = Json.num(o, "userId");
        u.username = Json.str(o, "username");
        u.nickname = Json.str(o, "nickname");
        u.avatarUrl = Json.str(o, "avatarUrl");
        return u;
    }

    /** 展示名优先级：昵称 &gt; 用户名 &gt; ID */
    public String displayName() {
        if (!nickname.isEmpty()) {
            return nickname;
        }
        if (!username.isEmpty()) {
            return username;
        }
        return String.valueOf(userId);
    }
}
