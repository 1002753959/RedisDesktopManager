#include "treeoperations.h"

#include <asyncfuture.h>
#include <qredisclient/redisclient.h>
#include <QDateTime>
#include <QTimer>
#include <QRegExp>
#include <QRegularExpression>
#include <QRegularExpressionMatchIterator>
#include <QSet>
#include <QtConcurrent>
#include <algorithm>

#include "app/applog.h"
#include "app/events.h"
#include "connections-tree/items/serveritem.h"
#include "connections-tree/items/databaseitem.h"
#include "connections-tree/items/namespaceitem.h"
#include "connections-tree/keysrendering.h"

TreeOperations::TreeOperations(const ServerConfig &config,
    QSharedPointer<Events> events)
    : m_events(events), m_dbCount(0),
      m_connectionMode(RedisClient::Connection::Mode::Normal),
      m_config(config){
  m_connection = QSharedPointer<RedisClient::Connection>(
              new RedisClient::Connection(config));
  m_events->registerLoggerForConnection(*m_connection);

  // Kept only for the head-less diagnostic run: allows reproducing the very
  // same "load namespace keys" pipeline without interacting with the tree.
  //   RESP_KEYS_SELFTEST="<dbIndex>|<filter>" RESP_KEYS_SELFTEST_CONN="<name>"
  const QString selfTest = qEnvironmentVariable("RESP_KEYS_SELFTEST");
  const QString selfTestConn = qEnvironmentVariable("RESP_KEYS_SELFTEST_CONN");

  if (!selfTest.isEmpty() &&
      (selfTestConn.isEmpty() || config.name() == selfTestConn)) {
    const QStringList parts = selfTest.split("|");
    const uint dbIndex = parts.value(0).toUInt();
    const QString filter = parts.value(1);

    /*
     * Optional parts:
     *   parts[2] - "full" disables the aggregated scan (first screen mode)
     *   parts[3] - SCAN cursor to continue a previous session (pagination)
     *   "loadall" - anywhere in the string triggers the load-all mode
     *               (dedicated inline menu button)
     */
    KeysLoadOptions selftestOptions;
    selftestOptions.aggregate = parts.value(2).compare("full", Qt::CaseInsensitive) != 0;
    selftestOptions.startCursor = parts.value(3).toLatin1();
    selftestOptions.loadAll =
        selfTest.contains("loadall", Qt::CaseInsensitive);

    AppLog::put(QString("SELFTEST scheduled for conn=%1 db=%2 filter=%3 "
                        "aggregate=%4 cursor=%5")
                    .arg(config.name())
                    .arg(dbIndex)
                    .arg(filter)
                    .arg(selftestOptions.aggregate)
                    .arg(parts.value(3)));

    const bool cacheProbe =
        qEnvironmentVariableIntValue("RESP_KEYS_SELFTEST_CACHE") > 0;

    AppLog::put(QString("SELFTEST parts=%1 cacheProbe=%2")
                    .arg(parts.join(","))
                    .arg(cacheProbe));

    QTimer::singleShot(6000, this, [this, dbIndex, filter, selftestOptions,
                                    cacheProbe]() {
      AppLog::put(QString("SELFTEST start db=%1 filter=%2")
                      .arg(dbIndex)
                      .arg(filter));

      auto cb = QSharedPointer<LoadNamespaceItemsCallback>(
          new LoadNamespaceItemsCallback(
              QWeakPointer<ConnectionsTree::TreeItem>(),
              [](const KeysPage &page, const QString &err) {
                AppLog::put(QString("SELFTEST done keys=%1 nextCursor=%2 err=%3")
                                .arg(page.keys.size())
                                .arg(QString::fromUtf8(page.nextCursor))
                                .arg(err.isEmpty() ? QString("<none>") : err));
                QCoreApplication::quit();
              }));

      loadNamespaceItems(dbIndex, filter, cb, selftestOptions);

      if (!cacheProbe) return;

      /*
       * Cache probe: the very same page is requested again after the first
       * one has been cached - the second call must be served from the
       * in-memory cache (log line "cache HIT") without any SCAN round-trip.
       */
      QTimer::singleShot(5000, this, [this, dbIndex, filter, selftestOptions]() {
        AppLog::put("SELFTEST cache probe request fired");

        auto cb2 = QSharedPointer<LoadNamespaceItemsCallback>(
            new LoadNamespaceItemsCallback(
                QWeakPointer<ConnectionsTree::TreeItem>(),
                [](const KeysPage &, const QString &) {}));

        loadNamespaceItems(dbIndex, filter, cb2, selftestOptions);
      });
    });
  }
}

