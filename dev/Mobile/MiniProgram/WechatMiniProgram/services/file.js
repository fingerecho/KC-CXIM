/**
 * 文件模块：图片/文件直传与下载
 *
 * 流程（见 docs/IMS_communication_protocol.md §4.15）：
 *   1. POST /api/files/upload-token  申报 name/mime/size，拿 fileId/key/uptoken/ticket
 *   2. 直传字节：
 *      - 服务端 MockObjectStore（QINIU_ENABLED=0）：POST /api/files/mock-put
 *      - 真实七牛：wx.uploadFile 表单直传到 uploadUrl/key（token + key + file）
 *   3. POST /api/files/commit        服务端 stat 核验后登记 files 表
 *   4. TCP type=2/4 带 {"file_id": ...}（由 im.sendFileMessage 完成）
 *   5. 下载：GET /api/files/{uuid} 302 到短时效签名直链，wx.downloadFile 自动跟随
 *
 * 二进制不经 IM 服务器，消息里只存服务器换取入口（稳定 URL）。
 */
const config = require('../utils/config');
const store = require('./store');
const api = require('./api');

const MIME_MAP = {
  jpg: 'image/jpeg', jpeg: 'image/jpeg', png: 'image/png', gif: 'image/gif',
  webp: 'image/webp', bmp: 'image/bmp',
  pdf: 'application/pdf', txt: 'text/plain', md: 'text/markdown',
  doc: 'application/msword',
  docx: 'application/vnd.openxmlformats-officedocument.wordprocessingml.document',
  xls: 'application/vnd.ms-excel',
  xlsx: 'application/vnd.openxmlformats-officedocument.spreadsheetml.sheet',
  ppt: 'application/vnd.ms-powerpoint',
  pptx: 'application/vnd.openxmlformats-officedocument.presentationml.presentation',
  zip: 'application/zip', rar: 'application/x-rar-compressed', '7z': 'application/x-7z-compressed',
  mp3: 'audio/mpeg', mp4: 'video/mp4', wav: 'audio/wav',
};

const DOC_EXT = ['pdf', 'txt', 'md', 'doc', 'docx', 'xls', 'xlsx', 'ppt', 'pptx'];

function extOf(name) {
  const s = String(name || '');
  const i = s.lastIndexOf('.');
  return i >= 0 ? s.slice(i + 1).toLowerCase() : '';
}

function mimeOf(name) {
  return MIME_MAP[extOf(name)] || 'application/octet-stream';
}

function isImageName(name) {
  return config.file.imageExt.indexOf(extOf(name)) !== -1;
}

function token() {
  const u = store.getUser();
  return (u && u.token) || '';
}

/** 从本地文件路径选择图片（相册/拍照） */
function chooseImage() {
  return new Promise((resolve, reject) => {
    const done = (filePath, size) => {
      if (!filePath) { reject(new Error('未选择图片')); return; }
      resolve({
        filePath,
        size: size || 0,
        name: filePath.split('/').pop() || ('image.' + (extOf(filePath) || 'png')),
      });
    };
    if (wx.chooseMedia) {
      wx.chooseMedia({
        count: 1,
        mediaType: ['image'],
        sourceType: ['album', 'camera'],
        success(res) {
          const f = res.tempFiles && res.tempFiles[0];
          done(f && f.tempFilePath, f && f.size);
        },
        fail(err) { reject(new Error(err.errMsg || '选择图片失败')); },
      });
    } else {
      wx.chooseImage({
        count: 1,
        sourceType: ['album', 'camera'],
        success(res) { done(res.tempFilePaths && res.tempFilePaths[0], 0); },
        fail(err) { reject(new Error(err.errMsg || '选择图片失败')); },
      });
    }
  });
}

/** 从微信聊天会话选择文件（小程序标准能力） */
function chooseFile() {
  return new Promise((resolve, reject) => {
    wx.chooseMessageFile({
      count: 1,
      type: 'file',
      success(res) {
        const f = res.tempFiles && res.tempFiles[0];
        if (!f) { reject(new Error('未选择文件')); return; }
        resolve({ filePath: f.path, size: f.size || 0, name: f.name || f.path.split('/').pop() || 'file' });
      },
      fail(err) { reject(new Error(err.errMsg || '选择文件失败')); },
    });
  });
}

/** 上传前校验：扩展名白名单 + 大小上限 */
function validate(name, size) {
  const ext = extOf(name);
  if (config.file.allowedExt.indexOf(ext) === -1) {
    throw new Error('不支持的文件类型：.' + (ext || '?'));
  }
  if (!size || size <= 0) throw new Error('文件为空或无法读取大小');
  if (size > config.file.maxSizeMB * 1024 * 1024) {
    throw new Error('文件不能超过 ' + config.file.maxSizeMB + 'MB');
  }
}

