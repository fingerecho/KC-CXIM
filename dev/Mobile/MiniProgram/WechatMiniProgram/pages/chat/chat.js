const im = require('../../services/im');
const fileService = require('../../services/file');
const config = require('../../utils/config');
const emitter = require('../../utils/emitter');
const format = require('../../utils/format');

const TIME_GAP = 5 * 60 * 1000;
const T = config.tcpTypes;

function formatSize(bytes) {
  const n = Number(bytes) || 0;
  if (n < 1024) return n + ' B';
  if (n < 1024 * 1024) return (n / 1024).toFixed(1) + ' KB';
  return (n / 1024 / 1024).toFixed(1) + ' MB';
}

Page({
  data: {
    peerId: '',
    isGroup: false,
    display: [],
    input: '',
    scrollInto: '',
    connected: false,
    groupInfo: null,
  },

  onLoad(options) {
    this.peerId = decodeURIComponent(options.peerId || '');
    this.isGroup = options.isGroup === '1' || im.isGroupPeer(this.peerId);
    this._thumbs = {}; // fileId -> 本地临时路径，避免重复下载
    const info = im.getPeerInfo(this.peerId);
    wx.setNavigationBarTitle({ title: info.name });

    this._offMsg = emitter.on('msg', (m) => {
      if (m.peerId === this.peerId) this.reload();
    });
    this._offUpd = emitter.on('msg-update', (u) => {
      if (u.peerId === this.peerId) this.reload();
    });
    this._offState = emitter.on('state', (s) => {
      this.setData({ connected: s === 'connected' });
    });
    this._offHistory = emitter.on('history', (h) => {
      if (h.peerId === this.peerId) this.reload();
    });
    this._offGroupInfo = emitter.on('group-info', (g) => {
      if (this.isGroup && g.groupId === im.groupIdFromPeer(this.peerId)) {
        this.updateGroupInfo();
        this.reload();
      }
    });

    this.setData({
      peerId: this.peerId,
      isGroup: this.isGroup,
      connected: im.isConnected(),
    });
    this.updateGroupInfo();
    this.reload();
    wx.setNavigationBarTitle({ title: info.name });

    if (im.isConnected()) {
      im.requestHistory(this.peerId).catch(() => {});
    }
    if (this.isGroup) {
      im.refreshGroupInfo(im.groupIdFromPeer(this.peerId), true).catch(() => {});
    }
  },

  onShow() {
    im.setViewing(this.peerId);
  },

  onHide() {
    im.setViewing(null);
  },

  onUnload() {
    im.setViewing(null);
    this._offMsg && this._offMsg();
    this._offUpd && this._offUpd();
    this._offState && this._offState();
    this._offHistory && this._offHistory();
    this._offGroupInfo && this._offGroupInfo();
  },

  updateGroupInfo() {
    if (!this.isGroup) return;
    const info = im.getPeerInfo(this.peerId);
    this.setData({
      groupInfo: info.data ? {
        name: info.data.name,
        memberCount: info.data.memberCount || 0,
        announcement: info.data.announcement || '',
      } : null,
    });
    if (info.name) wx.setNavigationBarTitle({ title: info.name });
  },

  goGroupInfo() {
    if (!this.isGroup) return;
    wx.navigateTo({
      url: '/pages/group-info/group-info?groupId=' + im.groupIdFromPeer(this.peerId),
    });
  },

  /** 组装渲染列表：插入时间分隔条，解析媒体消息 */
  reload() {
    const msgs = im.getMessages(this.peerId);
    const display = [];
    let lastTs = 0;
    msgs.forEach((m) => {
      if (lastTs === 0 || m.ts - lastTs > TIME_GAP) {
        display.push({ isTime: true, text: format.formatMsgTime(m.ts) });
      }
      const type = m.msgType || T.TEXT;
      const media = (type === T.IMAGE || type === T.FILE) ? im.parseMedia(m.content) : null;
      const fileName = m.fileName || (media && media.name) || '文件';
      display.push({
        isTime: false,
        key: m.id,
        msgId: m.msgId,
        direction: m.direction,
        content: m.content,
        msgType: type,
        status: m.status,
        senderName: m.senderName || '',
        senderChar: (m.senderName || '群')[0],
        fileName,
        fileSizeText: formatSize(m.fileSize || (media && media.size)),
        media,
        thumbPath: (media && this._thumbs[media.file_id]) || '',
      });
      lastTs = m.ts;
    });
    this.setData({
      display,
      scrollInto: display.length ? 'anchor-' + (display.length - 1) : '',
    });
    this._downloadThumbs(display);
  },

  /** 图片消息自动生成缩略图（下载换取入口 302 到本地临时文件） */
  _downloadThumbs(display) {
    display.forEach((item) => {
      if (item.msgType !== T.IMAGE || !item.media || !item.media.url || item.thumbPath) return;
      const fileId = item.media.file_id;
      if (this._thumbs[fileId]) return;
      this._thumbs[fileId] = 'loading';
      fileService.download(item.media.url).then((path) => {
        this._thumbs[fileId] = path;
        const idx = this.data.display.findIndex((x) => x.media && x.media.file_id === fileId);
        if (idx !== -1) this.setData({ ['display[' + idx + '].thumbPath']: path });
      }).catch(() => {
        this._thumbs[fileId] = ''; // 失败时保留占位，点击后可重试
      });
    });
  },

  onInput(e) {
    this.setData({ input: e.detail.value });
  },

  onSend() {
    const content = this.data.input.trim();
    if (!content) return;
    this.setData({ input: '' });
    im.sendMessage(this.peerId, content, T.TEXT);
  },

  onRetry(e) {
    const msgId = e.currentTarget.dataset.msgid;
    im.resendFailed(this.peerId, msgId);
  },

  onBlur() {
    this.reload();
  },

  // ---------- 文件发送 ----------

  onAdd() {
    wx.showActionSheet({
      itemList: ['发送图片', '发送文件'],
      success: (res) => {
        if (res.tapIndex === 0) this.sendMedia('image');
        if (res.tapIndex === 1) this.sendMedia('file');
      },
      fail() { /* 用户取消 */ },
    });
  },

  async sendMedia(kind) {
    wx.showLoading({ title: '上传中...', mask: true });
    try {
      const meta = await fileService.chooseAndUpload(kind);
      im.sendFileMessage(this.peerId, meta.fileId, meta, kind === 'image');
      wx.hideLoading();
    } catch (e) {
      wx.hideLoading();
      const msg = e.message || '发送失败';
      if (msg.indexOf('cancel') === -1) {
        wx.showToast({ title: msg, icon: 'none', duration: 2500 });
      }
    }
  },

  // ---------- 文件打开 ----------

  onOpenFile(e) {
    const { msgid } = e.currentTarget.dataset;
    const item = this.data.display.find((x) => x.msgId === msgid);
    if (!item || !item.media) return;
    const media = item.media;
    if (item.msgType === T.IMAGE && this._thumbs[media.file_id] && this._thumbs[media.file_id] !== 'loading') {
      wx.previewImage({ urls: [this._thumbs[media.file_id]] });
      return;
    }
    wx.showLoading({ title: '下载中...', mask: true });
    fileService.download(media.url).then((path) => {
      wx.hideLoading();
      this._thumbs[media.file_id] = path;
      fileService.open(path, media.name || item.fileName);
    }).catch((err) => {
      wx.hideLoading();
      wx.showToast({ title: err.message || '下载失败', icon: 'none' });
    });
  },
});