void TreeOperations::loadDatabases(
    QSharedPointer<RedisClient::Connection> c,
    QSharedPointer<AsyncFuture::Deferred<void>> d,
    std::function<void(RedisClient::DatabaseList, const QString&)> callback) {
  if (!d) return;

  auto connection = c->clone(false);
  auto connectionWRef = connection.toWeakRef();

  d->onCanceled([connectionWRef]() {
    QtConcurrent::run([connectionWRef]() {
      auto connection = connectionWRef.toStrongRef();
      if (connection) connection->disconnect();
    });
  });

  m_events->registerLoggerForConnection(*connection);

  if (!connect(connection)) {
    return callback(RedisClient::DatabaseList(),
                    QString("Cannot connect to redis-server"));
  }

  if (d && d->future().isCanceled()) {
    return;
  }

  RedisClient::DatabaseList availableDatabeses = connection->getKeyspaceInfo();

  if (connection->mode() == RedisClient::Connection::Mode::Cluster) {
    return callback(availableDatabeses, QString());
  }

  // detect all databases
  RedisClient::Response scanningResp;
  int lastDbIndex =
      (availableDatabeses.size() == 0) ? 0 : availableDatabeses.lastKey() + 1;

  if (m_dbCount > 0) {
    for (int index = lastDbIndex; index < m_dbCount; index++) {
      availableDatabeses.insert(index, 0);
    }

    return callback(availableDatabeses, QString());
  } else {
    m_dbCount = lastDbIndex;

    auto collectedDatabases = QSharedPointer<RedisClient::DatabaseList>(
        new RedisClient::DatabaseList(availableDatabeses));

    recursiveSelectScan(d, connection, collectedDatabases, callback);
  }
}

void TreeOperations::recursiveSelectScan(
    QSharedPointer<AsyncFuture::Deferred<void>> d,
    QSharedPointer<RedisClient::Connection> c,
    QSharedPointer<RedisClient::DatabaseList> dbList,
    std::function<void(RedisClient::DatabaseList, const QString&)> callback) {
  if (d && d->future().isCanceled()) {
    return;
  }

  if (m_dbCount >= m_config.databaseScanLimit() || !c) {
    return callback(*dbList, QString());
  }

  auto errHandler = [callback, dbList](const QString& err) {
    if (dbList && dbList->size() > 0) {
      callback(*dbList, QString());
    } else {
      callback(RedisClient::DatabaseList(), err);
    }
  };

  c->cmd(
      {"select", QString::number(m_dbCount).toLatin1()}, this, -1,
      [this, dbList, c, callback, d](const RedisClient::Response& scanningResp) {
        if (d && d->future().isCanceled()) {
          return;
        }

        if (!scanningResp.isOkMessage()) {
          callback(*dbList, QString());
          return;
        }

        dbList->insert(m_dbCount, 0);
        m_dbCount++;

        recursiveSelectScan(d, c, dbList, callback);
      },
      errHandler);
}

bool TreeOperations::connect(QSharedPointer<RedisClient::Connection> c) {
  if (c->isConnected()) return true;

  try {
    if (!c->connect(true)) {
      emit m_events->error(
          QCoreApplication::translate(
              "RESP", "Cannot connect to server '%1'. Check log for details.")
              .arg(m_connection->getConfig().name()));
      return false;
    }

    m_connectionMode = c->mode();
    return true;
  } catch (const RedisClient::Connection::SSHSupportException& e) {
      emit m_events->error(
          QCoreApplication::translate("RESP", "Open Source version of RESP.app <b>doesn't support SSH tunneling</b>.<br /><br /> "
                                             "To get fully-featured application, please buy subscription on "
                                             "<a href='https://resp.app/subscriptions'>resp.app</a>. <br/><br />"
                                             "Every single subscription gives us funds to continue "
                                             "the development process and provide support to our users. <br />"
                                             "If you have any questions please feel free to contact us "
                                             "at <a href='mailto:support@resp.app'>support@resp.app</a> "
                                             "or join <a href='https://t.me/RedisDesktopManager'>Telegram chat</a>.")
      );
      return false;
  } catch (const RedisClient::Connection::Exception& e) {
    emit m_events->error(
        QCoreApplication::translate("RESP", "Connection error: ") +
        QString(e.what()));
    return false;
  }
}

