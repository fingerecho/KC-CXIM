/**
 * IM 业务层（对接 KC-CXIM IMS 真实协议）
 *
 * - HTTP 写事实（api.js），TCP 推事件（本文件 + socket.js）
 * - 单聊 peerId 为数字字符串；群聊 peerId 为 'g' + groupId（如 'g12'），
 *   这样会话列表、消息缓存、未读数都可以复用同一套逻辑
 * - 发消息：type=1/2/4，群聊额外带 is_group=true，msg_id 为客户端临时消息号
 * - 收消息：收到后回 type=112 投递回执（服务端借此清理离线队列）
 * - 历史消息：type=110 请求，type=111 响应（content 为 JSON 数组字符串，二次解析）
 * - 群事件：140~144；好友事件：130~132
 * - token 过期时自动走 /api/token/refresh 换新 token 再重连
 *
 * 事件（utils/emitter）：
 * - 'state' / 'auth' / 'msg' / 'msg-update' / 'sessions'
 * - 'history'          : { peerId }
 * - 'friend-request' / 'friend-result' / 'friends-changed'
 * - 'group-list'       : 群列表已刷新
 * - 'group-info'       : { groupId } 群详情已刷新
 * - 'group-dismissed'  : { groupId, name }
 * - 'group-request'    : 收到入群申请（type=143）
 * - 'group-result'     : 入群申请处理结果（type=144）
 * - 'tcp-error'        : 服务端错误文本
 */
const config = require('../utils/config');
const socket = require('../utils/socket');
const emitter = require('../utils/emitter');
const utf8 = require('../utils/utf8');
const store = require('./store');
const api = require('./api');

const T = config.tcpTypes;

let msgIdCounter = Date.now() % 1000000;
const sendingQueue = [];     // FIFO：等待 SendAck 的消息 { peerId, msgId(临时号), timer }
const historyQueue = [];     // FIFO：等待 HistoryResp 的请求 { peerId, isGroup, beforeSeq, resolve, reject, timer }
const convMap = {};          // conversation_id -> peerId
let tcpLoginPending = null;  // { resolve, reject, timer, promise }
let tcpLoggedIn = false;
let viewingPeerId = null;
let skipRefreshOnce = false; // 刚用密码登录拿到新 token，首次 TCP 登录无需 refresh
let refreshPromise = null;

// ---------- peerId 工具 ----------

function isGroupPeer(peerId) {
  return typeof peerId === 'string' && peerId.charAt(0) === 'g' && peerId.length > 1;
}
function groupIdFromPeer(peerId) {
  return isGroupPeer(peerId) ? peerId.slice(1) : '';
}
function toPeerId(targetId, group) {
  return group ? 'g' + targetId : String(targetId);
}

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

function memberName(groupId, uid) {
  const members = store.getGroupMembers(groupId);
  const m = members.find((x) => String(x.userId) === String(uid));
  if (m) return m.groupNickname || m.nickname || m.username || ('用户 ' + uid);
  return friendName(uid);
}

function sessionName(peerId, group) {
  if (group) {
    const g = store.getGroup(groupIdFromPeer(peerId));
    return g ? g.name : '群 ' + groupIdFromPeer(peerId);
  }
  return friendName(peerId);
}

/** 解析媒体消息 content；发送时只有 file_id，接收时为服务端重建的完整元数据 */
function parseMedia(content) {
  if (typeof content !== 'string') return null;
  try {
    const obj = JSON.parse(content);
    return obj && obj.file_id ? obj : null;
  } catch (e) {
    return null;
  }
}

// ---------- token 刷新 ----------

/** 用 refreshToken 换取新的 access token（并发调用合并） */
function ensureToken(force) {
  const user = store.getUser();
  if (!user || !user.refreshToken) return Promise.resolve(false);
  if (!force && skipRefreshOnce) {
    skipRefreshOnce = false;
    return Promise.resolve(true);
  }
  if (refreshPromise) return refreshPromise;
  refreshPromise = api.refreshAccessToken(user.refreshToken)
    .then((data) => {
      const u = store.getUser();
      store.setUser(Object.assign({}, u, {
        token: data.token,
        refreshToken: data.refreshToken || (u && u.refreshToken),
        expiresIn: data.expiresIn,
      }));
      refreshPromise = null;
      return true;
    })
    .catch((e) => {
      console.warn('[im] refresh token 失败:', e.message);
      refreshPromise = null;
      return false;
    });
  return refreshPromise;
}

