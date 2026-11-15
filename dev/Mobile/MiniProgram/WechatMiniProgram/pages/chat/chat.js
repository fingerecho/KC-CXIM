const im = require('../../services/im');
const emitter = require('../../utils/emitter');
const format = require('../../utils/format');

const TIME_GAP = 5 * 60 * 1000; // 超过 5 分钟显示时间分隔

Page({
  data: {
    peerId: '',
    display: [],
    input: '',
    scrollInto: '',
    connected: false,
  },

  onLoad(options) {
    this.peerId = decodeURIComponent(options.peerId || '');
    const name = options.name ? decodeURIComponent(options.name) : this.peerId;
    wx.setNavigationBarTitle({ title: name });

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

    this.setData({ peerId: this.peerId, connected: im.isConnected() });
    this.reload();
    // 拉取服务端历史消息（TCP type=110/111），合并后重新渲染
    if (im.isConnected()) {
      im.requestHistory(this.peerId).catch(() => {});
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
  },

  /** 组装渲染列表：插入时间分隔条 */
  reload() {
    const msgs = im.getMessages(this.peerId);
    const display = [];
    let lastTs = 0;
    msgs.forEach((m) => {
      if (lastTs === 0 || m.ts - lastTs > TIME_GAP) {
        display.push({ isTime: true, text: format.formatMsgTime(m.ts) });
      }
      display.push({
        isTime: false,
        key: m.id,
        msgId: m.msgId,
        direction: m.direction,
        content: m.content,
        status: m.status,
        avatarChar: (im.currentUser() || {}).name ? (im.currentUser().name[0]) : '我',
      });
      lastTs = m.ts;
    });
    this.setData({
      display,
      scrollInto: display.length ? 'anchor-' + (display.length - 1) : '',
    });
  },

  onInput(e) {
    this.setData({ input: e.detail.value });
  },

  onSend() {
    const content = this.data.input.trim();
    if (!content) return;
    this.setData({ input: '' });
    im.sendMessage(this.peerId, content);
  },

  onRetry(e) {
    const msgId = e.currentTarget.dataset.msgid;
    im.resendFailed(this.peerId, msgId);
  },

  onBlur() {
    // 保持在底部
    this.reload();
  },
});
