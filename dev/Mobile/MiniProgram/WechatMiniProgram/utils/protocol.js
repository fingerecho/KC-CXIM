/**
 * 私有 TCP 协议适配层
 *
 * 负责两件事：
 * 1. encode(obj)  : JSON 对象 -> 一帧（string 或 ArrayBuffer）
 * 2. createParser : 把 TCP 字节流切分成完整帧，逐帧回调 JSON 对象
 *
 * 默认支持两种帧格式（见 utils/config.js 的 framing）：
 * - 'length' : 4 字节大端长度前缀 + UTF-8 JSON 负载
 * - 'line'   : 以 \n 分隔的 JSON 行
 *
 * 如服务端使用其他格式（魔数包头、2 字节长度等），修改本文件即可，
 * 页面与业务层不感知帧细节。
 */
const config = require('./config');
const utf8 = require('./utf8');

const MODE_LENGTH = 'length';
const MODE_LINE = 'line';

/** 将 JSON 对象编码为待写入 socket 的数据 */
function encode(obj) {
  const text = JSON.stringify(obj);
  if (config.framing === MODE_LINE) {
    return text + '\n';
  }
  // length 模式：4 字节大端长度 + 负载
  const payload = utf8.encode(text);
  const out = new Uint8Array(payload.length + 4);
  out[0] = (payload.length >>> 24) & 0xff;
  out[1] = (payload.length >>> 16) & 0xff;
  out[2] = (payload.length >>> 8) & 0xff;
  out[3] = payload.length & 0xff;
  out.set(payload, 4);
  return out.buffer;
}

/** chunk 可能是 ArrayBuffer 或 string（取决于端上实现），统一转成 string */
function chunkToString(chunk) {
  if (typeof chunk === 'string') return chunk;
  return utf8.decode(new Uint8Array(chunk));
}

/** 换行分隔模式解析器 */
function createLineParser(onFrame) {
  let buf = '';
  return {
    feed(chunk) {
      buf += chunkToString(chunk);
      let idx;
      while ((idx = buf.indexOf('\n')) !== -1) {
        const line = buf.slice(0, idx).replace(/\r$/, '').trim();
        buf = buf.slice(idx + 1);
        if (!line) continue;
        try {
          onFrame(JSON.parse(line));
        } catch (e) {
          console.error('[protocol] 非法 JSON 帧:', line, e);
        }
      }
      // 防御异常超长残留
      if (buf.length > 1024 * 1024) buf = '';
    },
    reset() { buf = ''; },
  };
}

/** 4 字节长度前缀模式解析器 */
function createLengthParser(onFrame) {
  let buf = new Uint8Array(0);
  return {
    feed(chunk) {
      const incoming = chunk instanceof Uint8Array ? chunk : new Uint8Array(typeof chunk === 'string' ? utf8.encode(chunk) : chunk);
      const merged = new Uint8Array(buf.length + incoming.length);
      merged.set(buf, 0);
      merged.set(incoming, buf.length);
      buf = merged;

      // 尽可能多切出完整帧
      for (;;) {
        if (buf.length < 4) break;
        const len = (buf[0] << 24) | (buf[1] << 16) | (buf[2] << 8) | buf[3];
        if (len <= 0 || len > 4 * 1024 * 1024) {
          console.error('[protocol] 非法帧长度:', len, '丢弃缓冲区');
          buf = new Uint8Array(0);
          break;
        }
        if (buf.length < 4 + len) break;
        const payload = buf.subarray(4, 4 + len);
        buf = buf.slice(4 + len);
        try {
          onFrame(JSON.parse(utf8.decode(payload)));
        } catch (e) {
          console.error('[protocol] 帧解析失败:', e);
        }
      }
    },
    reset() { buf = new Uint8Array(0); },
  };
}

function createParser(onFrame) {
  return config.framing === MODE_LINE ? createLineParser(onFrame) : createLengthParser(onFrame);
}

module.exports = { encode, createParser };