/** token 失效：断开当前连接、换 token、再重连 */
function reconnectWithRefresh() {
  socket.close();
  tcpLoggedIn = false;
  ensureToken(true).then((ok) => {
    if (!ok) {
      emitter.emit('auth', { ok: false, error: '登录已过期，请重新登录' });
      return;
    }
    emitter.emit('auth', { ok: false, reason: 'token-refreshed' });
    setTimeout(() => socket.connect().catch(() => {}), 300);
  });
}

// ---------- TCP 登录 ----------

function tcpLogin(forceRefresh) {
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
  }, 12000);

  ensureToken(forceRefresh).then(() => {
    if (!tcpLoginPending) return;
    const user = store.getUser();
    if (!user || !user.token) {
      finishLoginReject(new Error('本地无 token，请重新登录'));
      return;
    }
    socket.send({ from: 0, to: 0, type: T.LOGIN, msg_id: 0, ts: 0, conversation_id: 0, seq: 0, content: user.token })
      .catch((err) => finishLoginReject(err));
  });

  return promise;
}

function finishLoginReject(err) {
  if (!tcpLoginPending) return;
  const p = tcpLoginPending;
  tcpLoginPending = null;
  clearTimeout(p.timer);
  p.reject(err);
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

    case T.ERROR: { // 102
      const text = frame.content || 'TCP 错误';
      if (tcpLoginPending && /token/i.test(text)) {
        finishLoginReject(new Error(text));
        reconnectWithRefresh();
      } else {
        console.warn('[im] 服务端错误:', text);
        emitter.emit('tcp-error', text);
      }
      break;
    }

    case T.SEND_ACK: // 113
      handleSendAck(frame);
      break;

    case T.TEXT: // 1
    case T.IMAGE: // 2
    case T.FILE:  // 4
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

    case T.GROUP_MEMBER_CHANGED: // 140
    case T.GROUP_DISMISSED:      // 141
    case T.GROUP_INFO_CHANGED:   // 142
    case T.GROUP_REQUEST:        // 143
    case T.GROUP_REQUEST_RESULT: // 144
      handleGroupEvent(frame);
      break;

    default:
      break;
  }
}

// ---------- 消息发送 ----------

/**
 * @param {string} peerId 单聊 userId 或 'g'+groupId
 * @param {string} content 文本或媒体元数据 JSON 字符串
 * @param {number} msgType 1=文本 2=图片 4=文件
 * @param {object} extra 本地附加信息（如文件名/大小），不发给服务端
 */
function sendMessage(peerId, content, msgType, extra) {
  const tempMsgId = nextMsgId();
  const group = isGroupPeer(peerId);
  const type = msgType || T.TEXT;
  const ts = Date.now();
  const payload = {
    from: 0,
    to: Number(group ? groupIdFromPeer(peerId) : peerId) || 0,
    type,
    msg_id: tempMsgId,
    ts,
    content,
  };
  if (group) payload.is_group = true;

  // 上行单帧 body 上限 4096 字节，超限服务端会直接断连（docs/IMS_communication_protocol.md §5.1）
  if (utf8.encode(JSON.stringify(payload)).length > 4096) {
    wx.showToast({ title: '消息过长，请精简后发送（上限 4KB）', icon: 'none', duration: 2500 });
    return null;
  }

  const msg = Object.assign({
    id: genLocalId(),
    msgId: tempMsgId,
    peerId,
    direction: 'out',
    content,
    msgType: type,
    ts,
    status: 'sending',
  }, extra || {});

  const preview = type === T.TEXT ? content : '[文件] ' + ((extra && extra.fileName) || '文件');
  store.touchSession(peerId, sessionName(peerId, group), { content: preview, ts, direction: 'out' }, group);
  store.appendMessage(msg);

  socket.send(payload).catch(() => {
    markSendFailed(peerId, tempMsgId);
  });

  const timer = setTimeout(() => markSendFailed(peerId, tempMsgId), 15000);
  sendingQueue.push({ peerId, msgId: tempMsgId, timer });

  emitter.emit('msg', msg);
  emitter.emit('sessions');
  return msg;
}

function markSendFailed(peerId, msgId) {
  const rec = sendingQueue.find((x) => x.msgId === msgId);
  if (rec && !rec.failed) {
    rec.failed = true;
    // 服务端 SendAck 不回显客户端临时 msg_id，只能按 FIFO 对应。
    // 超时项继续保留一段时间，避免迟到的 SendAck 错配到下一条消息。
    rec.cleanupTimer = setTimeout(() => {
      const idx = sendingQueue.indexOf(rec);
      if (idx !== -1) sendingQueue.splice(idx, 1);
    }, 60000);
  }
  store.updateMessage(peerId, msgId, { status: 'failed' });
  emitter.emit('msg-update', { peerId, msgId, status: 'failed' });
}

