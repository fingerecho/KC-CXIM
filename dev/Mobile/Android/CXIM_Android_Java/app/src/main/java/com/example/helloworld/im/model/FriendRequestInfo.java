package com.example.helloworld.im.model;

import com.example.helloworld.im.Json;
import com.google.gson.JsonObject;

/** 好友申请项（协议 §4.6 / §4.9） */
public class FriendRequestInfo {
    public static final int PENDING = 0;
    public static final int ACCEPTED = 1;
    public static final int REJECTED = 2;
    public static final int CANCELLED = 3;

    public long requestId;
    public long requesterId;
    public long targetId;
    public String requesterUsername = "";
    public String requesterNickname = "";
    public String targetUsername = "";
    public String targetNickname = "";
    public String message = "";
    public int status;
    public long createdTs;

    public static FriendRequestInfo fromJson(JsonObject o) {
        FriendRequestInfo r = new FriendRequestInfo();
        r.requestId = Json.num(o, "requestId");
        r.requesterId = Json.num(o, "requesterId");
        r.targetId = Json.num(o, "targetId");
        r.requesterUsername = Json.str(o, "requesterUsername");
        r.requesterNickname = Json.str(o, "requesterNickname");
        r.targetUsername = Json.str(o, "targetUsername");
        r.targetNickname = Json.str(o, "targetNickname");
        r.message = Json.str(o, "message");
        r.status = Json.integer(o, "status");
        // 单条接口返回 createdTs，批量接口返回 createdAt，两者都兼容
        r.createdTs = Json.num(o, "createdTs");
        if (r.createdTs == 0) {
            r.createdTs = Json.num(o, "createdAt");
        }
        return r;
    }

    public String requesterName() {
        if (!requesterNickname.isEmpty()) {
            return requesterNickname;
        }
        if (!requesterUsername.isEmpty()) {
            return requesterUsername;
        }
        return String.valueOf(requesterId);
    }

    public String targetName() {
        if (!targetNickname.isEmpty()) {
            return targetNickname;
        }
        if (!targetUsername.isEmpty()) {
            return targetUsername;
        }
        return String.valueOf(targetId);
    }
}