void TreeOperations::requestBulkOperation(
    ConnectionsTree::AbstractNamespaceItem& ns,
    BulkOperations::Manager::Operation op,
    BulkOperations::AbstractOperation::OperationCallback callback) {
  QString pattern =
      QString("%1%2*")
          .arg(QString::fromUtf8(ns.getFullPath()))
          .arg(ns.getFullPath().size() > 0 ? m_config.namespaceSeparator()
                                           : "");
  QRegExp filter(pattern, Qt::CaseSensitive, QRegExp::Wildcard);

  auto dbIndex = ns.getDbIndex();

  getReadyConnection([this, dbIndex, filter, op,
                      callback](QSharedPointer<RedisClient::Connection> c) {
    // NOTE(u_glide): Use "clean" connection wihout logger here for better
    // performance
    emit m_events->requestBulkOperation(c->clone(), dbIndex, op, filter,
                                        callback);
  });
}

void TreeOperations::getReadyConnection(TreeOperations::PendingOperation callback)
{
    if (m_config.askForSshPassword() && m_config.sshPassword().isEmpty()) {
        m_pendingOperation = callback;
        m_config.setOwner(sharedFromThis().toWeakRef());
        emit secretRequired(m_config, ServerConfig::SSH_SECRET_ID);
    } else {
        return callback(m_connection);
    }
}

QFuture<void> TreeOperations::getDatabases(
    QSharedPointer<GetDatabasesCallback> callback) {
  m_dbScanOp = QSharedPointer<AsyncFuture::Deferred<void>>(
      new AsyncFuture::Deferred<void>());

  getReadyConnection(
      [this, callback](QSharedPointer<RedisClient::Connection> c) {
        QtConcurrent::run(this, &TreeOperations::loadDatabases, c, m_dbScanOp,
                          [callback](RedisClient::DatabaseList dbs, const QString& err){
            callback->call(dbs, err);
        });
      });

  return m_dbScanOp->future();
}

