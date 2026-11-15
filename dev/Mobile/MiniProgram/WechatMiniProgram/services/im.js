/**
 * IM 业务层（对接 KC-CXIM IMS 真实协议）
 *
 * 流程：
 * - 注册/登录走 HTTP（/api/login 拿 JWT），然后建立 TCP 长连接并发 type=100 登录（content=JWT）
 * - 断线重连后自动重新发送 TCP 登录
 * - 发消息：type=1/2，msg_id 为客户端临时消息号；服务端回 SendAck=113 确认
 *   （113 的 msg_id 是服务端消息 ID，不回显客户端临时号，故按 FIFO 对应最早的待确认消息，
 *    并用 conversation_id 建立 conversation_id -> peerId 映射，供回执与历史消息使用）
 * - 收消息：type=1/2 下行，收到后回 type=112 投递回执（服务端借此清理离线队列）
 * - 历史消息：type=110 请求，type=111 响应（content 为 JSON 数组字符串，二次解析）
 * - 好友事件：130/131/132，content 为 JSON 字符串，二次解析后广播
 *
 * 事件（utils/emitter）：
 * - 'state' / 'auth' / 'msg' / 'msg-update' / 'sessions'
 * - 'history'        : 历史消息合并完成 { peerId }
 * - 'friend-request' : 收到好友申请（type=130）
 * - 'friend-result'  : 申请通过/拒绝（type=131）
 * - 'friends-changed': 好友列表变化（type=132）
 */
const config = require('../utils/config');
const socket = require('../utils/socket');
const emitter = require('../utils/emitter');
const store = require('./store');
const api = require('./api');

const T = config.tcpTypes;

let msgIdCounter = Date.now() % 1000000;
const sendingQueue = [];     // FIFO：等待 SendAck 的消息 { peerId, msgId(临时号), timer }
const historyQueue = [];     // FIFO：等待 HistoryResp 的请求 { peerId, resolve }
const convMap = {};          // conversation_id -> peerId
let tcpLoginPending = null;  // { resolve, reject, timer }
let tcpLoggedIn = false;     // 当前连接是否已完成 TCP 登录

function nextMsgId() {
  msgIdCounter = (msgIdCounter + 1) % 1000000000;
  return Date.now().toString(36) + msgIdCounter.toString(36);
}

function genLocalId() {
  return 'm' + Date.now().toString(36) + Math.random().toString(36).slice(2, 8);
}

function friendName(uid) {
  const contacts = store.get().contacts || [];
  const c = contacts.find((x) => String(x.id) === String(uid));
  return c ? c.name : '用户 ' + uid;
}

// ---------- TCP 登录 ----------

function tcpLogin() {
  const user = store.getUser();
  if (!user || !user.token) {
    return Promise.reject(new Error('本地无 token，请重新登录'));
  }
  if (tcpLoginPending) return tcpLoginPending.promise;

  let resolveFn;
  let rejectFn;
  const promise = new Promise((resolve, reject) => {
    resolveFn = resolve;
    rejectFn = reject;
  });
  tcpLoginPending = { resolve: resolveFn, reject: rejectFn, promise };

  tcpLoginPending.timer = setTimeout(() => {
    if (tcpLoginPending) {
      const p = tcpLoginPending;
      tcpLoginPending = null;
      p.reject(new Error('TCP 登录超时'));
    }
  }, 10000);

  socket.send({ from: 0, to: 0, type: T.LOGIN, msg_id: 0, ts: 0, conversation_id: 0, seq: 0, content: user.token })
    .catch((err) => {
      clearTimeout(tcpLoginPending && tcpLoginPending.timer);
      tcpLoginPending = null;
      rejectFn(err);
    });

  return promise;
}

// ---------- 帧分发 ----------

