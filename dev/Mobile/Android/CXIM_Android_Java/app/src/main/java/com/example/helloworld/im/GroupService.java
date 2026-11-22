package com.example.helloworld.im;

import com.example.helloworld.im.model.GroupInfo;
import com.example.helloworld.im.model.GroupMember;
import com.example.helloworld.im.model.GroupRequestInfo;
import com.google.gson.JsonObject;

import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.List;

/**
 * 群聊模块（协议 §4.14）。建群/邀请/移出/任命/转让/解散/改资料/审批入群都走 HTTP，
 * 变更后的实时事件走 TCP 140~144，群消息本体走 TCP type=1/2 + is_group=true。
 */
public class GroupService {

    /** 群详情回调 */
    public interface DetailCb {
        void onResult(boolean ok, String error, GroupInfo group, List<GroupMember> members);
    }

    /** 通用布尔回调 */
    public interface BoolCb {
        void onResult(boolean ok, String error, JsonObject data);
    }

    private final HttpClient http;

    public GroupService(HttpClient http) {
        this.http = http;
    }

    // ===== 建群 / 群列表 / 群详情 =====

    public void createGroup(String name, long[] memberIds, DetailCb cb) {
        if (utf8Bytes(name) == 0 || utf8Bytes(name) > 128) {
            cb.onResult(false, "群名称不能为空且不超过 128 字节", null, new ArrayList<>());
            return;
        }
        if (memberIds.length == 0) {
            cb.onResult(false, "请至少选择 1 位好友", null, new ArrayList<>());
            return;
        }
        JsonObject body = new JsonObject();
        body.addProperty("name", name.trim());
        body.add("memberIds", Json.longArray(memberIds));
        http.post("/api/groups", body, r -> cb.onResult(r.ok, r.errorText("建群失败"),
                r.ok ? GroupInfo.fromJson(Json.obj(r.obj(), "group")) : null,
                r.ok ? parseMembers(r) : new ArrayList<>()));
    }

    public void listGroups(Callback cb) {
        http.get("/api/groups", null, cb);
    }

    public static List<GroupInfo> parseGroups(ApiResult r) {
        List<GroupInfo> out = new ArrayList<>();
        for (JsonObject o : Json.objList(Json.arr(r.obj(), "groups"))) {
            out.add(GroupInfo.fromJson(o));
        }
        return out;
    }

    public void groupDetail(long groupId, DetailCb cb) {
        http.get("/api/groups/" + groupId, null, r -> cb.onResult(r.ok, r.errorText("群信息加载失败"),
                r.ok ? GroupInfo.fromJson(Json.obj(r.obj(), "group")) : null,
                r.ok ? parseMembers(r) : new ArrayList<>()));
    }

    public static List<GroupMember> parseMembers(ApiResult r) {
        List<GroupMember> out = new ArrayList<>();
        for (JsonObject o : Json.objList(Json.arr(r.obj(), "members"))) {
            out.add(GroupMember.fromJson(o));
        }
        return out;
    }

    // ===== 群资料 / 成员管理 =====

    /**
     * 修改群资料。空串表示不修改该字段（服务端语义），因此无法把公告清空——
     * 桌面端同样的取舍，这里保持一致。
     */
    public void updateGroup(long groupId, String name, String announcement, String intro, BoolCb cb) {
        if (utf8Bytes(name) > 128) {
            cb.onResult(false, "群名称不能为空且不超过 128 字节", null);
            return;
        }
        if (utf8Bytes(announcement) > 512) {
            cb.onResult(false, "群公告不能超过 512 字节", null);
            return;
        }
        if (utf8Bytes(intro) > 255) {
            cb.onResult(false, "群简介不能超过 255 字节", null);
            return;
        }
        JsonObject body = new JsonObject();
        body.addProperty("name", name == null ? "" : name.trim());
        body.addProperty("announcement", announcement == null ? "" : announcement);
        body.addProperty("intro", intro == null ? "" : intro);
        body.addProperty("avatarUrl", "");
        http.patch("/api/groups/" + groupId, body, r ->
                cb.onResult(r.ok, r.errorText("保存失败"), r.obj()));
    }