void TreeOperations::loadNamespaceItems(
    uint dbIndex, const QString& filter,
    QSharedPointer<LoadNamespaceItemsCallback> callback,
    const KeysLoadOptions& options) {
  QString keyPattern = filter.isEmpty() ? m_config.keysPattern() : filter;

  if (m_filterHistory.contains(keyPattern)) {
    m_filterHistory[keyPattern] = m_filterHistory[keyPattern].toInt() + 1;
  } else {
    m_filterHistory[keyPattern] = 1;
  }
  m_config.setFilterHistory(m_filterHistory);
  emit filterHistoryUpdated();

  QSettings settings;

  /*
   * Hard limits for a single keys loading session:
   *
   *  - maxKeys        - maximum amount of keys returned to the tree in the
   *                     "first screen" (non aggregated) mode. Protects the
   *                     UI from loading millions of keys (huge db0).
   *  - maxIterations  - maximum amount of SCAN calls. Protects from a full
   *                     keyspace traversal when the filter matches almost
   *                     nothing (SCAN + MATCH is O(N) on the server side).
   *  - aggregateLimit - maximum amount of keys scanned by an aggregated
   *                     request (0 - unlimited).
   *
   * All values are configurable: connection settings / global settings.
   */
  /*
   * Pagination: the caller continues a previously interrupted scan session.
   */
  const bool paginated = options.startCursor != QByteArray("0");

  /*
   * "Load all": explicit user request to receive the whole filtered keyspace
   * in a single page. Every limit is dropped (keys limit, SCAN iterations
   * limit, aggregation limit) - the user asked for all keys, so the cost is
   * intentional. Continuation pages are never "load all" (they are always
   * bounded, see below).
   */
  const bool loadAll = options.loadAll && !paginated;

  const qlonglong maxKeys = m_config.keysLoadLimit();
  const qlonglong maxIterations =
      loadAll ? 0
              : settings.value("app/scanMaxIterations",
                               ServerConfig::DEFAULT_SCAN_MAX_ITERATIONS)
                    .toLongLong();

  /*
   * Aggregated mode is the default for the first page: the SCAN session is
   * executed until the end of the (filtered) keyspace, therefore every child
   * namespace receives an exact keys counter instead of being counted from
   * a truncated sample of 1000 keys.
   *
   * Continuation pages are always bounded by maxKeys, otherwise "Load more"
   * would perform exactly the same full scan again.
   */
  long scanLimit = 0;

  if (loadAll)
    scanLimit = 0;  // unlimited: the caller wants the whole keyspace
  else if (paginated)
    scanLimit = static_cast<long>(qMax<qlonglong>(maxKeys, 0));
  else if (options.aggregate)
    scanLimit = static_cast<long>(qMax<qlonglong>(scanAggregateLimit(), 0));
  else
    scanLimit = static_cast<long>(qMax<qlonglong>(maxKeys, 0));

  /*
   * Reload (useCache == false) drops every cached page of the database:
   * keys could be added/removed/renamed on the server side.
   */
  if (!options.useCache && !paginated)
    invalidateKeysCache(
        QString("%1|%2|").arg(connectionName()).arg(dbIndex));

  const QString cacheKey =
      keysCacheKey(dbIndex, keyPattern, options.startCursor);
  const qint64 requestId = QDateTime::currentMSecsSinceEpoch();

  if (options.useCache) {
    KeysPage cachedPage;

    if (lookupKeysCache(cacheKey, &cachedPage)) {
      AppLog::put(QString("REQ %1 cache HIT db=%2 pattern=%3 cursor=%4 keys=%5")
                      .arg(requestId)
                      .arg(dbIndex)
                      .arg(keyPattern)
                      .arg(QString::fromUtf8(options.startCursor))
                      .arg(cachedPage.keys.size()));

      callback->call(cachedPage, QString());
      return;
    }
  }

  AppLog::put(QString("REQ %1 loadNamespaceItems db=%2 pattern=%3 "
                      "aggregate=%4 cursor=%5 scanLimit=%6 maxIterations=%7 "
                      "loadAll=%8")
                  .arg(requestId)
                  .arg(dbIndex)
                  .arg(keyPattern)
                  .arg(options.aggregate)
                  .arg(QString::fromUtf8(options.startCursor))
                  .arg(scanLimit)
                  .arg(maxIterations)
                  .arg(loadAll));

  getReadyConnection(
      [this, dbIndex, callback, keyPattern, maxIterations, scanLimit,
       options, requestId, cacheKey](QSharedPointer<RedisClient::Connection> c) {
        AppLog::put(QString("REQ %1 connection ready after %2ms (connected=%3)")
                        .arg(requestId)
                        .arg(QDateTime::currentMSecsSinceEpoch() - requestId)
                        .arg(c && c->isConnected()));

        auto processErr = [callback, requestId](const QString& err) {
          AppLog::put(QString("REQ %1 FAILED: %2").arg(requestId).arg(err));
          return callback->call(KeysPage(), err);
        };

        /*
         * Report connection problem back to the tree item. Without it the item
         * stays locked forever ("endless loading" in the tree).
         */
        if (!connect(c)) {
          return processErr(QCoreApplication::translate(
              "RESP", "Cannot load keys: cannot connect to redis-server"));
        }

        /*
         * The cursor of the interrupted session is published by the client
         * and returned to the tree item, so that "Load more keys" is able to
         * continue the traversal instead of starting from scratch.
         */
        auto nextCursor = QSharedPointer<QByteArray>(new QByteArray("0"));

        /*
         * Amount of keys walked by the SCAN session (reported by the client
         * on every iteration). Returned to the tree item as the page
         * statistics.
         */
        auto scannedTotal = QSharedPointer<long>(new long(0));

        auto progressWrapper = [options, scannedTotal](long scannedKeys) {
          *scannedTotal = scannedKeys;
          if (options.progress) options.progress(scannedKeys);
        };

        auto callbackWrapper =
            [this, callback, requestId, cacheKey, options, nextCursor,
             scannedTotal](
                const RedisClient::Connection::RawKeysList& keys,
                const QString& err) {
              AppLog::put(
                  QString("REQ %1 SCAN DONE keys=%2 nextCursor=%3 err=%4 after %5ms")
                      .arg(requestId)
                      .arg(keys.size())
                      .arg(QString::fromUtf8(*nextCursor))
                      .arg(err.isEmpty() ? QString("<none>") : err)
                      .arg(QDateTime::currentMSecsSinceEpoch() - requestId));

              if (!err.isEmpty()) return callback->call(KeysPage(), err);

              KeysPage page;
              page.keys = keys;
              page.nextCursor = *nextCursor;
              page.scannedKeys = *scannedTotal;

              if (options.useCache) putKeysCache(cacheKey, page);

              return callback->call(page, QString());
            };

        try {
          /*
           * Loading is executed on a dedicated connection (clone):
           *
           *  - the keys scan is no longer serialized behind other tree
           *    operations which share the main connection (databases counter
           *    refresh, live update, value loading, previous scans). Before this
           *    change CTRL+R on a namespace could wait forever in the shared
           *    command queue of a huge database;
           *  - resetConnection()/disconnect() of the main connection (cancel of
           *    another operation) can't drop the running scan anymore, so the
           *    tree item always receives the result and gets unlocked.
           */
          auto loader = keysLoader(c);

          AppLog::put(QString("REQ %1 scanning on %2 connection")
                          .arg(requestId)
                          .arg(loader == c ? QString("MAIN")
                                           : QString("dedicated clone")));

          /*
           * Adaptive SCAN COUNT.
           *
           * The whole keys loading session needs about dbsize / COUNT
           * round-trips, and every round-trip costs a full network RTT
           * (60-140ms to a remote instance). With the old fixed hint (1000) a
           * 100k keys database meant ~100 sequential round-trips, i.e. ~15s
           * during which the tree looked stuck. Scaling COUNT with the
           * keyspace collapses the session into a handful of round-trips
           * (measured: 102 iterations / 14.5s -> ~10 iterations / ~0.8s on
           * the same database).
           */
          /*
           * Small initial COUNT: a dense filter fills the page limit within the
           * very first round-trip (fast first screen, no wasted bandwith).
           * Sparse filters are handled by the adaptive COUNT escalation inside
           * the client (x16 per round-trip, cap 20000), so a 100k keys
           * keyspace is still traversed in a handful of round-trips.
           *
           * Note: the hint is a constant on purpose - asking the server for
           * the keyspace size first (INFO) costs ~6s on a slow link, which is
           * far more than the scan itself.
           */
          long countHint = 0;

          if (loader->mode() != RedisClient::Connection::Mode::Cluster)
            countHint = 1000;

          /*
           * "Load all" walks the whole keyspace, so the initial COUNT is
           * raised to the adaptive ceiling right away: a 100k keys database
           * needs ~5 round-trips instead of ~100 (RTT dominates the scan on
           * a remote instance).
           */
          if (options.loadAll && options.startCursor == QByteArray("0"))
            countHint = 20000;

          AppLog::put(
              QString("REQ %1 scan countHint=%2 (scanLimit=%3 maxIterations=%4)")
                  .arg(requestId)
                  .arg(countHint)
                  .arg(scanLimit)
                  .arg(maxIterations));

          if (loader->mode() == RedisClient::Connection::Mode::Cluster) {
            loader->getClusterKeys(callbackWrapper, keyPattern, scanLimit,
                                   maxIterations, countHint);
          } else {
            loader->getDatabaseKeys(callbackWrapper, keyPattern, dbIndex,
                                    scanLimit, maxIterations, countHint,
                                    options.startCursor, nextCursor,
                                    progressWrapper);
          }

        } catch (const RedisClient::Connection::Exception& error) {
          processErr(error.what());
        }
      });
}