function handleSendAck(frame) {
  const rec = sendingQueue.shift();
  if (!rec) return;
  clearTimeout(rec.timer);
  if (rec.cleanupTimer) clearTimeout(rec.cleanupTimer);
  // 已超时的消息：迟到的 Ack 到此为止，不改下一条的状态
  if (rec.failed) return;

  if (frame.conversation_id) convMap[frame.conversation_id] = rec.peerId;
  const status = 'sent';
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
  store.removeMessage(peerId, msgId);
  if (m.msgType === T.IMAGE || m.msgType === T.FILE) {
    const media = parseMedia(m.content) || {};
    sendMessage(peerId, m.content, m.msgType, {
      fileName: m.fileName || media.name,
      fileSize: m.fileSize || media.size,
    });
  } else {
    sendMessage(peerId, m.content, m.msgType || T.TEXT);
  }
  emitter.emit('sessions');
}

/** 发送文件/图片消息（二进制已通过 services/file 直传并 commit） */
function sendFileMessage(peerId, fileId, fileMeta, isImage) {
  const media = { file_id: fileId };
  return sendMessage(peerId, JSON.stringify(media), isImage ? T.IMAGE : T.FILE, {
    fileName: fileMeta.name || '文件',
    fileSize: fileMeta.size || 0,
    fileMime: fileMeta.mime || '',
  });
}

// ---------- 消息接收 ----------

function handleIncoming(frame) {
  const me = store.getUser();
  const from = String(frame.from || '');
  if (!from || from === '0' || (me && Number(from) === Number(me.uid))) return;

  const group = frame.is_group === true || frame.is_group === 1;
  const peerId = group ? toPeerId(frame.to, true) : from;
  if (frame.conversation_id) convMap[frame.conversation_id] = peerId;

  const media = parseMedia(frame.content);
  const senderName = group ? memberName(groupIdFromPeer(peerId), from) : friendName(from);
  const msg = {
    id: genLocalId(),
    msgId: frame.msg_id,
    peerId,
    direction: 'in',
    content: frame.content,
    msgType: frame.type,
    ts: frame.ts || Date.now(),
    status: 'received',
    conversationId: frame.conversation_id,
    seq: frame.seq,
    senderName,
    fileName: media ? media.name : undefined,
    fileSize: media ? media.size : undefined,
    fileMime: media ? media.mime : undefined,
  };

  const preview = media
    ? (frame.type === T.FILE ? '[文件] ' + (media.name || '文件') : '[图片]')
    : frame.content;
  const known = store.getSession(peerId);
  store.touchSession(peerId, (known && known.name) || sessionName(peerId, group),
    { content: preview, ts: msg.ts, direction: 'in' }, group);
  store.appendMessage(msg);

  if (viewingPeerId !== peerId) store.incrUnread(peerId);

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

  if (group) refreshGroupInfo(groupIdFromPeer(peerId), true).catch(() => {});
}

// ---------- 历史消息 ----------

/** 拉取与 peer 的历史消息（beforeSeq=0 从最新开始） */
function requestHistory(peerId, beforeSeq, pageSize) {
  return new Promise((resolve, reject) => {
    const group = isGroupPeer(peerId);
    const payload = {
      from: 0,
      to: Number(group ? groupIdFromPeer(peerId) : peerId) || 0,
      type: T.HISTORY_REQ,
      seq: beforeSeq || 0,
      content: String(pageSize || config.history.pageSize),
    };
    if (group) payload.is_group = true;

    const rec = { peerId, isGroup: group, resolve, reject, timer: null };
    rec.timer = setTimeout(() => {
      const idx = historyQueue.findIndex((x) => x.resolve === resolve);
      if (idx !== -1) {
        clearTimeout(historyQueue[idx].timer);
        historyQueue.splice(idx, 1);
        reject(new Error('历史消息请求超时'));
      }
    }, 12000);
    historyQueue.push(rec);

    socket.send(payload).catch((err) => {
      const idx = historyQueue.findIndex((x) => x.resolve === resolve);
      if (idx !== -1) {
        clearTimeout(historyQueue[idx].timer);
        historyQueue.splice(idx, 1);
      }
      reject(err);
    });
  });
}

