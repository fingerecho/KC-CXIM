# 铖信 Android 移动端实现说明

依据 `docs/IMS_readme.md` 与 `docs/IMS_communication_protocol.md` 实现，界面与交互口径对齐桌面端
`CXIM_Qt6_QWidget`（配色、气泡、会话行、群管理权限矩阵、行内错误提示等均取自桌面端 `theme.cpp` 的实际值）。

## 1. 工程结构

```
app/src/main/java/com/example/helloworld/
├── CXIMApp.java                 Application：进程级单例 ImService + 通知渠道 + 前后台计数
├── im/                          协议与服务层（不依赖任何界面）
│   ├── MsgType.java             帧类型常量（1/2/3/4、100~113、130~144）
│   ├── Envelope.java            TCP Message 信封（from/to/type/msg_id/ts/conversation_id/seq/content/is_group）
│   ├── Json.java                宽松取值：兼容数字/字符串型 ID、is_group 的 bool|int 两种写法
│   ├── Config.java              服务器地址、心跳、分页、4096 上行上限
│   ├── Session.java             access/refresh 双 token + 登录偏好（记住密码/自动登录）
│   ├── HttpClient.java          HTTP 控制面（OkHttp）：401→refresh→重放、429/503→Retry-After 退避、网络错误退避
│   ├── TcpClient.java           TCP 数据面：分帧、登录、心跳、抖动退避重连、异步写队列
│   ├── FileUploader.java        文件直传：upload-token → mock/表单/分片 V2 → commit；下载走 302 换取入口
│   ├── AccountService.java      注册 / 登录 / 刷新 token
│   ├── FriendService.java       搜索、好友列表、申请、备注、删除、拉黑、批量接口
│   ├── GroupService.java        建群、群列表/详情、改资料、邀请/移出、任命、转让、退群、解散、入群审批
│   ├── ChatService.java         会话聚合、消息状态机、历史分页、回执、SendAck 配对、事件分发
│   ├── Contacts.java            好友/群/成员本地缓存 + 待处理角标
│   ├── ImService.java           门面（组装根）：接线刷新钩子、登录后全量对齐、会话过期广播
│   ├── ApiResult / Callback     统一响应与主线程回调
│   └── model/                   UserCard / FriendInfo / FriendRequestInfo / GroupInfo /
│                                GroupMember / GroupRequestInfo / ChatMessage / Conversation / MediaMeta
└── ui/                          界面层
    ├── BaseActivity / Ui / widget.AvatarView     顶栏绑定、通用工具、首字母圆形头像
    ├── LoginActivity / RegisterActivity / ServerSettingsActivity
    ├── MainActivity + ConversationListFragment / ContactsFragment / MeFragment
    ├── ChatActivity + ChatAdapter                气泡（文本/图片/文件）、历史上拉、重发、已读
    ├── FileViewerActivity                        媒体下载与打开
    ├── AddFriendActivity / FriendRequestsActivity
    ├── CreateGroupActivity / GroupDetailActivity / InviteMembersActivity /
    │   GroupRequestsActivity / GroupJoinActivity
    └── MultiSendActivity                         群发（多选好友与群）
```

## 2. 功能对照

| 需求 | 实现 | 协议依据 |
|---|---|---|
| 注册 / 登录 | 注册页本地校验（用户名 ≤64 字节、密码 6~72 字节、两次一致）；登录页记住密码/自动登录联动、行内错误 | §4.1 §4.2 §4.3 |
| 添加好友 | 按用户名/昵称/ID 搜索 → 发申请（默认附言）；「新朋友」页处理收到的与发出的申请；备注、删除、拉黑 | §4.4~§4.13 |
| 互发消息 | 单聊文本、在线推送、离线补推、SendAck 状态（发送中/已发送/失败可点重发，复用同一 `msg_id`）、历史上拉分页、投递与已读回执 | §5.7 §5.9 §5.10 |
| 群操作 | 建群（多选好友）、改群名/公告、邀请成员（已在群内的置灰）、移出、任命/取消管理员、转让群主、退群、解散；权限矩阵严格按角色控制入口 | §4.14 |
| 群消息 | 群聊文本/图片/文件（`to=groupId` + `is_group=true`）、群成员发送者名解析、群事件 140~144 驱动刷新 | §5.11 §5.13 |
| 互发文件 | 图片与文件：选系统文件 → 直传对象存储 → commit → 发 `{"file_id":...}`；接收端渲染缩略图/文件卡片，点击下载后交系统应用打开 | §4.15 §5.8 |
| 入群申请 | 按群号申请、群主/管理员审批（通过与拒绝）、待审批角标 | §4.14.7 |
| 群发消息 | 多选好友与群，逐条发送并显示进度 | 复用 §5.7 §5.11 |

