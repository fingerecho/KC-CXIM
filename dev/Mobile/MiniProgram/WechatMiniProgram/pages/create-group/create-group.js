const im = require('../../services/im');
const api = require('../../services/api');
const store = require('../../services/store');
const format = require('../../utils/format');

Page({
  data: {
    mode: 'create', // create | invite
    groupId: '',
    groupName: '',
    friends: [],
    selected: [],
    submitting: false,
  },

  onLoad(options) {
    const mode = options.mode === 'invite' ? 'invite' : 'create';
    const groupId = options.groupId || '';
    this.setData({ mode, groupId });
    wx.setNavigationBarTitle({ title: mode === 'invite' ? '邀请成员' : '发起群聊' });
    this.loadFriends();
  },

  loadFriends() {
    let contacts = (store.get().contacts || []).slice();
    if (this.data.mode === 'invite') {
      const members = store.getGroupMembers(this.data.groupId) || [];
      const inGroup = {};
      members.forEach((m) => { inGroup[String(m.userId)] = true; });
      contacts = contacts.filter((c) => !inGroup[String(c.id)]);
    }
    this.setData({
      friends: contacts.map((c) => ({
        id: c.id,
        name: c.name,
        avatarChar: (c.name || c.id)[0],
        avatarCls: format.avatarClass(c.id),
        checked: this.data.selected.indexOf(String(c.id)) !== -1,
      })),
    });
  },

  onNameInput(e) {
    this.setData({ groupName: e.detail.value });
  },

  onToggle(e) {
    const id = String(e.currentTarget.dataset.id);
    const selected = this.data.selected.slice();
    const idx = selected.indexOf(id);
    if (idx !== -1) selected.splice(idx, 1);
    else selected.push(id);
    const friends = this.data.friends.map((f) => Object.assign({}, f, {
      checked: selected.indexOf(String(f.id)) !== -1,
    }));
    this.setData({ selected, friends });
  },

  async onSubmit() {
    if (this.data.submitting) return;
    const { mode, groupId, groupName, selected } = this.data;
    if (mode === 'create' && !groupName.trim()) {
      wx.showToast({ title: '请输入群名称', icon: 'none' });
      return;
    }
    if (selected.length === 0) {
      wx.showToast({ title: '请至少选择一位好友', icon: 'none' });
      return;
    }
    this.setData({ submitting: true });
    wx.showLoading({ title: '处理中...', mask: true });
    try {
      if (mode === 'create') {
        const group = await api.createGroup(groupName.trim(), selected.map(Number));
        wx.hideLoading();
        await im.refreshGroupsCache();
        wx.showToast({ title: '群聊已创建', icon: 'success' });
        wx.redirectTo({
          url: '/pages/chat/chat?peerId=' + im.toPeerId(group.groupId, true)
            + '&name=' + encodeURIComponent(group.name) + '&isGroup=1',
        });
      } else {
        await api.inviteGroupMembers(groupId, selected.map(Number));
        wx.hideLoading();
        await im.refreshGroupInfo(groupId, true);
        wx.showToast({ title: '邀请成功', icon: 'success' });
        setTimeout(() => wx.navigateBack(), 600);
      }
    } catch (err) {
      wx.hideLoading();
      wx.showToast({ title: err.message || '操作失败', icon: 'none', duration: 2500 });
      this.setData({ submitting: false });
    }
  },
});