    public void inviteMembers(long groupId, long[] memberIds, DetailCb cb) {
        JsonObject body = new JsonObject();
        body.add("memberIds", Json.longArray(memberIds));
        http.post("/api/groups/" + groupId + "/members", body, r -> cb.onResult(r.ok, r.errorText("邀请失败"),
                r.ok ? GroupInfo.fromJson(Json.obj(r.obj(), "group")) : null,
                r.ok ? parseMembers(r) : new ArrayList<>()));
    }

    public void removeMember(long groupId, long userId, BoolCb cb) {
        http.delete("/api/groups/" + groupId + "/members/" + userId, r ->
                cb.onResult(r.ok, r.errorText("移出失败"), r.obj()));
    }

    /** 仅群主可操作；admin=true 升管理员，false 降为普通成员 */
    public void setAdmin(long groupId, long userId, boolean admin, BoolCb cb) {
        JsonObject body = new JsonObject();
        body.addProperty("userId", userId);
        body.addProperty("admin", admin);
        http.post("/api/groups/" + groupId + "/admins", body, r ->
                cb.onResult(r.ok, r.errorText("操作失败"), r.obj()));
    }

    public void transferOwner(long groupId, long toUserId, BoolCb cb) {
        JsonObject body = new JsonObject();
        body.addProperty("toUserId", toUserId);
        http.post("/api/groups/" + groupId + "/transfer", body, r ->
                cb.onResult(r.ok, r.errorText("转让失败"), r.obj()));
    }

    /** 退群：群主必须先转让 */
    public void quitGroup(long groupId, BoolCb cb) {
        http.post("/api/groups/" + groupId + "/quit", new JsonObject(), r ->
                cb.onResult(r.ok, r.errorText("退群失败"), r.obj()));
    }

    /** 解散：软删 status=1，清成员/游标/离线队列 */
    public void dismissGroup(long groupId, BoolCb cb) {
        http.post("/api/groups/" + groupId + "/dismiss", new JsonObject(), r ->
                cb.onResult(r.ok, r.errorText("解散失败"), r.obj()));
    }

    // ===== 入群申请与审批 =====

    public void requestJoin(long groupId, String message, BoolCb cb) {
        if (utf8Bytes(message) > 255) {
            cb.onResult(false, "附言不能超过 255 字节", null);
            return;
        }
        JsonObject body = new JsonObject();
        body.addProperty("groupId", groupId);
        body.addProperty("message", message == null ? "" : message.trim());
        http.post("/api/groups/requests", body, r ->
                cb.onResult(r.ok, r.errorText("申请失败"), r.obj()));
    }

    public void listJoinRequests(String direction, Callback cb) {
        http.get("/api/groups/requests", http.query("direction", direction), cb);
    }

    public static List<GroupRequestInfo> parseRequests(ApiResult r) {
        List<GroupRequestInfo> out = new ArrayList<>();
        for (JsonObject o : Json.objList(Json.arr(r.obj(), "requests"))) {
            out.add(GroupRequestInfo.fromJson(o));
        }
        return out;
    }

    /** 待审批（status=0）条数，群聊 tab 的角标用 */
    public static int countPending(ApiResult r) {
        int n = 0;
        for (GroupRequestInfo req : parseRequests(r)) {
            if (req.status == 0) {
                n++;
            }
        }
        return n;
    }

    public void acceptJoin(long requestId, BoolCb cb) {
        http.post("/api/groups/requests/" + requestId + "/accept", new JsonObject(), r ->
                cb.onResult(r.ok, r.errorText("审批失败"), r.obj()));
    }

    public void rejectJoin(long requestId, BoolCb cb) {
        http.post("/api/groups/requests/" + requestId + "/reject", new JsonObject(), r ->
                cb.onResult(r.ok, r.errorText("审批失败"), r.obj()));
    }

    private static int utf8Bytes(String s) {
        return s == null ? 0 : s.getBytes(StandardCharsets.UTF_8).length;
    }
}
