package com.example.helloworld.im.model;

import com.example.helloworld.im.Json;
import com.google.gson.JsonObject;

/** 群资料（协议 §4.14） */
public class GroupInfo {
    public static final int ROLE_OWNER = 1;
    public static final int ROLE_ADMIN = 2;
    public static final int ROLE_MEMBER = 3;

    public long groupId;
    public long conversationId;
    public String name = "";
    public String announcement = "";
    public String intro = "";
    public String avatarUrl = "";
    public long ownerId;
    /** 0=正常 1=已解散 */
    public int status;
    public int memberLimit = 200;
    public int memberCount;
    /** 查询者角色 1/2/3，0=非成员 */
    public int myRole;
    public long createdTs;

    public static GroupInfo fromJson(JsonObject o) {
        GroupInfo g = new GroupInfo();
        g.groupId = Json.num(o, "groupId");
        g.conversationId = Json.num(o, "conversationId");
        g.name = Json.str(o, "name");
        g.announcement = Json.str(o, "announcement");
        g.intro = Json.str(o, "intro");
        g.avatarUrl = Json.str(o, "avatarUrl");
        g.ownerId = Json.num(o, "ownerId");
        g.status = Json.integer(o, "status");
        g.memberLimit = Json.integer(o, "memberLimit", 200);
        g.memberCount = Json.integer(o, "memberCount");
        g.myRole = Json.integer(o, "myRole");
        g.createdTs = Json.num(o, "createdTs");
        return g;
    }

    /** 群主与管理员都有改资料 / 邀请 / 移成员的权限（权限矩阵见协议 §4.14） */
    public boolean canManage() {
        return myRole == ROLE_OWNER || myRole == ROLE_ADMIN;
    }

    public boolean isOwner() {
        return myRole == ROLE_OWNER;
    }

    public String displayName() {
        return name.isEmpty() ? ("群" + groupId) : name;
    }

    public static String roleText(int role) {
        if (role == ROLE_OWNER) {
            return "群主";
        }
        if (role == ROLE_ADMIN) {
            return "管理员";
        }
        if (role == ROLE_MEMBER) {
            return "成员";
        }
        return "非成员";
    }
}
