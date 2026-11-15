/**
 * HTTP 业务 API（注册 / 登录 / 用户搜索 / 好友关系管理）
 * 接口定义见 docs/IMS_readme.md 第 4、5 节
 */
const { http } = require('../utils/http');
const store = require('./store');

function token() {
  const u = store.getUser();
  return (u && u.token) || '';
}

/** 注册：POST /api/register -> { userId } */
function register(username, password) {
  return http('POST', '/api/register', { username, password }).then((r) => r.data);
}

/** 登录：POST /api/login -> { userId, username, token } */
function login(username, password) {
  return http('POST', '/api/login', { username, password }).then((r) => r.data);
}

/** 搜索用户：GET /api/users/search?keyword=&limit= -> { users } */
function searchUsers(keyword, limit) {
  return http('GET', '/api/users/search?keyword=' + encodeURIComponent(keyword || '') + '&limit=' + (limit || 20), null, token())
    .then((r) => (r.data && r.data.users) || []);
}

/** 好友列表：GET /api/friends -> { friends } */
function getFriends() {
  return http('GET', '/api/friends', null, token())
    .then((r) => (r.data && r.data.friends) || []);
}

/** 好友申请列表：GET /api/friends/requests?direction= -> { requests } */
function getFriendRequests(direction) {
  return http('GET', '/api/friends/requests?direction=' + (direction || 'incoming'), null, token())
    .then((r) => (r.data && r.data.requests) || []);
}

/** 发送好友申请：POST /api/friends/requests -> { request } */
function sendFriendRequest(toUserId, message) {
  return http('POST', '/api/friends/requests', { toUserId, message: message || '' }, token())
    .then((r) => (r.data && r.data.request) || null);
}

/** 通过好友申请：POST /api/friends/requests/{id}/accept */
function acceptFriendRequest(requestId) {
  return http('POST', '/api/friends/requests/' + requestId + '/accept', null, token());
}

/** 拒绝好友申请：POST /api/friends/requests/{id}/reject */
function rejectFriendRequest(requestId) {
  return http('POST', '/api/friends/requests/' + requestId + '/reject', null, token());
}

/** 修改好友备注：PATCH /api/friends/{friendId} */
function setFriendRemark(friendId, remark) {
  return http('PATCH', '/api/friends/' + friendId, { remark }, token());
}

/** 删除好友：DELETE /api/friends/{friendId} */
function deleteFriend(friendId) {
  return http('DELETE', '/api/friends/' + friendId, null, token());
}

/** 拉黑：POST /api/friends/{friendId}/block */
function blockFriend(friendId) {
  return http('POST', '/api/friends/' + friendId + '/block', null, token());
}

/** 取消拉黑：POST /api/friends/{friendId}/unblock */
function unblockFriend(friendId) {
  return http('POST', '/api/friends/' + friendId + '/unblock', null, token());
}

module.exports = {
  register,
  login,
  searchUsers,
  getFriends,
  getFriendRequests,
  sendFriendRequest,
  acceptFriendRequest,
  rejectFriendRequest,
  setFriendRemark,
  deleteFriend,
  blockFriend,
  unblockFriend,
};
