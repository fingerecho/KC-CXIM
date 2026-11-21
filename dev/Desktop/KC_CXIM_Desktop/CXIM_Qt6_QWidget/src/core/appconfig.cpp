#include "appconfig.h"

#include <QCoreApplication>
#include <QFileInfo>
#include <QSettings>

AppConfig::AppConfig()
    : m_httpHost(QStringLiteral("127.0.0.1"))
    , m_tcpHost(QStringLiteral("127.0.0.1"))
{
    // 与 exe 同级的 ini; 不存在时静默使用内置默认值
    const QString iniPath =
        QCoreApplication::applicationDirPath() + QStringLiteral("/CXIM_Qt6_config.ini");
    loadFromIni(iniPath);
}

void AppConfig::loadFromIni(const QString &path)
{
    if (!QFileInfo::exists(path))
        return;

    QSettings ini(path, QSettings::IniFormat);
    ini.beginGroup(QStringLiteral("server"));
    m_httpHost = ini.value(QStringLiteral("host"), m_httpHost).toString();
    m_httpPort = static_cast<quint16>(ini.value(QStringLiteral("port"), m_httpPort).toUInt());
    ini.endGroup();

    ini.beginGroup(QStringLiteral("tcp_server"));
    m_tcpHost = ini.value(QStringLiteral("host"), m_tcpHost).toString();
    m_tcpPort = static_cast<quint16>(ini.value(QStringLiteral("port"), m_tcpPort).toUInt());
    ini.endGroup();

    ini.beginGroup(QStringLiteral("client"));
    m_heartbeatMs = ini.value(QStringLiteral("heartbeat_ms"), m_heartbeatMs).toInt();
    m_historyPageSize = ini.value(QStringLiteral("history_page"), m_historyPageSize).toInt();
    ini.endGroup();
}

QString AppConfig::httpBaseUrl() const
{
    return QStringLiteral("http://%1:%2").arg(m_httpHost).arg(m_httpPort);
}
