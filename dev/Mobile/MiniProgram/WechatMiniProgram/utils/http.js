/**
 * HTTP 请求封装（wx.request -> Promise）
 *
 * 对接 KC-CXIM IMS HTTP API：
 * - 请求/响应体均为 UTF-8 JSON
 * - 除注册/登录外均需 Authorization: Bearer <JWT>
 */
const config = require('./config');

function http(method, path, data, token) {
  return new Promise((resolve, reject) => {
    const header = { 'Content-Type': 'application/json' };
    if (token) header['Authorization'] = 'Bearer ' + token;

    wx.request({
      url: config.server.httpBase + path,
      method,
      data,
      header,
      timeout: 10000,
      success(res) {
        if (res.statusCode === 401) {
          reject(new Error('登录已过期，请重新登录'));
          return;
        }
        const body = res.data;
        if (res.statusCode >= 200 && res.statusCode < 300 && body && body.code === 0) {
          resolve(body);
        } else {
          reject(new Error((body && body.message) || ('HTTP ' + res.statusCode)));
        }
      },
      fail(err) {
        reject(new Error(err.errMsg || '网络请求失败'));
      },
    });
  });
}

module.exports = { http };
