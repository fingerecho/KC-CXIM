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
    this._offState = emitter.on('state', (s) => {
      const connected = s === 'connected';
      // 事件可能在别的 setData 周期内触发，延迟一帧避免递归更新
      wx.nextTick(() => {
        this.setData({ connected, connectedShow: this.data.sessions.length > 0 });
      });
    });
    this._offAuth = emitter.on('auth', (res) => {
      if (!res.ok && this.data.ready) {
        // token 失效等情况，回登录页
        wx.reLaunch({ url: '/pages/login/login' });
      }
    });
  },

  onShow() {
    this.setData({ ready: true, connected: im.isConnected(), connectedShow: true });
    this.refresh();
    // 未登录被踢到登录页
    if (!im.currentUser()) {
      wx.reLaunch({ url: '/pages/login/login' });
    }
  },

  onUnload() {
    this._offMsg && this._offMsg();
    this._offSessions && this._offSessions();
    this._offState && this._offState();
    this._offAuth && this._offAuth();
  },

  refresh() {
    const sessions = im.getSessions().map((s) => ({
      id: s.id,
      name: s.name,
      lastMsg: s.lastMsg,
      unread: s.unread,
      time: format.formatChatListTime(s.ts),
      avatarChar: (s.name || s.id)[0],
      avatarCls: format.avatarClass(s.id),
    }));
    // 避免在别的 setData 周期内触发递归更新
    wx.nextTick(() => {
      this.setData({ sessions, connectedShow: this.data.connected && sessions.length >= 0 });
    });
  },

  openChat(e) {
    const { id, name } = e.currentTarget.dataset;
    wx.navigateTo({ url: '/pages/chat/chat?peerId=' + id + '&name=' + encodeURIComponent(name || id) });
  },
});