/*
 * Keys cache -----------------------------------------------------------------
 *
 * The tree reads the same namespace multiple times during a single working
 * session (expand -> collapse -> expand, switching between databases,
 * property panels). Every such read costs a full SCAN session on the server,
 * which is the most expensive operation for a remote instance. Cached pages
 * are keyed by connection + db + filter + cursor, are bounded by TTL and are
 * dropped on Reload.
 */

QString TreeOperations::keysCacheKey(uint dbIndex, const QString& filter,
                                    const QByteArray& cursor) const {
  return QString("%1|%2|%3|%4")
      .arg(connectionName())
      .arg(dbIndex)
      .arg(filter)
      .arg(QString::fromUtf8(cursor));
}

bool TreeOperations::lookupKeysCache(const QString& key, KeysPage* page) {
  auto it = m_keysCache.find(key);

  if (it == m_keysCache.end()) return false;

  QSettings settings;
  const qint64 ttl =
      settings.value("app/keysCacheTtlMs", 60000).toLongLong();

  if (ttl <= 0 ||
      QDateTime::currentMSecsSinceEpoch() - it->timestamp > ttl) {
    m_keysCache.remove(key);
    m_keysCacheOrder.removeAll(key);
    return false;
  }

  if (page) *page = it->page;

  // refresh LRU position
  m_keysCacheOrder.removeAll(key);
  m_keysCacheOrder.append(key);

  return true;
}

