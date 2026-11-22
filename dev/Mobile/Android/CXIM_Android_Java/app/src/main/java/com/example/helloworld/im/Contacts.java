package com.example.helloworld.im;

import com.example.helloworld.im.model.FriendInfo;
import com.example.helloworld.im.model.GroupInfo;
import com.example.helloworld.im.model.GroupMember;

import java.util.ArrayList;
import java.util.Collections;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;

/**
 * 通讯录缓存：好友列表 + 群列表 + 群成员。
 *
 * <p>协议 §5.15 明确「好友列表变化(132) / 群成员变化(140) / 群解散(141) 不持久化」，
 * 所以这里的口径是：收到事件 → 全量重拉 HTTP 对齐，本地只做展示缓存与名称解析。
 */
public class Contacts {

    public interface Listener {
        void onFriendsChanged();

        void onGroupsChanged();

        /** friendPending = 待我处理的好友申请数，groupPending = 待我审批的入群申请数 */
        void onPendingChanged(int friendPending, int groupPending);
    }

    public static class SimpleListener implements Listener {
        @Override
        public void onFriendsChanged() {
        }

        @Override
        public void onGroupsChanged() {
        }

        @Override
        public void onPendingChanged(int friendPending, int groupPending) {
        }
    }

    private final Map<Long, FriendInfo> friends = new LinkedHashMap<>();
    private final Map<Long, GroupInfo> groups = new LinkedHashMap<>();
    private final Map<Long, List<GroupMember>> members = new LinkedHashMap<>();
    private final List<Listener> listeners = new ArrayList<>();

    private volatile int friendPending;
    private volatile int groupPending;

    public void addListener(Listener l) {
        synchronized (listeners) {
            if (l != null && !listeners.contains(l)) {
                listeners.add(l);
            }
        }
    }

    public void removeListener(Listener l) {
        synchronized (listeners) {
            listeners.remove(l);
        }
    }

    private List<Listener> snapshot() {
        synchronized (listeners) {
            return new ArrayList<>(listeners);
        }
    }

    // ===== 好友 =====

    public void setFriends(List<FriendInfo> list) {
        synchronized (friends) {
            friends.clear();
            for (FriendInfo f : list) {
                friends.put(f.userId, f);
            }
        }
        for (Listener l : snapshot()) {
            l.onFriendsChanged();
        }
    }

    public List<FriendInfo> friends() {
        synchronized (friends) {
            return new ArrayList<>(friends.values());
        }
    }

    public FriendInfo friend(long userId) {
        synchronized (friends) {
            return friends.get(userId);
        }
    }

    public boolean isFriend(long userId) {
        synchronized (friends) {
            return friends.containsKey(userId);
        }
    }

    /** 好友展示名：备注 &gt; 昵称 &gt; 用户名 &gt; ID */
    public String friendName(long userId) {
        FriendInfo f = friend(userId);
        return f == null ? ("用户" + userId) : f.displayName();
    }

    // ===== 群 =====

    public void setGroups(List<GroupInfo> list) {
        synchronized (groups) {
            groups.clear();
            for (GroupInfo g : list) {
                groups.put(g.groupId, g);
            }
        }
        for (Listener l : snapshot()) {
            l.onGroupsChanged();
        }
    }

    public List<GroupInfo> groups() {
        synchronized (groups) {
            return new ArrayList<>(groups.values());
        }
    }

    public GroupInfo group(long groupId) {
        synchronized (groups) {
            return groups.get(groupId);
        }
    }

    public void putGroup(GroupInfo g) {
        if (g == null) {
            return;
        }
        synchronized (groups) {
            groups.put(g.groupId, g);
        }
        for (Listener l : snapshot()) {
            l.onGroupsChanged();
        }
    }

    public void removeGroup(long groupId) {
        synchronized (groups) {
            groups.remove(groupId);
        }
        synchronized (members) {
            members.remove(groupId);
        }
        for (Listener l : snapshot()) {
            l.onGroupsChanged();
        }
    }

    public void setMembers(long groupId, List<GroupMember> list) {
        synchronized (members) {
            members.put(groupId, list);
        }
    }

    public List<GroupMember> members(long groupId) {
        synchronized (members) {
            List<GroupMember> l = members.get(groupId);
            return l == null ? new ArrayList<>() : new ArrayList<>(l);
        }
    }

    public GroupMember member(long groupId, long userId) {
        for (GroupMember m : members(groupId)) {
            if (m.userId == userId) {
                return m;
            }
        }
        return null;
    }

    /** 群聊气泡上方的发送者名：群内昵称 &gt; 昵称 &gt; 用户名 &gt; 用户N */
    public String senderName(long groupId, long userId) {
        GroupMember m = member(groupId, userId);
        if (m != null) {
            return m.displayName();
        }
        FriendInfo f = friend(userId);
        return f != null ? f.displayName() : ("用户" + userId);
    }

    /** 会话标题：群名 或 好友展示名 */
    public String titleOf(long peerId, boolean isGroup) {
        if (isGroup) {
            GroupInfo g = group(peerId);
            return g == null ? ("群" + peerId) : g.displayName();
        }
        return friendName(peerId);
    }

    // ===== 角标 =====

    public void setPending(int friendPending, int groupPending) {
        this.friendPending = friendPending;
        this.groupPending = groupPending;
        for (Listener l : snapshot()) {
            l.onPendingChanged(friendPending, groupPending);
        }
    }

    public int friendPending() {
        return friendPending;
    }

    public int groupPending() {
        return groupPending;
    }

    public void clear() {
        synchronized (friends) {
            friends.clear();
        }
        synchronized (groups) {
            groups.clear();
        }
        synchronized (members) {
            members.clear();
        }
        friendPending = 0;
        groupPending = 0;
    }

    /** 好友按展示名排序（服务端返回顺序不保证） */
    public List<FriendInfo> sortedFriends() {
        List<FriendInfo> list = friends();
        Collections.sort(list, (a, b) -> a.displayName().compareToIgnoreCase(b.displayName()));
        return list;
    }

    /** 群按创建时间倒序，最新的在前面 */
    public List<GroupInfo> sortedGroups() {
        List<GroupInfo> list = groups();
        Collections.sort(list, (a, b) -> Long.compare(b.createdTs, a.createdTs));
        return list;
    }
}
