#include "bookmarksmanager.h"

#include <QDateTime>
#include <QDebug>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSettings>
#include <QUuid>

namespace {
const char* kSettingsKey = "bookmarks/items";

QString okResult() { return QString(); }
}  // namespace

BookmarksManager::BookmarksManager(QObject* parent) : QObject(parent) { load(); }

int BookmarksManager::count() const { return m_items.size(); }

QVariantList BookmarksManager::items() const { return m_items; }

QString BookmarksManager::addBookmark(const QString& pattern,
                                     const QString& connection, int dbIndex,
                                     const QString& name, const QString& note) {
  const QString normalizedPattern = pattern.trimmed();

  if (normalizedPattern.isEmpty()) {
    emit message(tr("Pattern is empty"), true);
    return QStringLiteral("empty_pattern");
  }

  if (contains(connection, dbIndex, normalizedPattern)) {
    emit message(tr("Bookmark already exists: %1").arg(normalizedPattern), true);
    return QStringLiteral("duplicate");
  }

  QVariantMap item;
  item["id"] = QUuid::createUuid().toString(QUuid::WithoutBraces);
  item["name"] = name.trimmed().isEmpty() ? defaultName(normalizedPattern)
                                          : name.trimmed();
  item["note"] = note.trimmed();
  item["pattern"] = normalizedPattern;
  item["connection"] = connection;
  item["db"] = dbIndex;
  item["createdAt"] = QDateTime::currentDateTime().toString(Qt::ISODate);

  m_items.append(item);

  save();
  emit changed();
  emit message(tr("Bookmark added: %1").arg(normalizedPattern), false);

  return okResult();
}

QString BookmarksManager::updateBookmark(const QString& id, const QString& name,
                                        const QString& note,
                                        const QString& pattern) {
  const int pos = indexOfId(id);

  if (pos < 0) return QStringLiteral("not_found");

  const QString normalizedPattern = pattern.trimmed();

  if (normalizedPattern.isEmpty()) {
    emit message(tr("Pattern is empty"), true);
    return QStringLiteral("empty_pattern");
  }

  QVariantMap item = m_items.at(pos).toMap();

  if (item.value("pattern").toString() != normalizedPattern &&
      contains(item.value("connection").toString(), item.value("db").toInt(),
               normalizedPattern)) {
    emit message(tr("Bookmark already exists: %1").arg(normalizedPattern), true);
    return QStringLiteral("duplicate");
  }

  item["name"] = name.trimmed().isEmpty() ? defaultName(normalizedPattern)
                                          : name.trimmed();
  item["note"] = note.trimmed();
  item["pattern"] = normalizedPattern;

  m_items.replace(pos, item);

  save();
  emit changed();
  emit message(tr("Bookmark updated"), false);

  return okResult();
}

QString BookmarksManager::removeBookmark(const QString& id) {
  const int pos = indexOfId(id);

  if (pos < 0) return QStringLiteral("not_found");

  m_items.removeAt(pos);

  save();
  emit changed();
  emit message(tr("Bookmark removed"), false);

  return okResult();
}

QString BookmarksManager::moveBookmark(const QString& id, int delta) {
  const int pos = indexOfId(id);

  if (pos < 0) return QStringLiteral("not_found");

  const int newPos = pos + delta;

  if (newPos < 0 || newPos >= m_items.size()) return okResult();

  m_items.move(pos, newPos);

  save();
  emit changed();

  return okResult();
}

bool BookmarksManager::contains(const QString& connection, int dbIndex,
                                const QString& pattern) const {
  for (const QVariant& rawItem : m_items) {
    const QVariantMap item = rawItem.toMap();

    if (item.value("connection").toString() == connection &&
        item.value("db").toInt() == dbIndex &&
        item.value("pattern").toString() == pattern) {
      return true;
    }
  }

  return false;
}

QVariantMap BookmarksManager::bookmark(const QString& id) const {
  const int pos = indexOfId(id);

  if (pos < 0) return QVariantMap();

  return m_items.at(pos).toMap();
}

void BookmarksManager::load() {
  QSettings settings;

  m_items.clear();

  const QByteArray raw = settings.value(kSettingsKey).toByteArray();

  if (raw.isEmpty()) return;

  QJsonDocument doc = QJsonDocument::fromJson(raw);

  if (!doc.isArray()) return;

  for (const QJsonValue& value : doc.array()) {
    if (!value.isObject()) continue;

    QVariantMap item = value.toObject().toVariantMap();

    if (!item.contains("pattern") || !item.contains("id")) continue;

    if (!item.contains("connection")) item["connection"] = QString();

    if (!item.contains("db")) item["db"] = 0;

    if (!item.contains("note")) item["note"] = QString();

    if (item.value("name").toString().isEmpty()) {
      item["name"] = defaultName(item.value("pattern").toString());
    }

    m_items.append(item);
  }
}

void BookmarksManager::save() {
  QJsonArray array;

  for (const QVariant& rawItem : m_items) {
    array.append(QJsonObject::fromVariantMap(rawItem.toMap()));
  }

  QSettings settings;

  settings.setValue(kSettingsKey,
                    QJsonDocument(array).toJson(QJsonDocument::Compact));
  settings.sync();
}

int BookmarksManager::indexOfId(const QString& id) const {
  for (int index = 0; index < m_items.size(); ++index) {
    if (m_items.at(index).toMap().value("id").toString() == id) return index;
  }

  return -1;
}

QString BookmarksManager::defaultName(const QString& pattern) {
  QString name = pattern;

  if (name.endsWith('*')) name.chop(1);

  if (!name.isEmpty()) name.chop(1);  // drop namespace separator

  const int pos = name.lastIndexOf(':');

  if (pos >= 0) name = name.mid(pos + 1);

  return name.isEmpty() ? pattern : name;
}
