const im = require('./services/im');
const config = require('./utils/config');

App({
  onLaunch() {
    // 启动时打印实际打包进去的服务器配置，便于排查真机缓存问题
    console.log('[app] server config:', JSON.stringify(config.server));
    // 加载本地数据，连接服务端并自动重登
    im.boot();
  },
  globalData: {},
});