## 3. 关键协议实现要点

- **分帧**：4 字节大端长度 + UTF-8 JSON。上行单帧 body 超过 4096 字节一律本地拒发（服务端会直接断连）；
  下行严格按长度头读取，不用 4096 限制（`HistoryResp` 可远大于此），并处理粘包/半包。
- **socket 写入全部在专用线程**：`TcpClient` 用单线程写队列，UI 线程调 `sendText`/`markRead` 只入队。
  否则 targetSdk 35 会在主线程写 socket 时抛 `NetworkOnMainThreadException`。
- **重连不用密码**：断线后按 `min(30s, 1s×2^n) + rand(0,1s)` 抖动退避；重连前若 access token 临近过期
  先调 `/api/token/refresh`，只有 refresh 也失效才判会话过期回登录页（协议 §4.3 的动机是避免登录风暴）。
- **登录后全量对齐**：132/140/141/142 不持久化，所以 TCP 登录成功一律重拉好友列表、群列表与待处理数。
- **二次解析**：`HistoryResp.content` 是「JSON 数组的字符串」，事件 `content` 是「JSON 对象的字符串」。
- **事件字段命名不统一（易踩）**：130/131/132 与 140/141/142 的 payload 是 snake_case
  （`request_id`/`group_id`/`user_id`/`requester_nickname`），而 **143/144 的 payload 是 camelCase**
  （`requestId`/`groupId`/`groupName`/`requesterNickname`）；HTTP 响应一律 camelCase。
  两套口径都按协议 §5.12/§5.13 的示例逐字核对过，读错就会静默取不到值。
- **`server busy` 不判失败**：错误帧含 `server busy` 时按协议 §6.2 降速并用**同一 `msg_id`** 自动重试（上限 2 次），
  服务端按 `client_msg_id` 去重，命中时 SendAck 回 `duplicate`，因此重发是幂等的。
- **102 要分流**：`bad peer`/`history failed`/`not a group member` 这类错误对应的是**拉历史请求**而不是某条待发消息，
  必须复位在途历史队列，否则后续翻页会永久排队。
- **SendAck 配对**：服务端不回带客户端 `msg_id`。先按 `conversation_id` 精确匹配已知会话，
  匹配不到再退回「同 `is_group` 队首」；`content=="duplicate"` 视为去重命中（等价成功）。
- **文件二进制不过 IM 服务器**：`QINIU_ENABLED=0` 时走 `/api/files/mock-put`，真实模式小文件走表单直传、
  大于 4MB 走分片 V2（bucket 从 `uptoken` 第三段 `policy.scope` 解出，key 做 urlsafe base64）。
  消息里永远只存服务器换取入口 `GET /api/files/{uuid}`，下载时 302 到短时效签名直链。

## 4. 系统栏适配（edge-to-edge）

targetSdk 35 起系统强制 edge-to-edge：布局不加 `fitsSystemWindows` 时，自绘顶栏会画在状态栏底下，
表现为「左上角返回键和时钟/电量重叠、按不动」。处理方式集中在 `BaseActivity`：

- `protectBarBelowStatusBar(view)`：给顶栏补 `paddingTop = 状态栏内缩（含刘海）+ 20dp`，
  并把固定高度改成 `wrap_content + minHeight`，留白只把顶栏撑高、不挤压内容。
  留白大小就是第二个参数 `extraDp`（约一个汉字高度），觉得不够可以调到 24~32。
- `protectBarAboveNavigationBar(view)`：同理把导航栏/手势条高度补成 `paddingBottom`。
- 带 `view_header` 顶栏的页面在 `bindHeaderViews()` 里自动生效；
  主界面的品牌栏与登录/注册页因为不用 `view_header`，各自显式调用一次。
- API < 35 上系统本来就把内容顶在状态栏下方，此时 `statusBars()` 内缩为 0，
  这套逻辑只会加上留白、不会双重内缩，所以同一份代码在所有版本都成立。

## 5. 构建与安装

