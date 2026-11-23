
请基于我提供的阿里云 ESA 边缘函数模板代码，生成一个完整的即时通信软件官网页面代码。

项目名称：
铖信

项目简介：
一款轻量、安全、跨平台的即时通信软件，支持Windows桌面端和 Android 端。

页面要求：

1. 输出形式
- 只输出一个完整的阿里云 ESA 边缘函数 JS 文件代码。
- 不要拆分多个文件。
- 所有 CSS 写在 HTML 的 <style> 标签里。
- 所有 JS 写在 HTML 的 <script> 标签里。
- 不要使用外部 CSS、外部 JS、npm、构建工具。
- 不要使用图片、logo 图片、icon 图片、背景图。
- 可以使用系统字体、CSS 渐变、CSS 圆角、阴影、Flex/Grid 布局。
- 页面要美观、简洁、现代、响应式，适配 PC、平板、手机。
- 千万不能过分占用GPU或者CPU等硬件资源。

2. 必须保留阿里云 ESA 边缘函数结构
- 保留 const html = ... 的写法。
- 保留 async function handleRequest(request)。
- 保留 export default { async fetch(request) { ... } }。
- 返回 new Response(html, { headers: { "content-type": "text/html;charset=UTF-8" } })。
- 不要改成普通静态 HTML 文件。
- 不要引入 Express、Koa、Node http server。

3. 模板字符串安全要求，非常重要
- 我的页面里会包含 JS 和 CSS。
- 不要把完整 HTML 直接塞进普通反引号模板字符串后让我手动转义。
- 请使用 String.raw 或更安全的字符串拼接方式生成 HTML。
- 如果使用占位符，请使用唯一占位符，例如：
  __DOWNLOAD_URL_WINDOWS__
  __DOWNLOAD_URL_MACOS__
  __DOWNLOAD_URL_LINUX__
  __DOWNLOAD_URL_ANDROID_APK__
  __GITHUB_REPO_URL__
  __GITEE_REPO_URL__
- 不要在 String.raw 模板里直接写反引号。
- 不要在 String.raw 模板里直接写 ${}。
- 页面内部 JS 如果需要字符串拼接，请使用普通字符串、单引号或双引号，避免破坏外层模板。
- 输出代码必须可以直接粘贴到阿里云 ESA 边缘函数编辑器中运行。

4. 页面内容结构
页面包含以下区域：

A. 顶部导航
- 左侧显示软件名称。
- 右侧显示：
  - GitHub
  - Gitee
  - 下载客户端

B. Hero 首屏
- 大标题：软件名称
- 副标题：一句话介绍即时通信软件
- 主按钮：下载客户端
- 次按钮：查看源码
- 页面自动识别当前设备后，主按钮显示对应平台下载文案。

C. 下载区域
展示以下下载入口：
- Windows 桌面端
- macOS 桌面端
- Linux 桌面端
- Android 端

说明：
- 桌面端下载的是 Qt6 可执行文件。
- Android 下载的是 .apk 文件。
- 下载链接不要写死，统一从配置对象读取。
- 配置对象放在页面 JS 顶部，方便我后续修改。

示例配置对象：

const DOWNLOAD_CONFIG = {
  windows: "__DOWNLOAD_URL_WINDOWS__",
  macos: "__DOWNLOAD_URL_MACOS__",
  linux: "__DOWNLOAD_URL_LINUX__",
  android: "__DOWNLOAD_URL_ANDROID_APK__",
  github: "__GITHUB_REPO_URL__",
  gitee: "__GITEE_REPO_URL__"
};

D. 源码介绍区域
展示 GitHub 和 Gitee 两个源码入口。

要求：
- GitHub 卡片包含：
  - 标题：GitHub 源码
  - 简介：查看项目源码、提交记录、Issue 和 Release
  - 按钮：前往 GitHub
- Gitee 卡片包含：
  - 标题：Gitee 源码
  - 简介：国内访问更快，适合查看镜像仓库和下载资源
  - 按钮：前往 Gitee

E. 页脚
- 显示版权信息
- 显示当前年份，用 JS 自动生成
- 显示“官方下载页”

5. 设备识别逻辑
请用原生 JavaScript 实现设备识别。

识别规则：
- Android 设备：
  - 匹配 /Android/i
  - 主下载按钮跳转到 DOWNLOAD_CONFIG.android
  - 按钮文案显示：下载 Android APK

- Windows 设备：
  - 匹配 /Windows/i
  - 主下载按钮跳转到 DOWNLOAD_CONFIG.windows
  - 按钮文案显示：下载 Windows 桌面端

- macOS 设备：
  - 匹配 /Macintosh|Mac OS X|iPhone|iPad|iPod/i
  - 如果是 iPhone 或 iPad，也暂时引导到 macOS 或通用桌面下载入口
  - 按钮文案显示：下载 macOS 桌面端

- Linux 设备：
  - 匹配 /Linux|X11/i
  - 注意 Android 也包含 Linux，所以必须先判断 Android，再判断 Linux
  - 按钮文案显示：下载 Linux 桌面端

- 无法识别：
  - 显示全部下载入口
  - 主按钮文案显示：选择下载版本

6. 交互要求
- 点击“下载客户端”按钮时，根据识别结果跳转对应下载链接。
- 如果下载链接还是占位符，例如包含 __DOWNLOAD_URL_，则不要跳转，而是滚动到下载区域，并提示“请先配置真实下载链接”。
- GitHub 和 Gitee 链接如果还是占位符，也要给出友好提示。
- 所有外部链接使用 target="_blank" 和 rel="noopener noreferrer"。
- 页面不能出现 alert 弹窗，使用页面内轻提示 toast。

7. UI 风格
- 深色科技风。
- 背景使用 CSS 渐变。
- 主色使用蓝色或青蓝色。
- 卡片使用半透明背景、圆角、阴影。
- 按钮有明显 hover 效果。
- 手机端单列布局。
- PC 端下载卡片可以一行 4 个，源码卡片一行 2 个。
- 页面加载要快，不依赖任何第三方库。

8. SEO 和基础 meta
HTML head 中包含：
- <meta charset="UTF-8">
- <meta name="viewport" content="width=device-width, initial-scale=1.0">
- <title>软件名称 - 官方下载</title>
- <meta name="description" content="一句话介绍">
- <meta name="theme-color" content="#0f172a">

9. 输出要求
- 只输出完整代码。
- 不要解释。
- 不要输出 Markdown。
- 不要输出多个版本。
- 代码必须可以直接粘贴到阿里云 ESA 边缘函数中使用。
- 如果必须使用示例链接，请使用占位符，不要编造真实下载地址。

我的 ESA 模板代码如下：

const html = `<!DOCTYPE html>
<body>
  <h1>Hello World<br/>方克铖!</h1>
</body>`

async function handleRequest(request) {
  return new Response(html, {
    headers: {
      "content-type": "text/html;charset=UTF-8",
    },
  })
}

export default {
  async fetch(request) {
    return handleRequest(request);
  }
};
