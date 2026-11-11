#include "connectionconf.h"

#include <QSettings>

ServerConfig::ServerConfig(const QString &host, const QString &auth, const uint port, const QString &name)
    : RedisClient::ConnectionConfig(host, auth, port, name),
      m_owner(QSharedPointer<TreeOperations>())
{
}

ServerConfig::ServerConfig(const QVariantHash &options)
    : RedisClient::ConnectionConfig(options),
      m_owner(QSharedPointer<TreeOperations>())
{

}

QString ServerConfig::keysPattern() const
{
    /*
     * Per-connection pattern has a priority. When it is not set or empty
     * the global default (Settings -> Default keys filter) is used.
     */
    QString custom = param<QString>("keys_pattern", QString());

    if (!custom.isEmpty()) return custom;

    QString globalDefault =
            QSettings().value("app/defaultKeysPattern",
                              QString(DEFAULT_KEYS_GLOB_PATTERN)).toString();

    return globalDefault.isEmpty() ? QString(DEFAULT_KEYS_GLOB_PATTERN)
                                   : globalDefault;
}

void ServerConfig::setKeysPattern(QString keyGlobPattern)
{
    setParam<QString>("keys_pattern", keyGlobPattern);
}

qlonglong ServerConfig::keysLoadLimit() const
{
    QVariant custom = param<QVariant>("keys_load_limit", QVariant());

    if (custom.isValid() && custom.toLongLong() >= 0)
        return custom.toLongLong();

    QSettings settings;

    qlonglong globalLimit = settings.value("app/scanLimit",
                                          DEFAULT_KEYS_LOAD_LIMIT).toLongLong();

    return globalLimit < 0 ? 0 : globalLimit;
}

void ServerConfig::setKeysLoadLimit(qlonglong limit)
{
    setParam<qlonglong>("keys_load_limit", limit);
}

QString ServerConfig::namespaceSeparator() const
{
    return param<QString>("namespace_separator", QString(DEFAULT_NAMESPACE_SEPARATOR));
}

void ServerConfig::setNamespaceSeparator(QString ns)
{
    return setParam<QString>("namespace_separator", ns);
}

uint ServerConfig::databaseScanLimit() const
{
    return param<uint>("db_scan_limit", DEFAULT_DB_SCAN_LIMIT);
}

void ServerConfig::setDatabaseScanLimit(uint limit)
{
    setParam<uint>("db_scan_limit", limit);
}

bool ServerConfig::useSshTunnel() const
{
    return RedisClient::ConnectionConfig::useSshTunnel();
}

QWeakPointer<TreeOperations> ServerConfig::owner() const
{
    return m_owner;
}

void ServerConfig::setOwner(QWeakPointer<TreeOperations> o)
{
    m_owner = o;
}

QVariantMap ServerConfig::filterHistory()
{
    return param<QVariantMap>("filter_history");
}

void ServerConfig::setFilterHistory(QVariantMap filterHistory)
{
    setParam<QVariantMap>("filter_history", filterHistory);
}

bool ServerConfig::askForSshPassword() const
{
    return param<bool>("ask_ssh_password", false);
}

void ServerConfig::setAskForSshPassword(bool v)
{
    setParam<bool>("ask_ssh_password", v);
}

QString ServerConfig::defaultFormatter() const
{
    return param<QString>("default_formatter", QString("auto"));
}

void ServerConfig::setDefaultFormatter(const QString &v)
{
    setParam<QString>("default_formatter", v);
}

QString ServerConfig::iconColor() const
{
    return param<QString>("icon_color", QString(""));
}

void ServerConfig::setIconColor(const QString &v)
{
    setParam<QString>("icon_color", v);
}