```bash
./build_apk.sh              # debug：产物 app/build/outputs/apk/debug/app-debug.apk
./build_apk.sh release      # release：产物 app/build/outputs/apk/release/app-release.apk
adb install -r app/build/outputs/apk/debug/app-debug.apk
```

### release 签名

`assembleRelease` 必须有签名配置，否则产出 `app-release-unsigned.apk`，
真机安装会在解析阶段直接被拒，报「软件包似乎无效」。工程现在的做法：

- 项目根 `keystore.properties`（**已加入 .gitignore，不要提交**）记录签名库与密码：
  ```properties
  storeFile=cxim-release.jks
  storePassword=<见本地文件>
  keyAlias=cxim
  keyPassword=<同上>
  ```
- `app/build.gradle` 读这个文件生成 `signingConfigs.cximRelease` 并挂到 release 上；
  **缺文件时 release 仍然构建成功但产出未签名包**，并在构建开头打警告，避免别人 clone 后直接失败。
- 实测签名方案：`v2=true`、`v1=false`。minSdk 24 起 AGP 按 minSdk 推导，所有目标设备都支持 v2，
  所以不带 v1 是正确的；关键约束是 Android 11+ 装 targetSdk 30+ 的包必须有 v2
  （只补 v1 jar 签名同样会报「软件包似乎无效」）。
- `build_apk.sh` 出包后会自动 `apksigner verify`：签名不通过就直接报错退出，
  并在校验通过时列出实际生效的签名方案；缺 `keystore.properties` 时给出可复制的 keytool 生成命令。

注意：

1. `cxim-release.jks` 不能丢也不能随便换——换了签名，已安装的用户就无法覆盖升级。
2. debug 与 release 的签名证书不同（debug 用 `~/.android/debug.keystore`），
   装过 debug 包再装 release 包会因签名不一致失败，需要先 `adb uninstall com.example.helloworld`。
3. 这个库是本地自测用的发布库。要正式分发就把自己的 jks 放好、改 `keystore.properties` 即可，
   代码不用动。

- 依赖：AndroidX + Material 1.12、RecyclerView、SwipeRefreshLayout、ViewPager2、Gson、OkHttp 4.12
  （用 OkHttp 是因为好友备注与群资料接口是 **PATCH**，`HttpURLConnection` 不支持该方法）。
- 服务端地址默认与桌面端 `CXIM_Qt6_config.ini` 一致（HTTP 10002 / TCP 10001），
  登录页底部「服务器设置」可改，改后需重新登录。服务端未启用 TLS，清单里已开 `usesCleartextTraffic`。
- **已用真实服务端核对**：HTTP 登录、`/api/token/refresh` 轮换、好友列表、用户搜索、好友申请列表、
  群列表与入群申请列表（字段名与本文解析口径一致）；TCP 侧核对过 `100→101`、文本消息 `SendAck`、
  历史消息的二次 JSON 解析。
- **未在真实服务端验证**：文件直传的**真实七牛模式**（该服务端跑 `QINIU_ENABLED=0` 的 mock，
  表单直传与分片 V2 分支只按协议和桌面端实现做过静态比对）、以及全套群操作
  （建群→邀请→任命→转让→退群→解散）。这两项需要在服务端配上七牛、或用测试账号实跑一轮才能定案。
- 补充口径：表单直传必须 POST 到 `{uploadUrl}/{key}`（key 也在路径里，协议 §4.15.6 B）；
  分片 V2 的 bucket 从 `uptoken` 第三段 `policy.scope` 解出，`entry` 用 urlsafe base64 且保留 `=` 填充。

## 6. 已知限制

- 服务端不提供在线状态（presence），所以界面上没有「在线/离线」标记，与桌面端一致。
- 群简介 `intro` 没有编辑入口；公告无法清空——服务端把空串解释为「不修改该字段」，与桌面端同一取舍。
- 服务端没有群搜索接口，「申请加群」只能按群主给出的群号提交。
- 长连接依赖应用进程存活：退到后台可能被系统冻结，回前台会自动重连并补推离线消息；
  没有做前台服务保活（需要额外的通知与权限声明，属后续项）。
- 图片缩略图按 `fileId` 做进程内 LruCache，冷启动重新拉取。
- 应用退到后台后收到的消息会发系统通知，Android 13+ 需要用户授予通知权限（进主界面会主动申请一次）。
