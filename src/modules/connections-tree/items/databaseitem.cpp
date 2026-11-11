#include "databaseitem.h"

#include <QCoreApplication>
#include <QDebug>
#include <QFutureWatcher>
#include <QInputDialog>
#include <QMenu>
#include <QMessageBox>
#include <QSettings>
#include <QTimer>
#include <algorithm>
#include <functional>
#include <typeinfo>

#include "app/applog.h"
#include "app/apputils.h"
#include "connections-tree/model.h"
#include "connections-tree/utils.h"
#include "keyitem.h"
#include "loadmoreitem.h"
#include "namespaceitem.h"
#include "serveritem.h"

using namespace ConnectionsTree;

DatabaseItem::DatabaseItem(unsigned int index, int keysCount,
                           QSharedPointer<Operations> operations,
                           QWeakPointer<TreeItem> parent, Model& model)
    : AbstractNamespaceItem(model, parent, operations, index),
      m_keysCount(keysCount) {}

DatabaseItem::~DatabaseItem() {}

QByteArray DatabaseItem::getName() const { return QByteArray(); }

QByteArray DatabaseItem::getFullPath() const { return QByteArray(); }

QString DatabaseItem::getDisplayName() const {
  QString filter = m_filter.pattern() == "*"
                       ? ""
                       : QString("[filter: %1]").arg(m_filter.pattern());

  QString baseString = QString("db%1").arg(m_dbIndex);

  baseString.append(loadingProgressSuffix());

  if (m_keysTruncated) {
    baseString.append(QString(" %1").arg(QCoreApplication::translate(
                                             "RESP", "[limited]")));
  }

  if (m_usedMemory > 0) {
    baseString.append(
        QString(" <b>[%1]</b>").arg(humanReadableSize(m_usedMemory)));
  }

  if (m_operations->mode() == "cluster") {
    return QString("%1 %2").arg(baseString).arg(filter);
  } else {
    return QString("%1 %2 (%3)").arg(baseString).arg(filter).arg(m_keysCount);
  }
}

bool DatabaseItem::isEnabled() const { return true; }

