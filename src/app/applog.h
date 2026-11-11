#pragma once

/*
 * Lightweight diagnostic log for the keys loading pipeline.
 *
 * The application has no file logger (qDebug goes to the debugger only), so
 * "why is the tree spinning forever" was impossible to diagnose after the
 * fact. This helper writes plain text lines to
 *
 *      %APPDATA%/.../logs/keys-load.log
 *
 * and is intentionally dependency free / header only.
 */

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QMutex>
#include <QMutexLocker>
#include <QStandardPaths>
#include <QString>
#include <QTextStream>
#include <QThread>

namespace AppLog {

inline QString keysLogPath() {
  static const QString path = []() {
    // Keep the log next to connections.json (~/.rdm) so that it is always
    // findable, independently of QStandardPaths resolution.
    QString dir = QDir::homePath() + "/.rdm/logs";
    QDir().mkpath(dir);

    return dir + "/keys-load.log";
  }();

  return path;
}

inline void put(const QString &message) {
  static QMutex mutex;

  QMutexLocker locker(&mutex);

  QFile file(keysLogPath());

  if (!file.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text))
    return;

  QTextStream stream(&file);
  stream.setCodec("UTF-8");
  stream << QDateTime::currentDateTime().toString("yyyy-MM-dd HH:mm:ss.zzz")
         << " [" << QLatin1String(QByteArray::number(
                static_cast<qulonglong>(reinterpret_cast<quintptr>(
                    QThread::currentThreadId())), 16))
         << "] " << message << "\n";
}

}  // namespace AppLog