function handleFrame(frame) {
  if (!frame || typeof frame !== 'object') return;
  const type = frame.type;

  switch (type) {
    case T.LOGIN_ACK: // 101
      tcpLoggedIn = true;
      if (tcpLoginPending) {
        const p = tcpLoginPending;
        tcpLoginPending = null;
        clearTimeout(p.timer);
        p.resolve(frame);
        emitter.emit('auth', { ok: true, user: store.getUser() });
      }
      break;

    case T.ERROR: // 102
      if (tcpLoginPending) {
        const p = tcpLoginPending;
        tcpLoginPending = null;
        clearTimeout(p.timer);
        p.reject(new Error(frame.content || 'TCP 登录失败'));
      } else {
        console.warn('[im] 服务端错误:', frame.content);
        emitter.emit('tcp-error', frame.content);
      }
      break;

    case T.SEND_ACK: // 113
      handleSendAck(frame);
      break;

    case T.TEXT: // 1
    case T.IMAGE: // 2
      handleIncoming(frame);
      break;

    case T.HISTORY_RESP: // 111
      handleHistoryResp(frame);
      break;

    case T.FRIEND_REQUEST: // 130
    case T.FRIEND_REQUEST_RESULT: // 131
    case T.FRIEND_LIST_CHANGED: // 132
      handleFriendEvent(frame);
      break;

    default:
      break;
  }
}

// ---------- 消息发送 ----------

function sendMessage(peerId, content, msgType) {
  const tempMsgId = nextMsgId();
  const msg = {
    id: genLocalId(),
    msgId: tempMsgId,
    peerId,
    direction: 'out',
    content,
    ts: Date.now(),
    status: 'sending',
  };

  store.touchSession(peerId, (store.getSession(peerId) || {}).name || friendName(peerId), { content, ts: msg.ts, direction: 'out' });
  store.appendMessage(msg);

  socket.send({
    from: 0,
    to: Number(peerId) || 0,
    type: msgType || T.TEXT,
    msg_id: tempMsgId,
    ts: msg.ts,
    content,
  }).catch(() => {
    store.updateMessage(peerId, tempMsgId, { status: 'failed' });
    emitter.emit('msg-update', { peerId, msgId: tempMsgId, status: 'failed' });
  });

  // 15s 未收到 SendAck 视为失败
  const timer = setTimeout(() => {
    const idx = sendingQueue.findIndex((x) => x.msgId === tempMsgId);
    if (idx !== -1) {
      sendingQueue.splice(idx, 1);
      store.updateMessage(peerId, tempMsgId, { status: 'failed' });
      emitter.emit('msg-update', { peerId, msgId: tempMsgId, status: 'failed' });
    }
  }, 15000);
  sendingQueue.push({ peerId, msgId: tempMsgId, timer });

  emitter.emit('msg', msg);
  emitter.emit('sessions');
  return msg;
}

function handleSendAck(frame) {
  const rec = sendingQueue.shift();
  if (!rec) return;
  clearTimeout(rec.timer);

  // 记录 conversation_id -> peerId 映射
  if (frame.conversation_id) convMap[frame.conversation_id] = rec.peerId;

  const status = frame.content === 'duplicate' ? 'sent' : 'sent';
  store.updateMessage(rec.peerId, rec.msgId, {
    status,
    serverMsgId: frame.msg_id,
    conversationId: frame.conversation_id,
    seq: frame.seq,
    ts: frame.ts || Date.now(),
  });
  emitter.emit('msg-update', { peerId: rec.peerId, msgId: rec.msgId, status });
}

function resendFailed(peerId, msgId) {
  const m = store.getMessages(peerId).find((x) => x.msgId === msgId);
  if (!m || m.status !== 'failed') return;
  // 先移除失败的旧气泡，再走正常发送链路（新的临时 msg_id）
  store.removeMessage(peerId, msgId);
  emitter.emit('sessions');
  sendMessage(peerId, m.content, m.msgType || T.TEXT);
}

// ---------- 消息接收 ----------

