const im = require('../../services/im');
const store = require('../../services/store');
const emitter = require('../../utils/emitter');
const config = require('../../utils/config');
const format = require('../../utils/format');

Page({
  data: {
    user: null,
    avatarChar: '',
    avatarCls: '',
    server: '',
    connected: false,
    stateText: '',
  },

  onLoad() {
    this._offState = emitter.on('state', (s) => this.updateState(s));
    this._offAuth = emitter.on('auth', () => this.refresh());
  },

  onUnload() {
    this._offState && this._offState();
    this._offAuth && this._offAuth();
  },

  onShow() {
    this.refresh();
  },

  refresh() {
    const user = im.currentUser();
    this.setData({
      user,
      avatarChar: user && user.name ? user.name[0] : '',
      avatarCls: format.avatarClass(user ? user.uid : 'x'),
      server: config.server.host + ' (TCP:' + config.server.tcpPort + ' / HTTP:' + config.server.httpBase.replace(/^https?:\/\//, '').split(':')[1] + ')',
    });
    this.updateState(im.isConnected() ? 'connected' : 'disconnected');
  },

  updateState(s) {
    const map = {
      connected: '已连接',
      connecting: '连接中...',
      disconnected: '未连接',
    };
    this.setData({ connected: s === 'connected', stateText: map[s] || s });
  },

  clearMessages() {
    wx.showModal({
      title: '清空聊天记录',
      content: '将删除本机所有会话与消息，不影响服务端。确定继续吗？',
      confirmColor: '#f5523d',
      success: (res) => {
        if (!res.confirm) return;
        const user = store.getUser();
        store.clearAll();
        if (user) store.setUser(user);
        this.refresh();
        wx.showToast({ title: '已清空', icon: 'success' });
      },
    });
  },

  logout() {
    wx.showModal({
      title: '退出登录',
      content: '确定退出当前账号吗？',
      success: (res) => {
        if (!res.confirm) return;
        im.logout();
        wx.reLaunch({ url: '/pages/login/login' });
      },
    });
  },
})