function handleHistoryResp(frame) {
  const rec = historyQueue.shift();
  let peerId = rec ? rec.peerId : null;
  if (frame.conversation_id && convMap[frame.conversation_id]) peerId = convMap[frame.conversation_id];
  if (!peerId) return;
  if (rec) clearTimeout(rec.timer);

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

  const group = isGroupPeer(peerId);
  const merged = existing.slice();
  list.forEach((item) => {
    if (knownIds[item.msg_id]) return;
    knownIds[item.msg_id] = true;
    if (item.conversation_id) convMap[item.conversation_id] = peerId;
    const media = parseMedia(item.content);
    merged.push({
      id: genLocalId(),
      msgId: item.msg_id,
      serverMsgId: item.msg_id,
      peerId,
      direction: me && Number(item.from) === Number(me.uid) ? 'out' : 'in',
      content: item.content,
      msgType: item.type,
      ts: item.ts,
      status: 'sent',
      conversationId: item.conversation_id,
      seq: item.seq,
      senderName: group ? memberName(groupIdFromPeer(peerId), item.from) : undefined,
      fileName: media ? media.name : undefined,
      fileSize: media ? media.size : undefined,
      fileMime: media ? media.mime : undefined,
    });
  });

  merged.sort((a, b) => (a.seq || a.ts) - (b.seq || b.ts));
  store.replaceMessages(peerId, merged);

  if (rec) rec.resolve(merged);
  emitter.emit('history', { peerId });
}

// ---------- 好友事件（130/131/132） ----------

function handleFriendEvent(frame) {
  let data = {};
  try {
    data = typeof frame.content === 'string' ? JSON.parse(frame.content) : frame.content;
  } catch (e) {
    console.error('[im] 好友事件解析失败', e);
    return;
  }

  if (frame.type === T.FRIEND_REQUEST) {
    store.incrUnread('__friend_requests__');
    emitter.emit('friend-request', Object.assign({ requestId: frame.msg_id }, data));
    emitter.emit('sessions');
  } else if (frame.type === T.FRIEND_REQUEST_RESULT) {
    emitter.emit('friend-result', Object.assign({ requestId: frame.msg_id }, data));
    refreshFriendsCache();
  } else if (frame.type === T.FRIEND_LIST_CHANGED) {
    emitter.emit('friends-changed', data);
    refreshFriendsCache();
  }
}

// ---------- 群事件（140~144） ----------

function handleGroupEvent(frame) {
  let data = {};
  try {
    data = typeof frame.content === 'string' ? JSON.parse(frame.content) : frame.content;
  } catch (e) {
    console.error('[im] 群事件解析失败', e);
    return;
  }
  const groupId = data.group_id || data.groupId;
  const action = data.action;

  switch (frame.type) {
    case T.GROUP_MEMBER_CHANGED: {
      emitter.emit('group-member-changed', data);
      // 自己被移出/退群：移除本地会话与群缓存
      const me = store.getUser();
      const uid = data.user_id || data.userId;
      if (me && String(uid) === String(me.uid) && (action === 'removed' || action === 'quit')) {
        store.removeSession(toPeerId(groupId, true));
        store.removeGroup(groupId);
        emitter.emit('sessions');
      }
      refreshGroupInfo(groupId, false).catch(() => {});
      refreshGroupsCache().catch(() => {});
      break;
    }
    case T.GROUP_DISMISSED: {
      store.removeSession(toPeerId(groupId, true));
      store.removeGroup(groupId);
      emitter.emit('group-dismissed', { groupId, name: data.name });
      emitter.emit('sessions');
      break;
    }
    case T.GROUP_INFO_CHANGED: {
      refreshGroupInfo(groupId, false).catch(() => {});
      refreshGroupsCache().catch(() => {});
      break;
    }
    case T.GROUP_REQUEST: {
      store.incrUnread('__group_requests__');
      emitter.emit('group-request', Object.assign({ requestId: frame.msg_id }, data));
      emitter.emit('sessions');
      break;
    }
    case T.GROUP_REQUEST_RESULT: {
      emitter.emit('group-result', Object.assign({ requestId: frame.msg_id }, data));
      refreshGroupsCache().catch(() => {});
      break;
    }
    default:
      break;
  }
}

// ---------- 群资料缓存（HTTP） ----------

/** 拉取我的群列表并写入缓存 */
function refreshGroupsCache() {
  return api.getGroups()
    .then((groups) => {
      store.setGroups(groups);
      groups.forEach((g) => {
        const peerId = toPeerId(g.groupId, true);
        const s = store.getSession(peerId);
        if (s) store.touchSession(peerId, g.name, s.lastMsg, true);
      });
      emitter.emit('group-list', groups);
      return groups;
    })
    .catch((e) => {
      console.warn('[im] 拉取群列表失败', e.message);
      return [];
    });
}