function handleIncoming(frame) {
  const me = store.getUser();
  // 统一转字符串：服务端 from/to 是数字，而页面参数与本地存储的 peerId 均为字符串，
  // 不转会导致聊天页严格比较 (m.peerId === this.peerId) 失败、气泡不刷新
  const from = String(frame.from || '');
  if (!from || from === '0' || (me && Number(from) === Number(me.uid))) return;

  if (frame.conversation_id) convMap[frame.conversation_id] = from;

  const msg = {
    id: genLocalId(),
    msgId: frame.msg_id,
    peerId: from,
    direction: 'in',
    content: frame.content,
    ts: frame.ts || Date.now(),
    status: 'received',
    conversationId: frame.conversation_id,
    seq: frame.seq,
    msgType: frame.type,
  };

  const known = store.getSession(from);
  store.touchSession(from, (known && known.name) || friendName(from), { content: msg.content, ts: msg.ts, direction: 'in' });
  store.appendMessage(msg);

  if (viewingPeerId !== from) store.incrUnread(from);

  // 投递回执 type=112（服务端借此删除离线消息）
  socket.send({
    from: 0,
    to: 0,
    type: T.ACK,
    conversation_id: frame.conversation_id,
    seq: frame.seq,
    content: 'delivered',
  }).catch(() => {});

  emitter.emit('msg', msg);
  emitter.emit('sessions');
}

// ---------- 历史消息 ----------

/** 拉取与 peer 的历史消息（seq=0 从最新开始） */
function requestHistory(peerId) {
  return new Promise((resolve, reject) => {
    historyQueue.push({ peerId, resolve, reject });
    socket.send({
      from: 0,
      to: Number(peerId) || 0,
      type: T.HISTORY_REQ,
      seq: 0,
      content: String(config.history.pageSize),
    }).catch((err) => {
      const idx = historyQueue.findIndex((x) => x.resolve === resolve);
      if (idx !== -1) historyQueue.splice(idx, 1);
      reject(err);
    });
    // 10s 超时
    setTimeout(() => {
      const idx = historyQueue.findIndex((x) => x.resolve === resolve);
      if (idx !== -1) {
        historyQueue.splice(idx, 1);
        reject(new Error('历史消息请求超时'));
      }
    }, 10000);
  });
}

function handleHistoryResp(frame) {
  const rec = historyQueue.shift();
  let peerId = rec ? rec.peerId : null;
  if (frame.conversation_id && convMap[frame.conversation_id]) peerId = convMap[frame.conversation_id];
  if (!peerId) return;

  let list = [];
  try {
    // content 本身是 JSON 数组字符串，需要二次解析
    list = typeof frame.content === 'string' ? JSON.parse(frame.content) : frame.content;
  } catch (e) {
    console.error('[im] 历史消息解析失败', e);
    if (rec) rec.resolve([]);
    return;
  }

  const me = store.getUser();
  const existing = store.getMessages(peerId);
  const knownIds = {};
  existing.forEach((m) => {
    if (m.serverMsgId !== undefined) knownIds[m.serverMsgId] = true;
    if (m.direction === 'in' && m.msgId !== undefined) knownIds[m.msgId] = true;
  });

  const merged = existing.slice();
  list.forEach((item) => {
    if (knownIds[item.msg_id]) return;
    knownIds[item.msg_id] = true;
    if (item.conversation_id) convMap[item.conversation_id] = peerId;
    merged.push({
      id: genLocalId(),
      msgId: item.msg_id,
      serverMsgId: item.msg_id,
      peerId,
      direction: me && Number(item.from) === Number(me.uid) ? 'out' : 'in',
      content: item.content,
      ts: item.ts,
      status: 'sent',
      conversationId: item.conversation_id,
      seq: item.seq,
      msgType: item.type,
    });
  });

  merged.sort((a, b) => a.ts - b.ts);
  store.replaceMessages(peerId, merged);

  if (rec) rec.resolve(merged);
  emitter.emit('history', { peerId });
}

// ---------- 好友事件（130/131/132） ----------

