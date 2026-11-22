const im = require('../../services/im');
const api = require('../../services/api');
const emitter = require('../../utils/emitter');
const format = require('../../utils/format');

const ROLE_TEXT = { 1: '群主', 2: '管理员', 3: '成员' };

Page({
  data: {
    groupId: '',
    group: null,
    members: [],
    myRole: 0,
    isOwner: false,
    canManage: false,
  },

  onLoad(options) {
    this.groupId = options.groupId || '';
    this.setData({ groupId: this.groupId });
    this._offInfo = emitter.on('group-info', (g) => {
      if (String(g.groupId) === String(this.groupId)) this.load();
    });
    this._offChg = emitter.on('group-member-changed', () => this.load());
    this._offDismiss = emitter.on('group-dismissed', (g) => {
      if (String(g.groupId) === String(this.groupId)) {
        wx.showToast({ title: '群聊已解散', icon: 'none' });
        wx.navigateBack();
      }
    });
  },

  onUnload() {
    this._offInfo && this._offInfo();
    this._offChg && this._offChg();
    this._offDismiss && this._offDismiss();
  },

  onShow() {
    this.load();
  },

  load() {
    return im.refreshGroupInfo(this.groupId, true).then((data) => {
      const group = data.group || {};
      const myRole = group.myRole || 0;
      const members = (data.members || []).map((m) => ({
        userId: m.userId,
        name: m.groupNickname || m.nickname || m.username || ('用户 ' + m.userId),
        role: m.role,
        roleText: ROLE_TEXT[m.role] || '成员',
        avatarCls: format.avatarClass(m.userId),
        isMe: false,
      }));
      const me = im.currentUser();
      members.forEach((m) => { m.isMe = me && String(m.userId) === String(me.uid); });
      // 群主排在最前
      members.sort((a, b) => (a.role || 9) - (b.role || 9));
      this.setData({
        group,
        members,
        myRole,
        isOwner: myRole === 1,
        canManage: myRole === 1 || myRole === 2,
      });
      wx.setNavigationBarTitle({ title: group.name || '群信息' });
      return data;
    }).catch(() => {});
  },

  openChat() {
    wx.navigateTo({
      url: '/pages/chat/chat?peerId=' + im.toPeerId(this.groupId, true)
        + '&name=' + encodeURIComponent((this.data.group || {}).name || '群聊') + '&isGroup=1',
    });
  },

  invite() {
    wx.navigateTo({
      url: '/pages/create-group/create-group?mode=invite&groupId=' + this.groupId,
    });
  },

  // ---------- 群资料 ----------

  editField(field, title, placeholder) {
    if (!this.data.canManage) {
      wx.showToast({ title: '仅群主/管理员可修改', icon: 'none' });
      return;
    }
    wx.showModal({
      title,
      content: '',
      editable: true,
      placeholderText: placeholder,
      success: async (res) => {
        if (!res.confirm) return;
        const value = String(res.content || '');
        // 服务端约定「空串=不改该字段」，提交空串不会真正清空，需明确提示用户
        if (!value.trim()) {
          wx.showToast({ title: '内容为空，服务端不支持清空该字段', icon: 'none', duration: 2500 });
          return;
        }
        try {
          await api.updateGroup(this.groupId, { [field]: value });
          wx.showToast({ title: '已保存', icon: 'success' });
          this.load();
          im.refreshGroupsCache();
        } catch (e) {
          wx.showToast({ title: e.message || '保存失败', icon: 'none' });
        }
      },
    });
  },

  editName() { this.editField('name', '修改群名称', '输入新群名'); },
  editAnnouncement() { this.editField('announcement', '修改群公告', '输入群公告'); },
  editIntro() { this.editField('intro', '修改群简介', '输入群简介'); },

  // ---------- 成员操作 ----------

  onMemberTap(e) {
    const userId = String(e.currentTarget.dataset.userid);
    const member = this.data.members.find((m) => String(m.userId) === userId);
    if (!member || !this.data.canManage) return;
    if (member.role === 1) {
      wx.showToast({ title: '群主不可被操作', icon: 'none' });
      return;
    }

    const items = [];
    if (this.data.isOwner) {
      items.push(member.role === 2 ? '取消管理员' : '设为管理员');
      items.push('转让群主');
    }
    if (this.data.isOwner || (this.data.myRole === 2 && member.role === 3)) {
      items.push('移出群聊');
    }
    if (items.length === 0) return;

    wx.showActionSheet({
      itemList: items,
      success: (res) => {
        const action = items[res.tapIndex];
        if (action === '设为管理员' || action === '取消管理员') {
          this.setAdmin(userId, action === '设为管理员');
        } else if (action === '转让群主') {
          this.transfer(userId, member.name);
        } else if (action === '移出群聊') {
          this.remove(userId, member.name);
        }
      },
      fail() { /* 用户取消 */ },
    });
  },

  async setAdmin(userId, admin) {
    try {
      await api.setGroupAdmin(this.groupId, Number(userId), admin);
      wx.showToast({ title: admin ? '已设为管理员' : '已取消管理员', icon: 'success' });
      this.load();
    } catch (e) {
      wx.showToast({ title: e.message || '操作失败', icon: 'none' });
    }
  },

  remove(userId, name) {
    wx.showModal({
      title: '移出成员',
      content: '确定将「' + name + '」移出群聊吗？',
      confirmColor: '#f5523d',
      success: async (res) => {
        if (!res.confirm) return;
        try {
          await api.removeGroupMember(this.groupId, Number(userId));
          wx.showToast({ title: '已移出', icon: 'success' });
          this.load();
        } catch (e) {
          wx.showToast({ title: e.message || '操作失败', icon: 'none' });
        }
      },
    });
  },

  transfer(userId, name) {
    wx.showModal({
      title: '转让群主',
      content: '确定将群主转让给「' + name + '」吗？转让后你将变为管理员。',
      confirmColor: '#f5523d',
      success: async (res) => {
        if (!res.confirm) return;
        try {
          await api.transferGroup(this.groupId, Number(userId));
          wx.showToast({ title: '已转让', icon: 'success' });
          this.load();
          im.refreshGroupsCache();
        } catch (e) {
          wx.showToast({ title: e.message || '操作失败', icon: 'none' });
        }
      },
    });
  },

  // ---------- 退群 / 解散 ----------

  quit() {
    wx.showModal({
      title: '退出群聊',
      content: '退群后将无法接收该群消息，确定退出吗？',
      confirmColor: '#f5523d',
      success: async (res) => {
        if (!res.confirm) return;
        try {
          await api.quitGroup(this.groupId);
          wx.showToast({ title: '已退群', icon: 'success' });
          wx.reLaunch({ url: '/pages/chat-list/chat-list' });
        } catch (e) {
          wx.showToast({ title: e.message || '操作失败', icon: 'none' });
        }
      },
    });
  },

  dismiss() {
    wx.showModal({
      title: '解散群聊',
      content: '解散后所有成员将无法再收发消息，且不可恢复。确定解散吗？',
      confirmColor: '#f5523d',
      success: async (res) => {
        if (!res.confirm) return;
        try {
          await api.dismissGroup(this.groupId);
          wx.showToast({ title: '已解散', icon: 'success' });
          wx.reLaunch({ url: '/pages/chat-list/chat-list' });
        } catch (e) {
          wx.showToast({ title: e.message || '操作失败', icon: 'none' });
        }
      },
    });
  },
});
