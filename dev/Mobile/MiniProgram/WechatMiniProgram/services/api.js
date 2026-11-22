/**
 * HTTP 业务 API（注册 / 登录 / 用户搜索 / 好友 / 群聊 / 文件）
 * 接口定义见 docs/IMS_readme.md §5 与 docs/IMS_communication_protocol.md §4
 */
const { http } = require('../utils/http');
const store = require('./store');

function token() {
  const u = store.getUser();
  return (u && u.token) || '';
}

// ---------- 鉴权 ----------

/** 注册：POST /api/register -> { userId } */
function register(username, password) {
  return http('POST', '/api/register', { username, password }).then((r) => r.data);
}

/** 登录：POST /api/login -> { userId, username, token, refreshToken, ... } */
function login(username, password) {
  return http('POST', '/api/login', { username, password }).then((r) => r.data);
}

/** 刷新 access token：POST /api/token/refresh（无需 Bearer） */
function refreshAccessToken(refreshTokenValue) {
  return http('POST', '/api/token/refresh', { refreshToken: refreshTokenValue }, null, { retry: false })
    .then((r) => r.data);
}

// ---------- 用户 / 好友 ----------

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

// ---------- 群聊（HTTP 写事实） ----------

/** 我的群列表：GET /api/groups -> { groups } */
function getGroups() {
  return http('GET', '/api/groups', null, token())
    .then((r) => (r.data && r.data.groups) || []);
}

/** 群详情：GET /api/groups/{groupId} -> { group, members } */
function getGroup(groupId) {
  return http('GET', '/api/groups/' + groupId, null, token())
    .then((r) => r.data || {});
}

/** 建群：POST /api/groups -> { group } */
function createGroup(name, memberIds) {
  return http('POST', '/api/groups', { name, memberIds }, token())
    .then((r) => (r.data && r.data.group) || null);
}

/** 修改群资料：PATCH /api/groups/{groupId} -> { group } */
function updateGroup(groupId, fields) {
  return http('PATCH', '/api/groups/' + groupId, fields, token())
    .then((r) => (r.data && r.data.group) || null);
}

/** 邀请成员：POST /api/groups/{groupId}/members -> { group, members } */
function inviteGroupMembers(groupId, memberIds) {
  return http('POST', '/api/groups/' + groupId + '/members', { memberIds }, token())
    .then((r) => r.data || {});
}

/** 移出成员：DELETE /api/groups/{groupId}/members/{userId} */
function removeGroupMember(groupId, userId) {
  return http('DELETE', '/api/groups/' + groupId + '/members/' + userId, null, token());
}

/** 任命/取消管理员：POST /api/groups/{groupId}/admins */
function setGroupAdmin(groupId, userId, admin) {
  return http('POST', '/api/groups/' + groupId + '/admins', { userId, admin: !!admin }, token());
}

/** 转让群主：POST /api/groups/{groupId}/transfer -> { group } */
function transferGroup(groupId, toUserId) {
  return http('POST', '/api/groups/' + groupId + '/transfer', { toUserId }, token())
    .then((r) => (r.data && r.data.group) || null);
}

/** 退群：POST /api/groups/{groupId}/quit */
function quitGroup(groupId) {
  return http('POST', '/api/groups/' + groupId + '/quit', null, token());
}

/** 解散群：POST /api/groups/{groupId}/dismiss */
function dismissGroup(groupId) {
  return http('POST', '/api/groups/' + groupId + '/dismiss', null, token());
}

/** 申请入群：POST /api/groups/requests */
function requestJoinGroup(groupId, message) {
  return http('POST', '/api/groups/requests', { groupId, message: message || '' }, token());
}

/** 入群申请列表：GET /api/groups/requests?direction= -> { requests } */
function getGroupRequests(direction) {
  return http('GET', '/api/groups/requests?direction=' + (direction || 'incoming'), null, token())
    .then((r) => (r.data && r.data.requests) || []);
}

/** 通过入群申请：POST /api/groups/requests/{id}/accept */
function acceptGroupRequest(requestId) {
  return http('POST', '/api/groups/requests/' + requestId + '/accept', null, token());
}

/** 拒绝入群申请：POST /api/groups/requests/{id}/reject */
function rejectGroupRequest(requestId) {
  return http('POST', '/api/groups/requests/' + requestId + '/reject', null, token());
}

// ---------- 文件模块（七牛直传控制面） ----------

/** 签发上传凭证：POST /api/files/upload-token */
function uploadTokenFile(name, mime, size) {
  return http('POST', '/api/files/upload-token', { name, mime: mime || '', size }, token())
    .then((r) => r.data || {});
}

/** 直传完成登记：POST /api/files/commit */
function commitFile(fileId, ticket) {
  return http('POST', '/api/files/commit', { file_id: fileId, ticket }, token())
    .then((r) => r.data || {});
}

/** mock 模式模拟直传：POST /api/files/mock-put（真实七牛模式返回 404） */
function mockPutFile(fileId, ticket) {
  return http('POST', '/api/files/mock-put', { file_id: fileId, ticket }, token(), { retry: false });
}

/** 删除文件：DELETE /api/files/{uuid}（仅上传者） */
function deleteFile(fileId) {
  return http('DELETE', '/api/files/' + fileId, null, token());
}

module.exports = {
  register,
  login,
  refreshAccessToken,
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
  getGroups,
  getGroup,
  createGroup,
  updateGroup,
  inviteGroupMembers,
  removeGroupMember,
  setGroupAdmin,
  transferGroup,
  quitGroup,
  dismissGroup,
  requestJoinGroup,
  getGroupRequests,
  acceptGroupRequest,
  rejectGroupRequest,
  uploadTokenFile,
  commitFile,
  mockPutFile,
  deleteFile,
};