void DatabaseItem::loadKeys(std::function<void()> callback,
                            bool partialReload) {
  lock();

  const int seq = ++m_loadSeq;

  if (!partialReload) m_keysTruncated = false;

  /*
   * Reload/live update must not be served from the cached page.
   */
  const bool bypassCache = m_bypassCache || partialReload;
  m_bypassCache = false;

  QString filter = (m_filter.isEmpty()) ? "" : m_filter.pattern();

  auto self = getSelf().toStrongRef();

  if (!self) {
    unlock();
    return;
  }

  auto dbLoadCallback = QSharedPointer<Operations::GetDatabasesCallback>(
      new Operations::GetDatabasesCallback(
          getSelf(), [this](QMap<int, int> dbMapping, const QString& err) {
            if (err.size() > 0) {
              unlock();
              emit m_model.error(QCoreApplication::translate(
                                     "RESP", "Cannot load databases:\n\n") +
                                 err);
              return;
            }

            if (dbMapping.contains(m_dbIndex)) {
              m_keysCount = dbMapping[m_dbIndex];
              emit m_model.itemChanged(getSelf());
            }
          }));

  AppLog::put(QString("DB loadKeys() seq=%1 db=%2 filter=%3 partialReload=%4")
                  .arg(seq)
                  .arg(m_dbIndex)
                  .arg(filter)
                  .arg(partialReload));

  m_operations->getDatabases(dbLoadCallback);

  auto onKeysRendered = QSharedPointer<RenderRawKeysCallback>(
      new RenderRawKeysCallback(getSelf(), [this, callback, seq]() {
        m_loadFinishedSeq = seq;
        ensureLoaderIsCreated();
        unlock();

        if (!isExpanded()) {
          setExpanded(true);
          m_model.expandItem(getSelf());
        }

        emit m_model.itemChanged(getSelf());

        if (callback) {
          callback();
        }

        /*
         * "Load all": the whole keyspace arrived in one page - start rendering
         * it in background chunks (with progress) instead of asking the user
         * to click "Load more keys".
         */
        if (m_loadAll) startAutoRender();
      }));

  auto nsItemsCallback = QSharedPointer<Operations::LoadNamespaceItemsCallback>(
      new Operations::LoadNamespaceItemsCallback(
          getSelf(), [this, onKeysRendered, partialReload, seq](
                         const Operations::KeysPage& page,
                         const QString& err) {
            m_loadFinishedSeq = seq;
            endScanProgress();
            m_loadedKeysCount = page.keys.size();
            m_lastPageKeys = static_cast<long>(page.keys.size());

            AppLog::put(QString("DB keys returned seq=%1 db=%2 keys=%3 next=%4 err=%5")
                            .arg(seq)
                            .arg(m_dbIndex)
                            .arg(page.keys.size())
                            .arg(QString::fromUtf8(page.nextCursor))
                            .arg(err.isEmpty() ? QString("<none>") : err));

            if (!err.isEmpty()) {
              m_nextCursor = QByteArray("0");
              unlock();
              return showLoadingError(err);
            }

            /*
             * Interrupted SCAN session cursor: "Load more keys" continues
             * the traversal from it (real pagination).
             */
            m_nextCursor = page.nextCursor;

            m_keysTruncated = m_loadAll
                                  ? false
                                  : (page.nextCursor != QByteArray("0") ||
                                     isKeysListTruncated(page.keys.size()));

            /*
             * "Load all" delivers the whole keyspace: only the first screen is
             * rendered synchronously (the tree has to stay responsive), the
             * remaining keys are rendered in background chunks afterwards
             * (startAutoRender / m_rawChildKeys).
             */
            int firstScreenLimit = -1;

            if (m_loadAll) {
              firstScreenLimit = qBound(
                  200,
                  QSettings()
                      .value("app/loadAllFirstScreen", 2000)
                      .toInt(),
                  20000);
            }

            return renderRawKeys(page.keys, m_filter, onKeysRendered,
                                 !partialReload, partialReload,
                                 firstScreenLimit);
          }));

  Operations::KeysLoadOptions options;
  /*
   * Aggregated scan (exact child namespaces counters) for the first screen,
   * bounded continuation pages for "Load more keys".
   */
  /*
   * Aggregated scan (full traversal, exact counters of every child) is
   * bandwidth expensive on a big keyspace, so it is opt-in. By default the
   * first screen is bounded by keysLoadLimit - and when the filter is sparse
   * the traversal reaches the end of the keyspace anyway, which already
   * gives exact counters.
   */
  options.aggregate = QSettings().value("app/scanAggregate", false).toBool();
  options.useCache = !bypassCache;
  options.loadAll = m_loadAll;

  auto selfWPtr = getSelf();
  options.progress = [this, selfWPtr](long scannedKeys) {
    if (!selfWPtr.toStrongRef()) return;
    notifyLoadingProgress(scannedKeys);
  };

  beginScanProgress(m_loadAll);
  startLoadWatchdog(seq);

  try {
    m_operations->loadNamespaceItems(m_dbIndex, filter, nsItemsCallback,
                                     options);
  } catch (const RedisClient::Connection::Exception& error) {
    m_loadFinishedSeq = seq;
    endScanProgress();
    unlock();
    showLoadingError(error.what());
  }
}

bool DatabaseItem::isKeysListTruncated(int keysCount) const {
  qlonglong limit = m_operations->keysLoadLimit();

  return limit > 0 && keysCount >= limit;
}

void DatabaseItem::startLoadWatchdog(int seq) {
  startProgressWatchdog(
      seq, [this, seq]() { return seq != m_loadSeq || m_loadFinishedSeq == seq; },
      QString("DB[%1]").arg(m_dbIndex));
}

long DatabaseItem::loadingTotalKeys() const {
  /*
   * INFO keyspace is refreshed on every loadKeys(), so the size of the
   * database is the natural total for the scan percentage. With an active
   * filter the scan returns a subset, so the percentage is not meaningful
   * (progress is reported as an amount of scanned keys).
   */
  if (!m_filter.isEmpty()) return 0;

  return static_cast<long>(m_keysCount);
}

