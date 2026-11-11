#include "scancommand.h"


void RedisClient::ScanCommand::setCursor(long long cursor)
{
    if (cursor <= 0)
        return;

    if (isKeyScanCommand(m_commandWithArguments[0])) {
        m_commandWithArguments[1] = QString::number(cursor).toUtf8();
    } else if (isValueScanCommand(m_commandWithArguments[0])) {
        m_commandWithArguments[2] = QString::number(cursor).toUtf8();
    }
}

bool RedisClient::ScanCommand::hasCountArgument() const
{
    auto parts = getSplitedRepresentattion();

    for (int i = 1; i < parts.size() - 1; ++i) {
        if (parts[i].toLower() == "count")
            return true;
    }

    return false;
}

void RedisClient::ScanCommand::setCount(long long count)
{
    if (count <= 0)
        return;

    auto parts = getSplitedRepresentattion();

    for (int i = 1; i < parts.size() - 1; ++i) {
        if (parts[i].toLower() == "count") {
            m_commandWithArguments[i + 1] = QString::number(count).toUtf8();
            return;
        }
    }
}

bool RedisClient::ScanCommand::isValidScanCommand() const
{
    auto parts = getSplitedRepresentattion();

    return (parts.size() > 1 && isKeyScanCommand(parts[0]))
            || (parts.size() > 2 && isValueScanCommand(parts[0]));
}

bool RedisClient::ScanCommand::isKeyScanCommand(const QString &cmd) const
{
    return cmd.toLower() == "scan";
}

bool RedisClient::ScanCommand::isValueScanCommand(const QString &cmd) const
{
    return cmd.toLower() == "zscan"
            || cmd.toLower() == "sscan"
            || cmd.toLower() == "hscan";
}
