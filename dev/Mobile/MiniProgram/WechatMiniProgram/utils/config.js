/**
 * 铖信 - 全局配置
 *
 * 对接 KC-CXIM IMS 服务端（见 docs/IMS_readme.md）
 * - HTTP 短连接(8080)：注册、登录(JWT)、用户搜索、好友关系管理
 * - TCP 长连接(8888)：登录鉴权、心跳、单聊消息、历史消息、回执、好友事件推送
 *
 * 注意：
 * - 开发调试需在开发者工具「详情 -> 本地设置」勾选“不校验合法域名”
 *   （wx.request 走 http + IP、wx.createTCPSocket 连公网 IP 均需要）
 * - 正式上线必须 HTTPS/wss 域名 + ICP 备案，且 TCPSocket 需要符合微信类目要求
 */
module.exports = {
  server: {
    // 真机调试优先用 IP（域名解析在某些手机网络下会失败）；
    // 需要切回域名时改为 'v0.fyping.cn'，两台指向同一服务器
    host: '110.40.206.253',
    tcpPort: 10001,
    httpBase: 'http://110.40.206.253:10002',
  },

  // TCP 帧模式：4 字节大端长度前缀 + JSON（Message）
  framing: 'length',

  // 心跳：type=3，服务端不回 pong，仅刷新空闲计时；服务端空闲 60s 踢连接
  heartbeat: {
    enabled: true,
    intervalMs: 30000,
    payload: { type: 3 },
  },

  // 断线重连：指数退避 base * 2^n + 随机抖动，封顶 maxDelayMs
  reconnect: {
    baseDelayMs: 1000,
    maxDelayMs: 30000,
  },

  // TCP Message type 常量（见 docs/IMS_readme.md 6.2）
  tcpTypes: {
    TEXT: 1,                  // 文本消息（双向）
    IMAGE: 2,                 // 图片消息（双向），content 为 URL
    HEARTBEAT: 3,             // 心跳
    LOGIN: 100,               // 客户端 -> 服务端，content = JWT
    LOGIN_ACK: 101,           // 登录成功
    ERROR: 102,               // 错误通知
    HISTORY_REQ: 110,         // 历史消息请求
    HISTORY_RESP: 111,        // 历史消息响应（content 为 JSON 数组字符串，需二次解析）
    ACK: 112,                 // 投递/已读回执（客户端 -> 服务端）
    SEND_ACK: 113,            // 发送成功确认
    FRIEND_REQUEST: 130,      // 好友申请事件
    FRIEND_REQUEST_RESULT: 131, // 申请处理结果事件
    FRIEND_LIST_CHANGED: 132, // 好友列表变化事件
  },

  // 历史消息
  history: {
    pageSize: 20,
  },
};
