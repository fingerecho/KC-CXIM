/** 时间格式化工具 */
function pad(n) {
  return n < 10 ? '0' + n : '' + n;
}

/** 会话列表时间：当天 HH:mm，本周内 周X HH:mm，更早 YYYY/MM/DD */
function formatChatListTime(ts) {
  if (!ts) return '';
  const d = new Date(ts);
  const now = new Date();
  const startOfDay = new Date(now.getFullYear(), now.getMonth(), now.getDate()).getTime();
  if (ts >= startOfDay) {
    return pad(d.getHours()) + ':' + pad(d.getMinutes());
  }
  if (ts >= startOfDay - 6 * 86400000) {
    return '周' + '日一二三四五六'[d.getDay()];
  }
  return d.getFullYear() + '/' + (d.getMonth() + 1) + '/' + d.getDate();
}

/** 聊天页时间戳：HH:mm */
function formatMsgTime(ts) {
  const d = new Date(ts);
  return pad(d.getHours()) + ':' + pad(d.getMinutes());
}

/** 根据字符串稳定地取一个头像配色 class */
const AVATAR_COLORS = ['avatar-blue', 'avatar-green', 'avatar-orange', 'avatar-purple', 'avatar-red'];
function avatarClass(str) {
  let h = 0;
  const s = String(str || '');
  for (let i = 0; i < s.length; i++) h = (h * 31 + s.charCodeAt(i)) >>> 0;
  return AVATAR_COLORS[h % AVATAR_COLORS.length];
}

module.exports = { formatChatListTime, formatMsgTime, avatarClass };
