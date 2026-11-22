const im = require('../../services/im');
const emitter = require('../../utils/emitter');
const format = require('../../utils/format');

Page({
  data: {
    sessions: [],
    connected: false,
    connectedShow: false,
    ready: false,
  },

  onLoad() {
    this._offMsg = emitter.on('msg', () => this.refresh());
    this._offSessions = emitter.on('sessions', () => this.refresh());
    this._offGroup = emitter.on('group-list', () => this.refresh());
    this._offGroupChg = emitter.on('group-member-changed', () => this.refresh());
    this._offDismiss = emitter.on('group-dismissed', () => this.refresh());
    this._offState = emitter.on('state', (s) => {
      const connected = s === 'connected';
      wx.nextTick(() => {
        this.setData({ connected, connectedShow: this.data.sessions.length > 0 });
      });
    });
    this._offAuth = emitter.on('auth', (res) => {
      if (!res.ok && !res.reason && this.data.ready) {
        wx.reLaunch({ url: '/pages/login/login' });
      }
    });
  },

  onShow() {
    this.setData({ ready: true, connected: im.isConnected(), connectedShow: true });
    this.refresh();
    if (!im.currentUser()) {
      wx.reLaunch({ url: '/pages/login/login' });
    }
  },

  onUnload() {
    this._offMsg && this._offMsg();
    this._offSessions && this._offSessions();
    this._offGroup && this._offGroup();
    this._offGroupChg && this._offGroupChg();
    this._offDismiss && this._offDismiss();
    this._offState && this._offState();
    this._offAuth && this._offAuth();
  },

  refresh() {
    const sessions = im.getSessions().map((s) => ({
      id: s.id,
      name: s.name,
      isGroup: !!s.isGroup,
      lastMsg: s.lastMsg,
      unread: s.unread,
      time: format.formatChatListTime(s.ts),
      avatarChar: s.isGroup ? '群' : (s.name || s.id)[0],
      avatarCls: s.isGroup ? 'avatar-purple' : format.avatarClass(s.id),
    }));
    wx.nextTick(() => {
      this.setData({ sessions, connectedShow: this.data.connected && sessions.length >= 0 });
    });
  },

  openChat(e) {
    const { id, name, isgroup } = e.currentTarget.dataset;
    let url = '/pages/chat/chat?peerId=' + encodeURIComponent(id) + '&name=' + encodeURIComponent(name || id);
    if (isgroup) url += '&isGroup=1';
    wx.navigateTo({ url });
  },

  goGroupList() {
    wx.navigateTo({ url: '/pages/group-list/group-list' });
  },

  goCreateGroup() {
    wx.navigateTo({ url: '/pages/create-group/create-group' });
  },
});
