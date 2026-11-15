/**
 * 本地数据仓库（基于 wx.setStorageSync）
 *
 * 持久化：会话列表、每个会话最近的消息、未读数、当前用户与 token
 */
const STORE_KEY = 'chengxin_store_v1';
const MAX_MESSAGES_PER_SESSION = 200;

function defaultData() {
  return {
    user: null,      // { uid, name, token }
    sessions: {},    // peerId -> { id, name, lastMsg: { content, ts, direction }, ts }
    messages: {},    // peerId -> [ { id, msgId, peerId, direction, content, ts, status } ]（按时间升序）
    unread: {},      // peerId -> number
    contacts: [],    // [{ id, name }]
  };
}

let data = defaultData();
let saveTimer = null;

function load() {
  try {
    const raw = wx.getStorageSync(STORE_KEY);
    if (raw && typeof raw === 'object') {
      data = Object.assign(defaultData(), raw);
    }
  } catch (e) {
    console.error('[store] 读取本地存储失败', e);
  }
}

function save() {
  if (saveTimer) return;
  saveTimer = setTimeout(() => {
    saveTimer = null;
    try {
      wx.setStorageSync(STORE_KEY, data);
    } catch (e) {
      console.error('[store] 写入本地存储失败', e);
    }
  }, 200);
}

function get() {
  return data;
}

function setUser(user) {
  data.user = user;
  save();
}

function getUser() {
  return data.user;
}

function getSession(peerId) {
  return data.sessions[peerId] || null;
}

function touchSession(peerId, name, lastMsg) {
  const s = data.sessions[peerId] || { id: peerId, name: name || peerId, lastMsg: null, ts: 0 };
  if (name) s.name = name;
  if (lastMsg) s.lastMsg = lastMsg;
  s.ts = lastMsg ? lastMsg.ts : Date.now();
  data.sessions[peerId] = s;
  save();
  return s;
}

function renameSession(peerId, name) {
  if (data.sessions[peerId]) {
    data.sessions[peerId].name = name;
    save();
  }
}

function getMessages(peerId) {
  return data.messages[peerId] || [];
}

function appendMessage(msg) {
  const list = data.messages[msg.peerId] || (data.messages[msg.peerId] = []);
  list.push(msg);
  if (list.length > MAX_MESSAGES_PER_SESSION) {
    list.splice(0, list.length - MAX_MESSAGES_PER_SESSION);
  }
  save();
}

/** 消息落库后按 id 更新状态（如 sending -> sent / failed） */
function updateMessage(peerId, msgId, patch) {
  const list = data.messages[peerId] || [];
  const m = list.find((x) => x.msgId === msgId || x.id === msgId);
  if (m) Object.assign(m, patch);
  save();
}

/** 整体替换某个会话的消息列表（历史消息合并时使用），并按 seq/ts 排序 */
function replaceMessages(peerId, list) {
  data.messages[peerId] = (list || []).slice(0, MAX_MESSAGES_PER_SESSION * 2);
  save();
}

/** 删除某条消息（重发失败消息前先移除旧气泡） */
function removeMessage(peerId, msgId) {
  const list = data.messages[peerId];
  if (!list) return;
  const idx = list.findIndex((x) => x.msgId === msgId);
  if (idx !== -1) list.splice(idx, 1);
  save();
}

function getUnread(peerId) {
  return data.unread[peerId] || 0;
}

function incrUnread(peerId) {
  data.unread[peerId] = (data.unread[peerId] || 0) + 1;
  save();
}

function clearUnread(peerId) {
  delete data.unread[peerId];
  save();
}

function setContacts(contacts) {
  data.contacts = contacts || [];
  save();
}

function clearAll() {
  data = defaultData();
  try { wx.removeStorageSync(STORE_KEY); } catch (e) { /* ignore */ }
}

module.exports = {
  load,
  get,
  getUser,
  setUser,
  getSession,
  touchSession,
  renameSession,
  getMessages,
  appendMessage,
  updateMessage,
  replaceMessages,
  removeMessage,
  getUnread,
  incrUnread,
  clearUnread,
  setContacts,
  clearAll,
};