QVariantMap DatabaseItem::metadata() const {
  QVariantMap metadata = AbstractNamespaceItem::metadata();
  metadata["filter"] = m_filter.pattern();
  metadata["filterHistory"] = filterHistoryTop10();
  metadata["live_update"] = isLiveUpdateEnabled();
  metadata["user_color"] = m_operations->iconColor();
  return metadata;
}

void DatabaseItem::setMetadata(const QString& key, QVariant value) {
  bool isResetValue = (value.isNull() || !value.canConvert<QString>() ||
                       value.toString().isEmpty());

  if (key == "filter") {
    if (!m_filter.isEmpty() && isResetValue)
      return resetFilter();
    else if (isResetValue)
      return;

    auto applyFilter = [this, value]() {
      QRegExp pattern(value.toString(), Qt::CaseSensitive,
                      QRegExp::PatternSyntax::WildcardUnix);
      filterKeys(pattern);
    };

    QByteArray val = value.toByteArray();

    if (val.contains('*')) {
      return applyFilter();
    }

    auto selfWPtr = getSelf();

    auto openKeyCallback = QSharedPointer<Operations::OpenKeyIfExistsCallback>(
        new Operations::OpenKeyIfExistsCallback(
            selfWPtr, [applyFilter](const QString&, bool result) {
              if (!result) {
                applyFilter();
              }
            }));

    auto self = selfWPtr.toStrongRef();

    if (!self) return;

    m_operations->openKeyIfExists(val, self.dynamicCast<DatabaseItem>(),
                                  openKeyCallback);

    return;
  } else if (key == "live_update") {
    if (liveUpdateTimer()->isActive() && isResetValue) {
      qDebug() << "Stop live update";
      liveUpdateTimer()->stop();
    } else {
      qDebug() << "Start live update";
      liveUpdateTimer()->start();
    }

    emit m_model.itemChanged(getSelf());
  }
}

void DatabaseItem::getMemoryUsage(std::function<void(qlonglong)> callback) {
  if (m_childItems.size() == 0) {
    auto d = QSharedPointer<AsyncFuture::Deferred<qlonglong>>(
        new AsyncFuture::Deferred<qlonglong>());
    loadKeys([this, callback]() {
      lock();
      AbstractNamespaceItem::getMemoryUsage(callback);
    });
  } else {
    AbstractNamespaceItem::getMemoryUsage(callback);
  }
}

void DatabaseItem::unload(bool notify) {
  if (m_childItems.size() == 0) return;

  lock();
  clear();

  m_keysCount = 0;

  if (notify) m_operations->notifyDbWasUnloaded(m_dbIndex);

  unlock();
}

void DatabaseItem::reload(std::function<void()> callback) {
  /*
   * Regular reload (Ctrl+R / filter change / live update): back to the
   * bounded first screen.
   */
  m_loadAll = false;

  clear();
  m_nextCursor = QByteArray("0");
  m_bypassCache = true;
  loadKeys([this, callback]() {
    QSettings settings;
    m_model.expandedNamespaces.clear();

    if (settings.value("app/reopenNamespacesOnReload", true).toBool()) {
      auto self = getSelf().toStrongRef();

      if (!self) return;

      restoreOpenedNamespaces(self.staticCast<AbstractNamespaceItem>());
    }

    if (callback) callback();
  });
}

void DatabaseItem::loadAllKeys() {
  AppLog::put(QString("DB loadAllKeys() db=%1 filter=%2 keysCount=%3")
                  .arg(m_dbIndex)
                  .arg(m_filter.pattern())
                  .arg(m_keysCount));

  /*
   * Same pipeline as a regular reload, but without any loading limit: the
   * whole (filtered) keyspace is requested in one page and rendered in
   * background chunks afterwards.
   */
  m_loadAll = true;

  clear();
  m_nextCursor = QByteArray("0");
  m_bypassCache = true;
  loadKeys();
}

