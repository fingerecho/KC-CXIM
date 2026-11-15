const im = require('../../services/im');

Page({
  data: {
    username: '',
    password: '',
    confirmPwd: '',
    mode: 'login', // login | register
    loading: false,
    stateText: '',
  },

  onLoad() {
    // 已登录则直接进入主界面
    if (im.currentUser()) {
      wx.switchTab({ url: '/pages/chat-list/chat-list' });
    }
  },

  onUsername(e) {
    this.setData({ username: e.detail.value.trim() });
  },

  onPassword(e) {
    this.setData({ password: e.detail.value });
  },

  onConfirmPwd(e) {
    this.setData({ confirmPwd: e.detail.value });
  },

  switchMode() {
    this.setData({
      mode: this.data.mode === 'login' ? 'register' : 'login',
      stateText: '',
    });
  },

  validate() {
    const { username, password, confirmPwd, mode } = this.data;
    if (!username) {
      wx.showToast({ title: '请输入用户名', icon: 'none' });
      return false;
    }
    if (username.length > 64) {
      wx.showToast({ title: '用户名过长（<= 64）', icon: 'none' });
      return false;
    }
    if (password.length < 6 || password.length > 72) {
      wx.showToast({ title: '密码长度需为 6~72 位', icon: 'none' });
      return false;
    }
    if (mode === 'register' && password !== confirmPwd) {
      wx.showToast({ title: '两次输入的密码不一致', icon: 'none' });
      return false;
    }
    return true;
  },

  async onSubmit() {
    const { username, password, loading, mode } = this.data;
    if (loading) return;
    if (!this.validate()) return;

    this.setData({ loading: true, stateText: mode === 'login' ? '正在登录...' : '正在注册...' });

    try {
      if (mode === 'register') {
        await im.register(username, password);
        wx.showToast({ title: '注册成功，自动登录', icon: 'none' });
      }
      await im.login(username, password);
      this.setData({ stateText: '登录成功' });
      wx.reLaunch({ url: '/pages/chat-list/chat-list' });
    } catch (e) {
      wx.showToast({ title: e.message || '操作失败', icon: 'none', duration: 2500 });
      this.setData({ stateText: '' });
    } finally {
      this.setData({ loading: false });
    }
  },
})