/** 拉取群详情与成员（silent=true 表示后台静默刷新） */
function refreshGroupInfo(groupId, silent) {
  return api.getGroup(groupId)
    .then((data) => {
      if (data.group) {
        const groups = store.getGroups().slice();
        const idx = groups.findIndex((g) => String(g.groupId) === String(groupId));
        if (idx !== -1) groups[idx] = data.group;
        else groups.push(data.group);
        store.setGroups(groups);
        const peerId = toPeerId(groupId, true);
        const s = store.getSession(peerId);
        if (s) store.touchSession(peerId, data.group.name, s.lastMsg, true);
      }
      if (data.members) store.setGroupMembers(groupId, data.members);
      if (!silent) emitter.emit('group-info', { groupId, group: data.group, members: data.members || [] });
      return data;
    })
    .catch((e) => {
      if (!silent) wx.showToast({ title: e.message || '获取群信息失败', icon: 'none' });
      throw e;
    });
}

/** 拉群列表 + 入群申请列表（进入群页面时调用） */
function refreshGroupPage() {
  return Promise.all([
    refreshGroupsCache(),
    api.getGroupRequests('incoming').then((list) => {
      store.setGroupRequests('incoming', list);
      return list;
    }).catch(() => []),
    api.getGroupRequests('outgoing').then((list) => {
      store.setGroupRequests('outgoing', list);
      return list;
    }).catch(() => []),
  ]);
}

// ---------- 好友缓存 ----------

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
      store.setUser({
        uid: data.userId,
        name: data.username,
        username: data.username,
        token: data.token,
        refreshToken: data.refreshToken,
        expiresIn: data.expiresIn,
      });
      skipRefreshOnce = true;
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
  tcpLoginPending = null;
  store.setUser(null);
  emitter.emit('auth', { ok: false, error: '已退出登录' });
}

// ---------- 启动 ----------

function boot() {
  emitter.on('frame', handleFrame);
  // HTTP 401：access token 过期但 TCP 仍在线时，自动换新 token 供后续请求使用
  emitter.on('http-unauthorized', () => {
    ensureToken(true).then((ok) => {
      if (!ok) emitter.emit('auth', { ok: false, error: '登录已过期，请重新登录' });
    });
  });
  emitter.on('state', (state) => {
    if (state === 'disconnected') {
      tcpLoggedIn = false;
      // 连接断开后 SendAck 不会再按原顺序到达，清空待确认队列避免后续错配
      sendingQueue.forEach((r) => {
        clearTimeout(r.timer);
        if (r.cleanupTimer) clearTimeout(r.cleanupTimer);
      });
      sendingQueue.length = 0;
    }
    if (state === 'connected') {
      tcpLogin()
        .then(() => Promise.all([refreshFriendsCache(), refreshGroupsCache()]))
        .catch((e) => console.warn('[im] TCP 登录失败:', e.message));
    }
  });
  store.load();
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
  if (peerId === '__group_requests__') store.setGroupRequests('incoming', store.getGroupRequests('incoming'));
  emitter.emit('sessions');
}

function setViewing(peerId) {
  viewingPeerId = peerId;
  if (peerId) markRead(peerId);
}

/** 会话摘要（用于聊天页标题与权限判断） */
function getPeerInfo(peerId) {
  const group = isGroupPeer(peerId);
  if (group) {
    const g = store.getGroup(groupIdFromPeer(peerId));
    return { group: true, groupId: groupIdFromPeer(peerId), name: g ? g.name : '群聊', data: g };
  }
  const c = store.get().contacts.find((x) => String(x.id) === String(peerId));
  return { group: false, peerId, name: c ? c.name : friendName(peerId), data: c };
}

module.exports = {
  boot,
  register,
  login,
  logout,
  ensureToken,
  isGroupPeer,
  toPeerId,
  groupIdFromPeer,
  parseMedia,
  sendMessage,
  sendFileMessage,
  resendFailed,
  requestHistory,
  refreshFriendsCache,
  refreshGroupsCache,
  refreshGroupInfo,
  refreshGroupPage,
  getSessions,
  getMessages,
  markRead,
  setViewing,
  getPeerInfo,
  isConnected: () => socket.isConnected() && tcpLoggedIn,
  currentUser: () => store.getUser(),
};
