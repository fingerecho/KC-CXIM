# CXIM Qt6 桌面端 (QWidget)

KC-CXIM 即时通信软件的 Qt6 原生桌面客户端, 对接自研 IMS 后端
(`dev/IMS/KC-CXIM-Svc/basic_service_20261107`), 协议口径以
`docs/IMS_communication_protocol.md` 为准, UI 对照 `docs/desktop_real_*_ui_0.png` 三张真实截图。

功能闭环: 注册 / 登录(记住密码·自动登录) / 搜索加好友 / 好友申请审批 / 备注·删除·拉黑 /
单聊·群聊文本互发 / 图片·文件互发(七牛云直传) / 建群·邀请·移出·管理员·转让·退群·解散 /
入群申请审批 / 历史消息分页 / 投递·已读回执 / 离线补推 / 断线重连。

## 构建

```bash
# Qt 6.11 MinGW 环境 (Qt Creator 直接打开本目录亦可)
cmake -S . -B build -G Ninja \
  -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_PREFIX_PATH=C:/Qt/6.11.1/mingw_64 \
  -DCMAKE_CXX_COMPILER=C:/Qt/Tools/mingw1310_64/bin/g++.exe
cmake --build build
```

只依赖 Qt 原生模块: **Widgets + Network**, 无第三方库、无 .ui 文件、无图片资源
(图标使用 Unicode emoji / 自绘圆形头像, 头像底色按 userId 哈希)。

服务端地址在 `CXIM_Qt6_config.ini`(随构建拷贝到 exe 目录):

```ini
[server]      ; HTTP 控制面
host=110.40.206.253
port=10002
[tcp_server]  ; TCP 数据面
host=v0.fyping.cn
port=10001
```

## 分层架构(低耦合的落点)

```
src/
├── core/     数据与会话: 不依赖 Qt Network/Widgets 之外的东西
│   ├── appconfig        ini 配置读取
│   ├── imstypes         业务模型 + 协议字段解析(唯一一份字段口径)
│   └── session          JWT access/refresh 与登录偏好持久化
├── net/      协议层: 只认识 JSON 帧/HTTP, 不认识业务
│   ├── httpclient       统一响应{code,message,data} · Bearer · 401→refresh 重放 · 429/503 退避
│   ├── tcpclient        4B 大端分帧 · type=100 登录 · 25s 心跳 · 指数退避重连 · 上行 4096 自律
│   └── fileuploader     七牛直传: upload-token → 表单(≤4MB)/分片V2(4MB片) → commit; mock 模式自适应
├── service/  业务门面: 把协议翻译成领域信号/回调, UI 唯一依赖
│   ├── accountservice   注册/登录/refresh(并发去重)
│   ├── friendservice    好友全量 REST 封装
│   ├── groupservice     群全量 REST 封装
│   ├── chatservice      会话存储 · 消息状态机 · SendAck/History FIFO 配对 · 回执 · 事件解析
│   └── imservice        组装根: 依赖注入 + 跨层接线(401/TCP 过期→refresh→重连)
└── ui/       界面: 只 include service 头, 不触碰 JSON/QAbstractSocket
    ├── common           theme(全局 QSS)/AvatarWidget/FramelessWindow(原生拖动+缩放)
    ├── login            LoginWindow / RegisterDialog(2:3 渐变卡片, 对照截图)
    ├── main             MainWindow(蓝顶栏+rail+双页) / ConversationListView / ContactsView
    ├── chat             ChatView(气泡列表/公告/历史分页/图片文件收发)
    └── dialogs          AddFriend / FriendRequests / CreateGroup / GroupManage / GroupRequests
```

依赖方向严格单向: `ui → service → net → core`。换传输(WebSocket/HTTP2)、换存储
(七牛→OSS)、换后端(自研→腾讯云 IM)都只动 `net/` 与组装根; 换 UI 风格只动 `ui/`。

## 关键协议决策(实现位置)

| 决策 | 位置 |
| --- | --- |
| 事件不承载事实: 132/140~144 只触发 HTTP 全量重拉 | `mainwindow.cpp wireSignals()` |
| SendAck/Error 无请求标识 → 待确认 FIFO 按 isGroup 配对 | `chatservice.cpp handleSendAck` |
| HistoryResp 严格 FIFO 串行配对, 页大小在 content 字符串 | `chatservice.cpp requestHistory/handleHistoryResp` |
| msg_id = 毫秒时间戳×16+计数器, 重试复用(幂等, 协议 5.7) | `chatservice.cpp genClientMsgId` |
| 媒体消息 content 只放 {"file_id"}, 服务端重建权威元数据 | `chatservice.cpp sendMedia` + `fileuploader` |
| 下载走服务器换取入口(302→签名直链), 带 Bearer | `chatview.cpp` 媒体下载 |
| TCP 登录 102 → refresh → 新 token 重连; 刷新失败回登录页 | `imservice.cpp` 构造接线 |
| 上行单帧 ≤4096 本地拒发; 下行按长度头严格组帧 | `tcpclient.cpp sendFrame/onReadyRead` |
| 429/503 按 Retry-After 退避, 不当业务失败 | `httpclient.cpp handleReply` |

## 已知边界(MVP 口径, 与协议文档一致)

- 群消息 113/102 与发送的配对沿用旧 Win32 客户端的 FIFO 策略(服务端不回填 client msg_id);
  同会话连发时 UI 状态短暂错位可接受, 落库以服务端为准。
- 头像 URL 字段协议存在但后端暂未提供静态资源, 当前统一用首字母哈希头像。
- 表情面板/语音/翻译按钮为占位(与截图对齐的入口), 待后端信令支持。
- 记住密码为本地明文存储(与旧客户端一致), 加密可替换 `session.cpp` 持久化实现。

## 测试账号(与旧客户端共用环境)

```
fingerecho11 / asdfls
helloworld0  / helloworld
helloworld1..3 / asdfls
```