/** 真实七牛：表单直传（小文件一次往返，服务端只做 commit 核验） */
function formPut(uploadInfo, filePath, name, mime) {
  return new Promise((resolve, reject) => {
    wx.uploadFile({
      url: uploadInfo.uploadUrl + '/' + uploadInfo.key,
      filePath,
      name: 'file',
      header: {}, // 不要手动设置 Content-Type，由 wx.uploadFile 生成 multipart 边界
      formData: {
        token: uploadInfo.uptoken,
        key: uploadInfo.key,
      },
      success(res) {
        if (res.statusCode !== 200) {
          reject(new Error('直传失败 HTTP ' + res.statusCode + '：' + String(res.data || '').slice(0, 120)));
          return;
        }
        try {
          const body = typeof res.data === 'string' ? JSON.parse(res.data) : res.data;
          if (body && body.error) { reject(new Error(body.error)); return; }
          resolve(body || {});
        } catch (e) {
          resolve({});
        }
      },
      fail(err) { reject(new Error(err.errMsg || '直传失败')); },
    });
  });
}

/** 判断是否为 MockObjectStore（服务端 QINIU_ENABLED=0） */
function isMockUpload(uploadUrl) {
  const u = String(uploadUrl || '');
  return u.indexOf('mock-put') !== -1 || u.indexOf(config.server.httpBase) === 0;
}

/**
 * 完整上传链路：签发凭证 -> 直传 -> commit
 * @returns Promise<{ fileId, url, name, size, mime, qetag }>
 */
function upload(filePath, name, size) {
  const mime = mimeOf(name);
  // 部分选择接口不返回大小（如旧版 chooseImage），用 getFileInfo 补齐
  const sizeReady = (size && size > 0)
    ? Promise.resolve(size)
    : new Promise((resolve, reject) => {
      wx.getFileInfo({
        filePath,
        success(res) { resolve(res.size || 0); },
        fail(err) { reject(new Error(err.errMsg || '读取文件信息失败')); },
      });
    });

  return sizeReady.then((realSize) => {
    validate(name, realSize);
    return api.uploadTokenFile(name, mime, realSize).then((info) => {
      if (!info || !info.fileId || !info.ticket) {
        throw new Error('签发上传凭证失败');
      }
      const put = isMockUpload(info.uploadUrl)
        ? api.mockPutFile(info.fileId, info.ticket)
        : formPut(info, filePath, name, mime);

      return put.then(() => api.commitFile(info.fileId, info.ticket)).then((committed) => ({
        fileId: info.fileId,
        url: committed.url || (config.server.httpBase + '/api/files/' + info.fileId),
        name,
        size: committed.size || realSize,
        mime,
        qetag: committed.qetag || '',
      }));
    });
  });
}

/** 便捷方法：选择并上传（返回媒体元数据） */
function chooseAndUpload(kind) {
  const pick = kind === 'image' ? chooseImage() : chooseFile();
  return pick.then((f) => upload(f.filePath, f.name, f.size));
}

/**
 * 下载文件：GET /api/files/{uuid} 会 302 到短时效签名直链，
 * wx.downloadFile 自动跟随重定向，返回本地临时路径。
 */
function download(url) {
  return new Promise((resolve, reject) => {
    if (!url) { reject(new Error('文件地址无效')); return; }
    wx.downloadFile({
      url,
      header: { Authorization: 'Bearer ' + token() },
      success(res) {
        if (res.statusCode === 200 && res.tempFilePath) resolve(res.tempFilePath);
        else reject(new Error('下载失败 HTTP ' + res.statusCode));
      },
      fail(err) { reject(new Error(err.errMsg || '下载失败')); },
    });
  });
}

/** 预览/打开已下载的文件 */
function open(filePath, name) {
  const ext = extOf(name || filePath);
  if (config.file.imageExt.indexOf(ext) !== -1) {
    wx.previewImage({ urls: [filePath] });
    return;
  }
  if (DOC_EXT.indexOf(ext) !== -1) {
    wx.openDocument({
      filePath,
      showMenu: true,
      fail(err) { wx.showToast({ title: err.errMsg || '打开失败', icon: 'none' }); },
    });
    return;
  }
  wx.showToast({ title: '已下载，该文件类型暂不支持直接打开', icon: 'none', duration: 2500 });
}

module.exports = {
  chooseImage,
  chooseFile,
  upload,
  chooseAndUpload,
  download,
  open,
  extOf,
  mimeOf,
  isImageName,
};
