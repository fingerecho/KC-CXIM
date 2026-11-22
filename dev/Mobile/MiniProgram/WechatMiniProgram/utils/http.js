/**
 * HTTP 请求封装（wx.request -> Promise）
 *
 * 对接 KC-CXIM IMS HTTP API：
 * - 请求/响应体均为 UTF-8 JSON
 * - 除注册/登录/刷新外均需 Authorization: Bearer <JWT>
 * - 429/503 按 Retry-After 退避重试（不计入业务失败）
 */

const config = require('./config');
const emitter = require('./emitter');

const MAX_RETRY = 3;

function sleep(ms) {
  return new Promise((resolve) => setTimeout(resolve, ms));
}

function retryAfterMs(res) {
  const h = res && res.header ? res.header : {};
  const raw = h['Retry-After'] || h['retry-after'];
  const sec = Number(raw);
  if (!isNaN(sec) && sec > 0) return Math.min(sec * 1000, 10000);
  return 0;
}

/**
 * @param {string} method HTTP 方法
 * @param {string} path   接口路径
 * @param {object|null} data 请求体
 * @param {string} token  Bearer token（可选）
 * @param {object} options { retry: 是否允许 429/503 退避重试, timeout }
 */
function request(method, path, data, token, options) {
  const opts = options || {};
  return new Promise((resolve, reject) => {
    const header = { 'Content-Type': 'application/json' };
    if (token) header['Authorization'] = 'Bearer ' + token;

    wx.request({
      url: config.server.httpBase + path,
      method,
      data,
      header,
      timeout: opts.timeout || 10000,
      success(res) {
        const body = res.data;
        const status = res.statusCode;

        if (status === 429 || status === 503) {
          const attempt = opts._attempt || 0;
          const wait = retryAfterMs(res) + Math.floor(Math.random() * 300) + attempt * 500;
          if (opts.retry !== false && attempt < MAX_RETRY) {
            console.warn('[http]', status, '服务端过载/限流，', wait, 'ms 后重试', path);
            sleep(wait).then(() => {
              request(method, path, data, token, Object.assign({}, opts, { _attempt: attempt + 1 }))
                .then(resolve, reject);
            });
            return;
          }
          reject(new Error((body && body.message) || '服务繁忙，请稍后重试'));
          return;
        }

        if (status === 401) {
          // 通知业务层用 refreshToken 换新 token（排除刷新/登录接口本身，避免循环）
          if (path !== '/api/token/refresh' && path !== '/api/login') {
            emitter.emit('http-unauthorized', path);
          }
          reject(new Error('登录已过期，请重新登录'));
          return;
        }

        if (status >= 200 && status < 300 && body && body.code === 0) {
          resolve(body);
        } else {
          reject(new Error((body && body.message) || ('HTTP ' + status)));
        }
      },
      fail(err) {
        reject(new Error(err.errMsg || '网络请求失败'));
      },
    });
  });
}

function http(method, path, data, token, options) {
  return request(method, path, data, token, options);
}

module.exports = { http, sleep };