void DatabaseItem::loadNextPage() {
  const QByteArray startCursor = m_nextCursor;

  if (startCursor == QByteArray("0")) return;

  QString filter = (m_filter.isEmpty()) ? "" : m_filter.pattern();

  AppLog::put(QString("DB loadNextPage db=%1 filter=%2 cursor=%3")
                  .arg(m_dbIndex)
                  .arg(filter)
                  .arg(QString::fromUtf8(startCursor)));

  lock();
  beginScanProgress();

  Operations::KeysLoadOptions options;
  /*
   * Continuation page: bounded scan, aggregation limit is not applied again
   * (otherwise "Load more" would repeat the whole scan).
   */
  options.aggregate = false;
  options.useCache = true;
  options.startCursor = startCursor;

  auto selfWPtr = getSelf();
  options.progress = [this, selfWPtr](long scannedKeys) {
    if (!selfWPtr.toStrongRef()) return;
    notifyLoadingProgress(scannedKeys);
  };

  auto callback = QSharedPointer<Operations::LoadNamespaceItemsCallback>(
      new Operations::LoadNamespaceItemsCallback(
          getSelf(), [this, startCursor](const Operations::KeysPage& page,
                                         const QString& err) {
            clearLoader();
            endScanProgress();

            if (!err.isEmpty()) {
              unlock();
              return showLoadingError(err);
            }

            m_nextCursor = page.nextCursor;

            AppLog::put(QString("DB page loaded db=%1 cursor=%2 keys=%3 next=%4")
                            .arg(m_dbIndex)
                            .arg(QString::fromUtf8(startCursor))
                            .arg(page.keys.size())
                            .arg(QString::fromUtf8(page.nextCursor)));

            if (page.keys.isEmpty()) {
              ensureLoaderIsCreated();
              unlock();
              return emit m_model.itemChanged(getSelf());
            }

            const int renderLimit = static_cast<int>(m_childItems.size()) +
                                    static_cast<int>(keysRenderingLimit());

            auto rendered = QSharedPointer<RenderRawKeysCallback>(
                new RenderRawKeysCallback(getSelf(), [this]() {
                  ensureLoaderIsCreated();
                  emit m_model.itemChanged(getSelf());
                  unlock();
                }));

            renderRawKeys(page.keys, m_filter, rendered, true, false,
                          renderLimit);
          }));

  try {
    m_operations->loadNamespaceItems(m_dbIndex, filter, callback, options);
  } catch (const RedisClient::Connection::Exception& error) {
    endScanProgress();
    unlock();
    showLoadingError(error.what());
  }
}

void DatabaseItem::performLiveUpdate() {
  qDebug() << "Live update loading keys...";

  if (isLocked()) {
    qDebug()
        << "Another loading operation is in progress. Skip this live update...";
    liveUpdateTimer()->start();
    return;
  }

  m_rawChildKeys.clear();

  loadKeys(
      [this]() {
        QSettings settings;
        if (m_childItems.size() >=
            settings.value("app/liveUpdateKeysLimit", 1000).toInt()) {
          liveUpdateTimer()->stop();

          emit m_model.itemChanged(getSelf());

          QMessageBox::warning(
              nullptr,
              QCoreApplication::translate("RESP", "Live update was disabled"),
              QCoreApplication::translate(
                  "RESP",
                  "Live update was disabled due to exceeded keys limit. "
                  "Please specify filter more carefully or change limit in "
                  "settings."));
        } else {
          liveUpdateTimer()->start();
          emit m_model.itemChanged(getSelf());
        }
      },
      true);
}

void DatabaseItem::filterKeys(const QRegExp& filter) {
  m_filter = filter;
  emit m_model.itemChanged(getSelf());
  reload();
}

void DatabaseItem::resetFilter() {
  m_filter = QRegExp(m_operations->defaultFilter());
  emit m_model.itemChanged(getSelf());
  reload();
}

