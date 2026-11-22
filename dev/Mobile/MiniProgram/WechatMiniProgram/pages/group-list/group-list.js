const im = require('../../services/im');
const api = require('../../services/api');
const store = require('../../services/store');
const emitter = require('../../utils/emitter');
const format = require('../../utils/format');

Page({
  data: {
    tab: 'groups', // groups | requests
    groups: [],
    incoming: [],
    outgoing: [],
    badge: 0,
  },

  onLoad() {
    this._offList = emitter.on('group-list', () => this.refresh());
    this._offInfo = emitter.on('group-info', () => this.refresh());
    this._offReq = emitter.on('group-request', () => this.loadRequests());
    this._offResult = emitter.on('group-result', () => this.loadRequests());
    this._offDismiss = emitter.on('group-dismissed', () => this.refresh());
    this._offChg = emitter.on('group-member-changed', () => this.refresh());
  },

  onUnload() {
    this._offList && this._offList();
    this._offInfo && this._offInfo();
    this._offReq && this._offReq();
    this._offResult && this._offResult();
    this._offDismiss && this._offDismiss();
    this._offChg && this._offChg();
  },

  onShow() {
    this.refresh();
    this.loadRequests();
  },

  refresh() {
    const groups = store.getGroups().map((g) => ({
      groupId: g.groupId,
      name: g.name,
      memberCount: g.memberCount || 0,
      roleText: g.myRole === 1 ? '群主' : (g.myRole === 2 ? '管理员' : '成员'),
      avatarCls: 'avatar-purple',
    }));
    const incoming = store.getGroupRequests('incoming')
      .filter((r) => r.status === 0)
      .map((r) => ({
        requestId: r.requestId,
        groupId: r.groupId,
        groupName: r.groupName || ('群 ' + r.groupId),
        name: r.requesterNickname || r.requesterUsername || ('用户 ' + r.requesterId),
        message: r.message || '',
        avatarCls: format.avatarClass(r.requesterId),
      }));
    const outgoing = store.getGroupRequests('outgoing').map((r) => ({
      requestId: r.requestId,
      groupId: r.groupId,
      groupName: r.groupName || ('群 ' + r.groupId),
      statusText: r.status === 0 ? '待审批' : (r.status === 1 ? '已通过' : (r.status === 2 ? '已拒绝' : '已取消')),
    }));
    this.setData({
      groups,
      incoming,
      outgoing,
      badge: store.getUnread('__group_requests__'),
    });
  },

  loadRequests() {
    return im.refreshGroupPage().then(() => {
      store.clearUnread('__group_requests__');
      this.refresh();
    });
  },

  switchTab(e) {
    this.setData({ tab: e.currentTarget.dataset.tab });
    if (this.data.tab === 'requests') this.loadRequests();
  },

  openChat(e) {
    const { groupid, name } = e.currentTarget.dataset;
    const peerId = im.toPeerId(groupid, true);
    wx.navigateTo({
      url: '/pages/chat/chat?peerId=' + peerId + '&name=' + encodeURIComponent(name) + '&isGroup=1',
    });
  },

  openInfo(e) {
    wx.navigateTo({ url: '/pages/group-info/group-info?groupId=' + e.currentTarget.dataset.groupid });
  },

  goCreate() {
    wx.navigateTo({ url: '/pages/create-group/create-group' });
  },

  onJoinGroup() {
    wx.showModal({
      title: '申请入群',
      content: '',
      editable: true,
      placeholderText: '请输入群 ID（数字）',
      success: (res) => {
        if (!res.confirm) return;
        const groupId = String(res.content || '').trim();
        if (!/^\d+$/.test(groupId)) {
          wx.showToast({ title: '群 ID 必须是数字', icon: 'none' });
          return;
        }
        wx.showModal({
          title: '入群附言',
          content: '',
          editable: true,
          placeholderText: '选填，如：我是 Alice',
          success: async (r2) => {
            if (!r2.confirm) return;
            try {
              await api.requestJoinGroup(Number(groupId), r2.content || '');
              wx.showToast({ title: '申请已发送', icon: 'success' });
              this.loadRequests();
            } catch (err) {
              wx.showToast({ title: err.message || '申请失败', icon: 'none', duration: 2500 });
            }
          },
        });
      },
    });
  },

  async onAccept(e) {
    const requestId = e.currentTarget.dataset.requestid;
    try {
      await api.acceptGroupRequest(requestId);
      wx.showToast({ title: '已通过', icon: 'success' });
      this.loadRequests();
    } catch (err) {
      wx.showToast({ title: err.message || '操作失败', icon: 'none' });
    }
  },

  async onReject(e) {
    const requestId = e.currentTarget.dataset.requestid;
    try {
      await api.rejectGroupRequest(requestId);
      wx.showToast({ title: '已拒绝', icon: 'none' });
      this.loadRequests();
    } catch (err) {
      wx.showToast({ title: err.message || '操作失败', icon: 'none' });
    }
  },
});
