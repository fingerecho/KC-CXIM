const im = require('../../services/im');
const api = require('../../services/api');
const emitter = require('../../utils/emitter');
const format = require('../../utils/format');

Page({
  data: {
    friends: [],
    requests: [],       // 收到的好友申请
    keyword: '',
    searching: false,
    searchResults: null, // null=未搜索，[]=无结果
  },

  onLoad() {
    this._offReq = emitter.on('friend-request', () => this.refresh());
    this._offRes = emitter.on('friend-result', () => this.refresh());
    this._offChg = emitter.on('friends-changed', () => this.refresh());
    this._offContacts = emitter.on('contacts', () => this.refresh());
  },

  onUnload() {
    this._offReq && this._offReq();
    this._offRes && this._offRes();
    this._offChg && this._offChg();
    this._offContacts && this._offContacts();
  },

  onShow() {
    this.refresh();
    im.refreshFriendsCache();
    this.loadRequests();
  },

  refresh() {
    const contacts = store_contacts(this);
    const friends = contacts.map((c) => ({
      id: c.id,
      name: c.name,
      avatarChar: (c.name || c.id)[0],
      avatarCls: format.avatarClass(c.id),
    }));
    this.setData({ friends });
  },

  /** 收到的好友申请列表（HTTP） */
  async loadRequests() {
    try {
      const requests = await api.getFriendRequests('incoming');
      this.setData({
        requests: requests
          .filter((r) => r.status === 0)
          .map((r) => ({
            requestId: r.requestId,
            requesterId: r.requesterId,
            name: r.requesterNickname || r.requesterUsername || ('用户 ' + r.requesterId),
            message: r.message || '',
            avatarCls: format.avatarClass(r.requesterId),
          })),
      });
      // 清掉「好友申请」角标计数
      store_clearRequestBadge();
    } catch (e) {
      console.warn('[contacts] 拉取好友申请失败', e.message);
    }
  },

  async onAccept(e) {
    const requestId = e.currentTarget.dataset.requestid;
    try {
      await api.acceptFriendRequest(requestId);
      wx.showToast({ title: '已添加好友', icon: 'success' });
      this.loadRequests();
      im.refreshFriendsCache();
    } catch (err) {
      wx.showToast({ title: err.message || '操作失败', icon: 'none' });
    }
  },

  async onReject(e) {
    const requestId = e.currentTarget.dataset.requestid;
    try {
      await api.rejectFriendRequest(requestId);
      wx.showToast({ title: '已拒绝', icon: 'none' });
      this.loadRequests();
    } catch (err) {
      wx.showToast({ title: err.message || '操作失败', icon: 'none' });
    }
  },

  // ---------- 搜索加好友 ----------

  onKeyword(e) {
    this.setData({ keyword: e.detail.value.trim() });
  },

  async onSearch() {
    const { keyword } = this.data;
    if (!keyword) {
      this.setData({ searchResults: null });
      return;
    }
    this.setData({ searching: true });
    try {
      const users = await api.searchUsers(keyword, 20);
      this.setData({
        searchResults: users.map((u) => ({
          userId: u.userId,
          name: u.nickname || u.username || ('用户 ' + u.userId),
          avatarCls: format.avatarClass(u.userId),
        })),
      });
    } catch (e) {
      wx.showToast({ title: e.message || '搜索失败', icon: 'none' });
      this.setData({ searchResults: [] });
    } finally {
      this.setData({ searching: false });
    }
  },

  async onAddFriend(e) {
    const { userid, name } = e.currentTarget.dataset;
    try {
      await api.sendFriendRequest(userid, '你好，我是 ' + ((im.currentUser() || {}).name || ''));
      wx.showToast({ title: '申请已发送，等待对方通过', icon: 'none', duration: 2500 });
      const results = this.data.searchResults.filter((u) => u.userId !== userid);
      this.setData({ searchResults: results });
    } catch (err) {
      wx.showToast({ title: err.message || '发送申请失败', icon: 'none' });
    }
  },

  // ---------- 联系人操作 ----------

  openChat(e) {
    const { id, name } = e.currentTarget.dataset;
    wx.navigateTo({ url: '/pages/chat/chat?peerId=' + id + '&name=' + encodeURIComponent(name || id) });
  },

  onLongPressFriend(e) {
    const { id, name } = e.currentTarget.dataset;
    wx.showActionSheet({
      itemList: ['设置备注', '删除好友'],
      success: (res) => {
        if (res.tapIndex === 0) this.editRemark(id);
        if (res.tapIndex === 1) this.deleteFriend(id, name);
      },
    });
  },

  editRemark(id) {
    wx.showModal({
      title: '设置备注',
      editable: true,
      placeholderText: '输入备注名',
      success: async (res) => {
        if (!res.confirm) return;
        try {
          await api.setFriendRemark(id, res.content || '');
          wx.showToast({ title: '已保存', icon: 'success' });
          im.refreshFriendsCache();
        } catch (e) {
          wx.showToast({ title: e.message || '保存失败', icon: 'none' });
        }
      },
    });
  },

  deleteFriend(id, name) {
    wx.showModal({
      title: '删除好友',
      content: '确定删除「' + name + '」吗？',
      confirmColor: '#f5523d',
      success: async (res) => {
        if (!res.confirm) return;
        try {
          await api.deleteFriend(id);
          wx.showToast({ title: '已删除', icon: 'success' });
          im.refreshFriendsCache();
        } catch (e) {
          wx.showToast({ title: e.message || '删除失败', icon: 'none' });
        }
      },
    });
  },
})

/** 从 im 好友缓存读取联系人 */
function store_contacts(page) {
  return page ? require('../../services/store').get().contacts || [] : [];
}

function store_clearRequestBadge() {
  const store = require('../../services/store');
  store.clearUnread('__friend_requests__');
}
