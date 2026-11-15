/**
 * TCPSocket 连接层
 *
 * - 基于 wx.createTCPSocket（基础库 >= 2.18.0）
 * - 自动重连（指数退避 + 抖动）
 * - 心跳保活 + 看门狗（超时未收到任何数据则强制重连）
 * - 通过 protocol.js 与 JSON 私有协议解耦
 *
 * 事件（通过 emitter）：
 * - 'state'    : 'disconnected' | 'connecting' | 'connected'
 * - 'frame'    : 收到完整 JSON 帧
 * - 'error'    : 连接/通信错误
 */
const config = require('./config');
const emitter = require('./emitter');
const protocol = require('./protocol');

const STATE_DISCONNECTED = 'disconnected';
const STATE_CONNECTING = 'connecting';
const STATE_CONNECTED = 'connected';

class TcpClient {
  constructor() {
    this.state = STATE_DISCONNECTED;
    this.socket = null;
    this.parser = null;
    this.manualClose = false;
    this.connecting = null;
    this.retries = 0;
    this.reconnectTimer = null;
    this.heartbeatTimer = null;
    this.watchdogTimer = null;
    this.lastActivity = 0;
  }

  _setState(s) {
    if (this.state !== s) {
      this.state = s;
      emitter.emit('state', s);
    }
  }

  isConnected() {
    return this.state === STATE_CONNECTED;
  }

  /** 建立连接（幂等）。resolve 于真正连上后；失败/超时 reject */
  connect() {
    if (this.state === STATE_CONNECTED) return Promise.resolve();
    if (this.connecting) return this.connecting;

    if (!wx.createTCPSocket) {
      return Promise.reject(new Error('当前基础库不支持 TCPSocket（需要 >= 2.18.0）'));
    }

    // 配置防御：地址缺失时立即报错，避免出现 "invalid address undefined:port"
    const host = config.server && config.server.host;
    const port = config.server && config.server.tcpPort;
    if (!host || !port) {
      console.error('[tcp] 服务器配置缺失: config.server =', JSON.stringify(config.server || {}));
      return Promise.reject(new Error('服务器地址未配置，请检查 utils/config.js 的 server.host / server.tcpPort'));
    }

    this.manualClose = false;
    this._setState(STATE_CONNECTING);
    this._cleanupSocket();

    this.connecting = new Promise((resolve, reject) => {
      const socket = wx.createTCPSocket();
      this.socket = socket;
      this.parser = protocol.createParser((frame) => {
        this.lastActivity = Date.now();
        emitter.emit('frame', frame);
      });

      let settled = false;
      const connectTimeout = setTimeout(() => {
        if (!settled) {
          settled = true;
          this.connecting = null;
          reject(new Error('连接超时'));
          this._onBroken();
        }
      }, 10000);

      socket.onConnect(() => {
        if (this.socket !== socket) return; // 旧连接的迟到事件，忽略
        clearTimeout(connectTimeout);
        this.retries = 0;
        this.lastActivity = Date.now();
        this._setState(STATE_CONNECTED);
        this._startHeartbeat();
        if (!settled) {
          settled = true;
          this.connecting = null;
          resolve();
        }
      });

      socket.onMessage((res) => {
        if (this.socket !== socket) return; // 旧连接的迟到事件，忽略
        // res.message: ArrayBuffer | string
        try {
          this.parser.feed(res.message);
        } catch (e) {
          console.error('[tcp] 消息解析异常', e);
        }
      });

      socket.onError((err) => {
        if (this.socket !== socket) return; // 关闭旧连接时触发的错误，忽略（防止无限递归）
        clearTimeout(connectTimeout);
        this.lastError = (err && err.errMsg) || 'unknown tcp error';
        console.error('[tcp] onError:', this.lastError);
        emitter.emit('error', err);
        if (!settled) {
          settled = true;
          this.connecting = null;
          reject(new Error(this.lastError));
        }
        this._onBroken();
      });

      socket.onClose(() => {
        if (this.socket !== socket) return; // 旧连接的迟到事件，忽略
        clearTimeout(connectTimeout);
        console.warn('[tcp] onClose 触发', this.lastError || '');
        if (!settled) {
          settled = true;
          this.connecting = null;
          reject(new Error('连接已关闭'));
        }
        this._onBroken();
      });

      const host = String(config.server.host);
      const port = Number(config.server.tcpPort);
      console.log('[tcp] connect ->', host + ':' + port);
      // 兼容说明：真机调试的调试通道会代理部分网络 API，可能丢失字符串参数（host 变 undefined）。
      // 同时传 host 与 address 两个键做兼容，正式连接请用「预览」或体验版验证。
      socket.connect({
        host: host,
        port: port,
        address: host,
        timeout: 10000,
      });
    });

    return this.connecting;
  }