void TreeOperations::putKeysCache(const QString& key, const KeysPage& page) {
  QSettings settings;
  const int maxEntries = settings.value("app/keysCacheEntries", 16).toInt();

  if (maxEntries <= 0) return;

  CachedKeysPage entry;
  entry.page = page;
  entry.timestamp = QDateTime::currentMSecsSinceEpoch();

  m_keysCache[key] = entry;
  m_keysCacheOrder.removeAll(key);
  m_keysCacheOrder.append(key);

  while (m_keysCacheOrder.size() > maxEntries) {
    const QString oldest = m_keysCacheOrder.takeFirst();
    m_keysCache.remove(oldest);
  }

  /*
   * Memory budget: a single aggregated page may contain the whole filtered
   * keyspace (hundreds of thousands of keys), so the cache is additionally
   * bounded by the total amount of cached keys (LRU eviction).
   */
  const long maxCachedKeys =
      settings.value("app/keysCacheMaxKeys", 300000).toLongLong();

  if (maxCachedKeys <= 0) return;

  auto cachedKeysTotal = [this]() {
    long total = 0;
    for (auto it = m_keysCache.constBegin(); it != m_keysCache.constEnd();
         ++it) {
      total += it->page.keys.size();
    }
    return total;
  };

  while (!m_keysCacheOrder.isEmpty() &&
         cachedKeysTotal() > maxCachedKeys) {
    const QString oldest = m_keysCacheOrder.takeFirst();
    m_keysCache.remove(oldest);
  }
}

void TreeOperations::invalidateKeysCache(const QString& keyPrefix) {
  if (keyPrefix.isEmpty()) {
    m_keysCache.clear();
    m_keysCacheOrder.clear();
    return;
  }

  for (auto it = m_keysCache.begin(); it != m_keysCache.end();) {
    if (it.key().startsWith(keyPrefix)) {
      m_keysCacheOrder.removeAll(it.key());
      it = m_keysCache.erase(it);
    } else {
      ++it;
    }
  }
}

qlonglong TreeOperations::scanAggregateLimit() {
  QSettings settings;
  return settings
      .value("app/scanAggregateLimit",
             ServerConfig::DEFAULT_SCAN_AGGREGATE_LIMIT)
      .toLongLong();
}

qlonglong TreeOperations::keysLoadLimit() { return m_config.keysLoadLimit(); }

QSharedPointer<RedisClient::Connection> TreeOperations::keysLoader(
    QSharedPointer<RedisClient::Connection> main) {
  if (m_keysLoader && m_keysLoader->isConnected()) return m_keysLoader;

  try {
    auto loader = main->clone();

    if (!loader->isConnected()) connect(loader);

    if (loader->isConnected()) {
      m_keysLoader = loader;
      return m_keysLoader;
    }

    AppLog::put("keysLoader: clone is not connected, fallback to main connection");
  } catch (const RedisClient::Connection::Exception& e) {
    AppLog::put(QString("keysLoader: clone failed (%1), fallback to main connection")
                    .arg(QString::fromUtf8(e.what())));
  }

  return main;
}

void TreeOperations::resetKeysLoader() {
  if (!m_keysLoader) return;

  auto loader = m_keysLoader;
  m_keysLoader.clear();

  QtConcurrent::run([loader]() { loader->disconnect(); });
}

void TreeOperations::disconnect() {
  resetKeysLoader();
  m_connection->disconnect();
}

void TreeOperations::resetConnection() {
  resetKeysLoader();

  auto oldConnection = m_connection;
  setConnection(oldConnection->clone());

  QtConcurrent::run([oldConnection]() { oldConnection->disconnect(); });
}

QString TreeOperations::getNamespaceSeparator() {
  return m_config.namespaceSeparator();
}

QString TreeOperations::defaultFilter() { return m_config.keysPattern(); }

QVariantMap TreeOperations::getFilterHistory() {
    m_filterHistory = m_config.filterHistory();
    return m_filterHistory;
}

QString TreeOperations::connectionName() const {
    return m_config.name();
}

void TreeOperations::openKeyTab(QSharedPointer<ConnectionsTree::KeyItem> key,
                                bool openInNewTab) {
  getReadyConnection(
      [this, key, openInNewTab](QSharedPointer<RedisClient::Connection> c) {
        emit m_events->openValueTab(c, key, openInNewTab);
      });
}

void TreeOperations::openConsoleTab(int dbIndex) {
  getReadyConnection(
      [this, dbIndex](QSharedPointer<RedisClient::Connection> c) {
        emit m_events->openConsole(c, dbIndex, true);
      });
}

