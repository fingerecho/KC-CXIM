package com.example.helloworld.im;

import com.example.helloworld.im.model.FriendInfo;
import com.example.helloworld.im.model.FriendRequestInfo;
import com.example.helloworld.im.model.UserCard;
import com.google.gson.JsonObject;

import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.List;
import java.util.Map;

/**
 * 好友模块（协议 §4.4 ~ §4.13）。
 *
 * <p>设计原则：**好友关系的事实变更走 HTTP，变更后的实时事件走 TCP 推送**。
 * 所以这里全是请求-响应，不持有状态；UI 收到 130/131/132 事件后重新调本服务拉全量对齐。
 */
public class FriendService {

    /** 添加好友时的默认附言（与桌面端一致） */
    public static final String DEFAULT_REQUEST_MESSAGE = "你好，我想加你为好友";

    private final HttpClient http;

    public FriendService(HttpClient http) {
        this.http = http;
    }

    // ===== 搜索用户 =====

    public void searchUsers(String keyword, Callback cb) {
        Map<String, String> q = http.query("keyword", keyword.trim(), "limit", "20");
        http.get("/api/users/search", q, cb);
    }

    public static List<UserCard> parseUsers(ApiResult r) {
        List<UserCard> out = new ArrayList<>();
        for (JsonObject o : Json.objList(Json.arr(r.obj(), "users"))) {
            out.add(UserCard.fromJson(o));
        }
        return out;
    }

    // ===== 好友列表 =====

    public void listFriends(Callback cb) {
        http.get("/api/friends", null, cb);
    }

    public static List<FriendInfo> parseFriends(ApiResult r) {
        List<FriendInfo> out = new ArrayList<>();
        for (JsonObject o : Json.objList(Json.arr(r.obj(), "friends"))) {
            out.add(FriendInfo.fromJson(o));
        }
        return out;
    }

    // ===== 好友申请 =====

    /** direction: incoming(默认) / outgoing / all */
    public void listRequests(String direction, Callback cb) {
        http.get("/api/friends/requests", http.query("direction", direction), cb);
    }

    public static List<FriendRequestInfo> parseRequests(ApiResult r) {
        List<FriendRequestInfo> out = new ArrayList<>();
        for (JsonObject o : Json.objList(Json.arr(r.obj(), "requests"))) {
            out.add(FriendRequestInfo.fromJson(o));
        }
        return out;
    }

    /** 只统计待处理的条数，通讯录角标用 */
    public static int countPending(ApiResult r) {
        int n = 0;
        for (FriendRequestInfo req : parseRequests(r)) {
            if (req.status == FriendRequestInfo.PENDING) {
                n++;
            }
        }
        return n;
    }

    public void sendRequest(long toUserId, String message, Callback cb) {
        String checked = checkMessage(message, cb);
        if (checked == null) {
            return;
        }
        JsonObject body = new JsonObject();
        body.addProperty("toUserId", toUserId);
        body.addProperty("message", checked);
        http.post("/api/friends/requests", body, cb);
    }

    /** 批量发送申请：单次最多 200 个目标，尽力而为，不满足条件的进 skipped */
    public void sendRequestsBatch(long[] toUserIds, String message, Callback cb) {
        if (toUserIds.length == 0 || toUserIds.length > 200) {
            cb.onResult(ApiResult.fail(400, "单次最多 200 个目标"));
            return;
        }
        String checked = checkMessage(message, cb);
        if (checked == null) {
            return;
        }
        JsonObject body = new JsonObject();
        body.add("toUserIds", Json.longArray(toUserIds));
        body.addProperty("message", checked);
        http.post("/api/friends/requests/batch", body, cb);
    }

    public void acceptRequest(long requestId, Callback cb) {
        http.post("/api/friends/requests/" + requestId + "/accept", new JsonObject(), cb);
    }

    public void rejectRequest(long requestId, Callback cb) {
        http.post("/api/friends/requests/" + requestId + "/reject", new JsonObject(), cb);
    }

    // ===== 备注 / 删除 / 拉黑 =====

    public void setRemark(long friendId, String remark, Callback cb) {
        if (utf8Bytes(remark) > 64) {
            cb.onResult(ApiResult.fail(400, "备注不能超过 64 字节"));
            return;
        }
        JsonObject body = new JsonObject();
        body.addProperty("remark", remark);
        http.patch("/api/friends/" + friendId, body, cb);
    }

    public void removeFriend(long friendId, Callback cb) {
        http.delete("/api/friends/" + friendId, cb);
    }

    public void batchRemoveFriends(long[] friendIds, Callback cb) {
        JsonObject body = new JsonObject();
        body.add("friendIds", Json.longArray(friendIds));
        http.post("/api/friends/batch-delete", body, cb);
    }

    public void blockFriend(long friendId, Callback cb) {
        http.post("/api/friends/" + friendId + "/block", new JsonObject(), cb);
    }

    public void unblockFriend(long friendId, Callback cb) {
        http.post("/api/friends/" + friendId + "/unblock", new JsonObject(), cb);
    }

    // ===== 工具 =====

    /**
     * 附言上限 255 字节（协议 §4.8：超长直接拒绝，不截断）。
     *
     * <p>本地静默截断会把用户写的话改掉却让他以为发原文成功了，
     * 而服务端本来也会拒；这里与群申请接口统一为「拒绝并给出原因」。
     * 返回 null 表示已回调失败、调用方应中止。
     */
    private String checkMessage(String message, Callback cb) {
        String m = message == null ? "" : message.trim();
        int n = utf8Bytes(m);
        if (n > 255) {
            cb.onResult(ApiResult.fail(400, "附言不能超过 255 字节（当前 " + n + " 字节）"));
            return null;
        }
        return m;
    }

    private static int utf8Bytes(String s) {
        return s == null ? 0 : s.getBytes(StandardCharsets.UTF_8).length;
    }
}
