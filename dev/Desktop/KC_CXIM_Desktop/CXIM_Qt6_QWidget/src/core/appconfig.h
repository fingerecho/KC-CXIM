#ifndef CXIM_APPCONFIG_H
#define CXIM_APPCONFIG_H

#include <QString>

/**
 * @brief 应用配置(服务端地址等)
 *
 * 设计要点:
 *  - 配置文件 CXIM_Qt6_config.ini 位于可执行文件同级目录, 与旧版 Win32 客户端
 *    (CXIM_Desktop_config.ini)保持同样的键结构, 便于运维共用一份部署说明;
 *  - 只暴露读取接口, 缺省时返回代码内置默认值, UI 层不直接触碰 QSettings,
 *    保证配置来源可替换(将来可换成远端下发配置)。
 */
class AppConfig
{
public:
    AppConfig();

    // HTTP 接入层(注册/登录/好友/群/文件控制面)
    QString httpHost() const { return m_httpHost; }
    quint16 httpPort() const { return m_httpPort; }
    QString httpBaseUrl() const;              // http://host:port

    // TCP 长连接(聊天/事件推送)
    QString tcpHost() const { return m_tcpHost; }
    quint16 tcpPort() const { return m_tcpPort; }

    // 心跳间隔(ms): 协议建议 30s, 空闲超时 60s, 取 25s 留足余量
    int heartbeatIntervalMs() const { return m_heartbeatMs; }

    // 单帧拉取历史条数(DB 层上限 100)
    int historyPageSize() const { return m_historyPageSize; }

private:
    void loadFromIni(const QString &path);

    QString m_httpHost;
    quint16 m_httpPort = 8080;
    QString m_tcpHost;
    quint16 m_tcpPort = 8888;
    int m_heartbeatMs = 25000;
    int m_historyPageSize = 50;
};

#endif // CXIM_APPCONFIG_H