QHash<QString, std::function<bool ()> > DatabaseItem::eventHandlers() {
  auto events = AbstractNamespaceItem::eventHandlers();

  events.insert("click", [this]() {
    if (m_childItems.size() != 0) {
      if (!isExpanded()) {
        setExpanded(true);
        m_model.expandItem(getSelf());
      }
      return true;
    }

    loadKeys();
    return false;
  });

  events.insert("right-click", [this]() {
    if (m_childItems.size() != 0) return true;

    emit m_model.itemChanged(getSelf());
    return true;
  });

  events.insert("add_key", [this]() {
    auto callback = QSharedPointer<Operations::OpenNewKeyDialogCallback>(
        new Operations::OpenNewKeyDialogCallback(getSelf(), [this]() {
          confirmAction(
              nullptr,
              QCoreApplication::translate(
                  "RESP",
                  "Key was added. Do you want to reload keys in "
                  "selected database?"),
              [this]() {
                reload();
                m_keysCount++;
              },
              QCoreApplication::translate("RESP", "Key was added"));
        }));

    m_operations->openNewKeyDialog(m_dbIndex, callback);
    return true;
  });

  events.insert("reload", [this]() {
    reload();
    return false;
  });

  events.insert("load_all", [this]() {
    loadAllKeys();
    return false;
  });

  events.insert("flush", [this]() {
    confirmAction(
        nullptr,
        QCoreApplication::translate(
            "RESP",
            "Do you really want to remove all keys from this database?"),
        [this]() {
          auto callback = QSharedPointer<Operations::FlushDbCallback>(
              new Operations::FlushDbCallback(
                  getSelf(), [this](const QString&) { unload(); }));
          m_operations->flushDb(m_dbIndex, callback);
        });
    return true;
  });

  events.insert("console",
                [this]() { m_operations->openConsoleTab(m_dbIndex); return true; });

  events.insert("delete_keys", [this]() { m_operations->deleteDbKeys(*this); return true; });

  events.insert("copy_keys", [this]() { m_operations->copyKeys(*this); return true; });

  events.insert("rdb_import",
                [this]() { m_operations->importKeysFromRdb(*this); return true; });

  events.insert("ttl", [this]() { m_operations->setTTL(*this); return true; });

  return events;
}

QSharedPointer<QTimer> DatabaseItem::liveUpdateTimer() {
  if (!m_liveUpdateTimer) {
    QSettings settings;
    m_liveUpdateTimer = QSharedPointer<QTimer>(new QTimer());
    m_liveUpdateTimer->setInterval(
        settings.value("app/liveUpdateInterval", 10).toInt() * 1000);

    qDebug() << "Live update timer"
             << settings.value("app/liveUpdateInterval", 10).toInt() * 1000;

    m_liveUpdateTimer->setSingleShot(true);

    QObject::connect(m_liveUpdateTimer.data(), &QTimer::timeout,
                     [this]() { performLiveUpdate(); });
  }

  return m_liveUpdateTimer;
}

bool DatabaseItem::isLiveUpdateEnabled() const {
  return m_liveUpdateTimer && m_liveUpdateTimer->isActive();
}

// Top 10 filters
QVariantList DatabaseItem::filterHistoryTop10() const {
  typedef QPair<QString, int> FilterUsage;

  QList<FilterUsage> filterHistoryRating;
  QVariantList filterHistoryList;
  auto server = parent().toStrongRef();

  if (!server || !server.staticCast<ServerItem>()) return filterHistoryList;

  QVariantMap filterHistory = m_operations->getFilterHistory();
  QVariantMap::const_iterator i(filterHistory.begin());

  while (i != filterHistory.end()) {
    FilterUsage filterUsage;
    filterUsage.first = i.key();
    filterUsage.second = i.value().toInt();
    filterHistoryRating.append(filterUsage);
    ++i;
  }
  std::sort(filterHistoryRating.begin(), filterHistoryRating.end(),
            [](FilterUsage i, FilterUsage j) { return (i.second > j.second); });

  for (int i = 0; filterHistoryRating.size() > 0; i++) {
    if (i >= 10) break;
    filterHistoryList.append(filterHistoryRating.takeFirst().first);
  }
  return filterHistoryList;
}