  /** 发送一帧 JSON */
  send(obj) {
    return new Promise((resolve, reject) => {
      if (!this.socket || this.state !== STATE_CONNECTED) {
        reject(new Error('未连接到服务器'));
        return;
      }
      try {
        const err = this.socket.write(protocol.encode(obj));
        if (err && err.errMsg && err.errMsg.indexOf('fail') !== -1) {
          reject(new Error(err.errMsg));
        } else {
          resolve();
        }
      } catch (e) {
        reject(e);
      }
    });
  }

  /** 主动断开（登出时使用），不触发自动重连 */
  close() {
    this.manualClose = true;
    this._stopTimers();
    this._cleanupSocket();
    this.retries = 0;
    this._setState(STATE_DISCONNECTED);
  }

  // ---------- 内部 ----------

  _cleanupSocket() {
    // 先置空引用再 close：close 可能同步触发 onError/onClose，
    // 置空 + 回调里的守卫可避免 "_onBroken -> close -> onError -> _onBroken" 无限递归
    const s = this.socket;
    this.socket = null;
    this.parser = null;
    if (s) {
      try { s.close(); } catch (e) { /* ignore */ }
    }
  }

  _stopTimers() {
    if (this.reconnectTimer) { clearTimeout(this.reconnectTimer); this.reconnectTimer = null; }
    if (this.heartbeatTimer) { clearInterval(this.heartbeatTimer); this.heartbeatTimer = null; }
    if (this.watchdogTimer) { clearInterval(this.watchdogTimer); this.watchdogTimer = null; }
  }

  _onBroken() {
    this._stopTimers();
    this._cleanupSocket();
    this._setState(STATE_DISCONNECTED);
    if (!this.manualClose) this._scheduleReconnect();
  }

  _scheduleReconnect() {
    if (this.reconnectTimer || this.manualClose) return;
    const { baseDelayMs, maxDelayMs } = config.reconnect;
    const delay = Math.min(maxDelayMs, baseDelayMs * Math.pow(2, this.retries)) + Math.floor(Math.random() * 500);
    this.retries += 1;
    console.log('[tcp]', delay, 'ms 后尝试第', this.retries, '次重连');
    this.reconnectTimer = setTimeout(() => {
      this.reconnectTimer = null;
      this.connect().catch((e) => {
        console.error('[tcp] 重连尝试失败:', e.message);
      });
    }, delay);
  }

  _startHeartbeat() {
    const hb = config.heartbeat;
    if (!hb.enabled) return;
    // 心跳 type=3：服务端不回 pong，仅刷新空闲计时（服务端空闲 60s 踢连接），
    // 因此不能以“是否收到下行数据”做看门狗，链路中断由 write 失败与 onClose 触发重连。
    this.heartbeatTimer = setInterval(() => {
      if (this.state === STATE_CONNECTED) {
        this.send(Object.assign({ ts: Date.now() }, hb.payload)).catch(() => { /* ignore */ });
      }
    }, hb.intervalMs);
  }
}

module.exports = new TcpClient();
