#pragma once

#include <QObject>
#include <QVariantList>
#include <QVariantMap>

/*
 * BookmarksManager - persistent list of key-pattern shortcuts.
 *
 * A bookmark stores a (connection, db index, pattern) tuple, e.g.
 *   connection: "prod-cloudflare", db: 0, pattern: "A0_CLOUDFLARE5S:*"
 *
 * Storage is QSettings (the same place where other application settings
 * are stored), the whole list is serialized as a JSON array.
 */
class BookmarksManager : public QObject {
  Q_OBJECT
  Q_PROPERTY(int count READ count NOTIFY changed)

 public:
  explicit BookmarksManager(QObject* parent = nullptr);

  int count() const;

  Q_INVOKABLE QVariantList items() const;

  /*
   * All the methods below return an empty string on success or an error
   * code ("empty_pattern", "duplicate", "not_found") on failure.
   * Detailed feedback is delivered through the "message" signal.
   */
  Q_INVOKABLE QString addBookmark(const QString& pattern,
                                  const QString& connection, int dbIndex,
                                  const QString& name, const QString& note);

  Q_INVOKABLE QString updateBookmark(const QString& id, const QString& name,
                                     const QString& note,
                                     const QString& pattern);

  Q_INVOKABLE QString removeBookmark(const QString& id);

  Q_INVOKABLE QString moveBookmark(const QString& id, int delta);

  Q_INVOKABLE bool contains(const QString& connection, int dbIndex,
                            const QString& pattern) const;

  Q_INVOKABLE QVariantMap bookmark(const QString& id) const;

 signals:
  void changed();

  void message(const QString& text, bool isError);

 private:
  void load();
  void save();

  int indexOfId(const QString& id) const;

  static QString defaultName(const QString& pattern);

  QVariantList m_items;
};