void TreeOperations::openNewKeyDialog(int dbIndex,
                                      QSharedPointer<OpenNewKeyDialogCallback> callback,
                                      QString keyPrefix) {
  getReadyConnection([this, dbIndex, callback,
                      keyPrefix](QSharedPointer<RedisClient::Connection> c) {
    emit m_events->newKeyDialog(c, callback, dbIndex, keyPrefix);
  });
}

void TreeOperations::openServerStats() {
  getReadyConnection([this](QSharedPointer<RedisClient::Connection> c) {
    emit m_events->openServerStats(c);
  });
}

void TreeOperations::duplicateConnection() {
  emit createNewConnection(m_config);
}

void TreeOperations::notifyDbWasUnloaded(int dbIndex) {
  emit m_events->closeDbKeys(m_connection, dbIndex);
}

void TreeOperations::deleteDbKey(ConnectionsTree::KeyItem& key,
                                 QSharedPointer<DeleteDbKeyCallback> callback) {
  getReadyConnection(
      [this, &key, callback](QSharedPointer<RedisClient::Connection> c) {
        c->cmd(
            {"DEL", key.getFullPath()}, this, key.getDbIndex(),
            [this, &key](RedisClient::Response) {
              key.setRemoved();
              QRegExp filter(key.getFullPath(), Qt::CaseSensitive,
                             QRegExp::Wildcard);
              if (m_events)
                m_events->closeDbKeys(m_connection, key.getDbIndex(), filter);
            },
            [this, callback](const QString& err) {
              QString errorMsg =
                  QCoreApplication::translate("RESP", "Delete key error: %1")
                      .arg(err);
              callback->call(errorMsg);
              if (m_events) m_events->error(errorMsg);
            });
      });
}

void TreeOperations::deleteDbKeys(ConnectionsTree::DatabaseItem& db) {
  auto self = sharedFromThis().toWeakRef();
  requestBulkOperation(
      db, BulkOperations::Manager::Operation::DELETE_KEYS,
      [self, this, &db](QRegExp filter, int, const QStringList&) {
        if (!self) {
          return;
        }
        uint dbIndex = db.getDbIndex();
        db.reload();

        if (m_events && m_connection) {
          getReadyConnection(
              [this, dbIndex, filter](QSharedPointer<RedisClient::Connection> c) {
                emit m_events->closeDbKeys(c, dbIndex, filter);
              });
        }
      });
}

void TreeOperations::deleteDbNamespace(ConnectionsTree::NamespaceItem& ns) {
  auto self = sharedFromThis().toWeakRef();
  requestBulkOperation(
      ns, BulkOperations::Manager::Operation::DELETE_KEYS,
      [this, self, &ns](QRegExp filter, int, const QStringList&) {
        if (!self) {
          return;
        }
        uint dbIndex = ns.getDbIndex();
        ns.setRemoved();

        if (m_events && m_connection) {
          getReadyConnection(
              [this, dbIndex, filter](QSharedPointer<RedisClient::Connection> c) {
                emit m_events->closeDbKeys(c, dbIndex, filter);
              });
        }
      });
}

void TreeOperations::setTTL(ConnectionsTree::AbstractNamespaceItem& ns) {
  requestBulkOperation(ns, BulkOperations::Manager::Operation::TTL,
                       [](QRegExp, int, const QStringList&) {});
}

void TreeOperations::copyKeys(ConnectionsTree::AbstractNamespaceItem& ns) {
  requestBulkOperation(ns, BulkOperations::Manager::Operation::COPY_KEYS,
                       [](QRegExp, int, const QStringList&) {});
}

void TreeOperations::importKeysFromRdb(ConnectionsTree::DatabaseItem& db) {
  getReadyConnection([this, &db](QSharedPointer<RedisClient::Connection> c) {
    emit m_events->requestBulkOperation(
        c->clone(), db.getDbIndex(),
        BulkOperations::Manager::Operation::IMPORT_RDB_KEYS, QRegExp(".*"),
        [&db](QRegExp, int, const QStringList&) { db.reload(); });
  });
}

void TreeOperations::flushDb(int dbIndex,
                             QSharedPointer<FlushDbCallback> callback) {

  auto callbackWrapper = [callback](const QString &err) {
    callback->call(err);
  };

  getReadyConnection(
      [dbIndex, callbackWrapper](QSharedPointer<RedisClient::Connection> c) {
        try {
          c->flushDbKeys(dbIndex, callbackWrapper);
        } catch (const RedisClient::Connection::Exception& e) {
          throw ConnectionsTree::Operations::Exception(
              QCoreApplication::translate("RESP", "Cannot flush database: ") +
              QString(e.what()));
        }
      });
}

