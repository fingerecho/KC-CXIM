package com.example.helloworld.im.model;

import com.example.helloworld.im.Json;
import com.google.gson.JsonObject;

/** 入群申请项（协议 §4.14.7） */
public class GroupRequestInfo {
    public long requestId;
    public long groupId;
    public String groupName = "";
    public long ownerId;
    public long requesterId;
    public String requesterUsername = "";
    public String requesterNickname = "";
    /** 审批人，0=未处理 */
    public long handlerId;
    public String message = "";
    public int status;
    public long createdTs;

    public static GroupRequestInfo fromJson(JsonObject o) {
        GroupRequestInfo r = new GroupRequestInfo();
        r.requestId = Json.num(o, "requestId");
        r.groupId = Json.num(o, "groupId");
        r.groupName = Json.str(o, "groupName");
        r.ownerId = Json.num(o, "ownerId");
        r.requesterId = Json.num(o, "requesterId");
        r.requesterUsername = Json.str(o, "requesterUsername");
        r.requesterNickname = Json.str(o, "requesterNickname");
        r.handlerId = Json.num(o, "handlerId");
        r.message = Json.str(o, "message");
        r.status = Json.integer(o, "status");
        r.createdTs = Json.num(o, "createdTs");
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

    public String groupNameDisplay() {
        return groupName.isEmpty() ? ("群" + groupId) : groupName;
    }
}