function handleFriendEvent(frame) {
  let data = {};
  try {
    // content 是 JSON 字符串，需要二次解析
    data = typeof frame.content === 'string' ? JSON.parse(frame.content) : frame.content;
  } catch (e) {
    console.error('[im] 好友事件解析失败', e);
    return;
  }

  if (frame.type === T.FRIEND_REQUEST) {
    // 130：收到好友申请（event: new / pending），msg_id 复用为 request_id
    store.incrUnread('__friend_requests__');
    emitter.emit('friend-request', Object.assign({ requestId: frame.msg_id }, data));
    emitter.emit('sessions');
  } else if (frame.type === T.FRIEND_REQUEST_RESULT) {
    // 131：申请通过/拒绝
    emitter.emit('friend-result', Object.assign({ requestId: frame.msg_id }, data));
    refreshFriendsCache();
  } else if (frame.type === T.FRIEND_LIST_CHANGED) {
    // 132：好友列表变化（added/deleted/updated/blocked/unblocked）
    emitter.emit('friends-changed', data);
    refreshFriendsCache();
  }
}

// ---------- 好友（HTTP） ----------

/** 拉取好友列表并写入本地缓存（联系人/会话命名使用） */
function refreshFriendsCache() {
  return api.getFriends()
    .then((friends) => {
      store.setContacts(friends.map((f) => ({
        id: f.userId,
        name: f.remark || f.nickname || f.username || ('用户 ' + f.userId),
      })));
      emitter.emit('contacts', friends);
      return friends;
    })
    .catch((e) => {
      console.warn('[im] 拉取好友列表失败', e.message);
      return [];
    });
}

// ---------- 注册 / 登录 / 登出 ----------

function register(username, password) {
  return api.register(username, password);
}

function login(username, password) {
  return api.login(username, password)
    .then((data) => {
      store.setUser({ uid: data.userId, name: data.username, username: data.username, token: data.token });
      // 建立 TCP 长连接；TCP 登录由 'connected' 状态回调自动完成
      socket.connect().catch((e) => {
        console.warn('[im] TCP 连接失败，将自动重连:', e.message);
      });
      emitter.emit('auth', { ok: true, user: store.getUser() });
      return data;
    });
}

function logout() {
  socket.close();
  tcpLoggedIn = false;
  store.setUser(null);
  emitter.emit('auth', { ok: false, error: '已退出登录' });
}

// ---------- 启动 ----------

let viewingPeerId = null;

function boot() {
  emitter.on('frame', handleFrame);
  emitter.on('state', (state) => {
    if (state === 'disconnected') tcpLoggedIn = false;
    // 每次连上后自动完成 TCP 登录（首次登录与断线重连共用）
    if (state === 'connected') {
      tcpLogin()
        .then(() => refreshFriendsCache())
        .catch((e) => console.warn('[im] TCP 登录失败:', e.message));
    }
  });
  store.load();
  // 启动时若有历史登录，则自动连接
  if (store.getUser()) {
    socket.connect().catch((e) => {
      console.error('[im] 启动连接失败:', e.message, '（将由重连机制继续尝试）');
    });
  }
}

// ---------- 会话查询 ----------

function getSessions() {
  const sessions = store.get().sessions;
  return Object.values(sessions)
    .map((s) => Object.assign({}, s, { unread: store.getUnread(s.id) }))
    .sort((a, b) => b.ts - a.ts);
}

function getMessages(peerId) {
  return store.getMessages(peerId).slice();
}

function markRead(peerId) {
  store.clearUnread(peerId);
  emitter.emit('sessions');
}

function setViewing(peerId) {
  viewingPeerId = peerId;
  if (peerId) markRead(peerId);
}

module.exports = {
  boot,
  register,
  login,
  logout,
  sendMessage,
  resendFailed,
  requestHistory,
  refreshFriendsCache,
  getSessions,
  getMessages,
  markRead,
  setViewing,
  isConnected: () => socket.isConnected() && tcpLoggedIn,
  currentUser: () => store.getUser(),
};