QFuture<bool> TreeOperations::connectionSupportsMemoryOperations() {
    return m_connection->isCommandSupported({"MEMORY", "HELP"});
}

void TreeOperations::openKeyIfExists(const QByteArray& fullPath,
    QSharedPointer<ConnectionsTree::DatabaseItem> parent,
    QSharedPointer<OpenKeyIfExistsCallback> callback) {
  if (!parent) {
    qWarning() << "TreeOperations::openKeyIfExists > Invalid parent";
    return;
  }

  getReadyConnection([this, fullPath, parent,
                      callback](QSharedPointer<RedisClient::Connection> c) {
    c->cmd(
        {"exists", fullPath}, this, static_cast<int>(parent->getDbIndex()),
        [this, parent, fullPath, callback](RedisClient::Response r) {
          QVariant result = r.value();

          if (result.toByteArray() == "1") {
            auto key = QSharedPointer<ConnectionsTree::KeyItem>(
                new ConnectionsTree::KeyItem(fullPath, parent.toWeakRef(),
                                             parent->model(),
                                             parent->keysShortNameRendering()));

            emit m_events->openValueTab(m_connection, key, true);

            callback->call(QString(), true);
          } else {
            callback->call(QString(), false);
          }
        },
        [callback](const QString& err) { callback->call(err, false); });
  });
}

void TreeOperations::getUsedMemory(const QList<QByteArray>& keys, int dbIndex,
                                   QSharedPointer<GetUsedMemoryCallback> result,
                                   QSharedPointer<GetUsedMemoryCallback> progress) {
  QList<QList<QByteArray>> commands;

  for (int index = 0; index < keys.size(); ++index) {
    commands.append({"MEMORY", "USAGE", keys[index]});
  }

  int expectedResponses = commands.size();
  auto processedResponses = QSharedPointer<int>(new int(0));
  auto totalMemory = QSharedPointer<qlonglong>(new qlonglong(0));

  m_connection->pipelinedCmd(
      commands, this, dbIndex,
      [this, expectedResponses, processedResponses, totalMemory, progress,
       result](RedisClient::Response r, QString err) {
        if (!err.isEmpty()) {
          QString errorMsg =
              QCoreApplication::translate(
                  "RESP", "Cannot determine amount of used memory by key: %1")
                  .arg(err);
          m_events->error(errorMsg);          
        } else {
          QVariant incrResult = r.value();

          if (incrResult.canConvert(QVariant::LongLong)) {
            (*totalMemory) += incrResult.toLongLong();
            (*processedResponses)++;
          } else if (incrResult.canConvert(QVariant::List)) {
            auto responses = incrResult.toList();

            for (auto resp : responses) {
              (*totalMemory) += resp.toLongLong();
              (*processedResponses)++;
            }
          }

          if (progress)
            progress->call(*totalMemory);

          if ((*processedResponses) >= expectedResponses && result) {
            result->call(*totalMemory);
          }
        }
      }, true);
}

QString TreeOperations::mode() {
  if (m_connectionMode == RedisClient::Connection::Mode::Cluster) {
    return QString("cluster");
  } else if (m_connectionMode == RedisClient::Connection::Mode::Sentinel) {
    return QString("sentinel");
  } else {
    return QString("standalone");
  }
}

bool TreeOperations::isConnected() const { return m_connection->isConnected(); }

QSharedPointer<RedisClient::Connection> TreeOperations::connection()
{
    return m_connection;
}

void TreeOperations::setConnection(QSharedPointer<RedisClient::Connection> c) {
  m_connection = c;
  m_events->registerLoggerForConnection(*c);
}

ServerConfig TreeOperations::config()
{
    m_config.setOwner(sharedFromThis().toWeakRef());
    return m_config;
}

void TreeOperations::setConfig(const ServerConfig &c)
{
    m_config = c;
    m_config.setOwner(sharedFromThis().toWeakRef());
    m_connection->setConnectionConfig(m_config);
    emit configUpdated();
}

void TreeOperations::proceedWithSecret(const ServerConfig &c)
{
    m_config = c;
    m_config.setOwner(sharedFromThis().toWeakRef());
    m_connection->setConnectionConfig(m_config);

    if (m_pendingOperation) {
        m_pendingOperation(m_connection);
    } else {
        qWarning() << "Unknown proceedWithSecret request";
        return;
    }
}

QString TreeOperations::iconColor()
{
    return m_config.iconColor();
}
