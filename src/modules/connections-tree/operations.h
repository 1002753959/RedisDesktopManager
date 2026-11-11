#pragma once
#include <qredisclient/connection.h>

#include <QFuture>
#include <QMap>
#include <QSharedPointer>
#include <QString>
#include <QStringList>
#include <functional>

#include "exception.h"
#include "modules/common/callbackwithowner.h"

namespace Console {
class Operations;
}

namespace ConnectionsTree {

class KeyItem;
class NamespaceItem;
class AbstractNamespaceItem;
class DatabaseItem;
class TreeItem;

class Operations {
  ADD_EXCEPTION
 public:
  /**
   * List of databases with keys counters
   * @emit databesesLoaded
   **/
  using DbMapping = QMap<int, int>;
  using GetDatabasesCallback =
      CallbackWithOwner<TreeItem, DbMapping, const QString&>;

  virtual QFuture<void> getDatabases(QSharedPointer<GetDatabasesCallback>) = 0;

  /**
   * @brief loadNamespaceItems
   *
   * One page of keys produced by a single keys loading request.
   *
   * nextCursor == "0" means the whole filtered keyspace was traversed,
   * otherwise the cursor can be passed back through KeysLoadOptions in
   * order to continue the traversal (real pagination of "Load more keys").
   */
  struct KeysPage {
    RedisClient::Connection::RawKeysList keys;
    QByteArray nextCursor = QByteArray("0");
    long scannedKeys = 0;
  };

  struct KeysLoadOptions {
    /*
     * Resume the scan session from this cursor ("0" - new session).
     */
    QByteArray startCursor = QByteArray("0");

    /*
     * Aggregate mode: the scan is executed until the end of the keyspace
     * (up to the aggregate limit) instead of being truncated by keysLoadLimit.
     * It gives exact keys counters for the child namespaces.
     */
    bool aggregate = true;

    /*
     * Allow using the in-memory keys cache (reload must bypass it).
     */
    bool useCache = true;

    /*
     * "Load all" mode: the whole filtered keyspace is returned in one page.
     * keysLoadLimit / scanMaxIterations / scanAggregateLimit are ignored.
     * Triggered explicitly by the user (dedicated inline menu button), so the
     * cost of a full traversal is paid intentionally. The received keys are
     * rendered afterwards in background chunks (no "Load more" clicking).
     */
    bool loadAll = false;

    /*
     * Optional progress notification (amount of scanned keys).
     */
    std::function<void(long)> progress;
  };

  using LoadNamespaceItemsCallback =
      CallbackWithOwner<TreeItem, const KeysPage&, const QString&>;

  virtual void loadNamespaceItems(uint dbIndex, const QString& filter,
                                  QSharedPointer<LoadNamespaceItemsCallback>,
                                  const KeysLoadOptions& options =
                                      KeysLoadOptions()) = 0;

  /**
   * Cancel all operations & close connection
   * @brief disconnect
   */
  virtual void disconnect() = 0;

  /**
    Cancel all operations & reconnect
   * @brief resetConnection
   */
  virtual void resetConnection() = 0;

  /**
   * @brief getNamespaceSeparator
   * @return
   */
  virtual QString getNamespaceSeparator() = 0;

  virtual QString iconColor() = 0;

  virtual QString defaultFilter() = 0;

  /*
   * Maximum amount of keys loaded per one database/namespace
   * (0 - unlimited). Used to notify about truncated key list.
   */
  virtual qlonglong keysLoadLimit() = 0;

  /*
   * Maximum amount of keys scanned by one aggregated request
   * (0 - unlimited). Bounds the memory/time cost of the exact
   * child namespaces counting.
   */
  virtual qlonglong scanAggregateLimit() = 0;

  virtual QVariantMap getFilterHistory() = 0;

  virtual QString connectionName() const = 0;

  virtual void openKeyTab(QSharedPointer<KeyItem> key, bool openInNewTab) = 0;

  virtual void openConsoleTab(int dbIndex = 0) = 0;

  using OpenNewKeyDialogCallback = CallbackWithOwner<TreeItem>;

  virtual void openNewKeyDialog(int dbIndex, QSharedPointer<OpenNewKeyDialogCallback> callback,
                                QString keyPrefix = QString()) = 0;

  virtual void openServerStats() = 0;

  virtual void duplicateConnection() = 0;

  virtual void notifyDbWasUnloaded(int dbIndex) = 0;

  using DeleteDbKeyCallback = CallbackWithOwner<TreeItem, const QString&>;

  virtual void deleteDbKey(ConnectionsTree::KeyItem& key,
                           QSharedPointer<DeleteDbKeyCallback> callback) = 0;

  virtual void deleteDbKeys(ConnectionsTree::DatabaseItem& db) = 0;

  virtual void deleteDbNamespace(ConnectionsTree::NamespaceItem& ns) = 0;

  virtual void setTTL(ConnectionsTree::AbstractNamespaceItem& ns) = 0;

  virtual void copyKeys(ConnectionsTree::AbstractNamespaceItem& ns) = 0;

  virtual void importKeysFromRdb(ConnectionsTree::DatabaseItem& ns) = 0;

  using FlushDbCallback = CallbackWithOwner<TreeItem, const QString&>;

  virtual void flushDb(int dbIndex, QSharedPointer<FlushDbCallback> callback) = 0;

  using OpenKeyIfExistsCallback = CallbackWithOwner<TreeItem, const QString&, bool>;

  virtual void openKeyIfExists(
      const QByteArray& key,
      QSharedPointer<ConnectionsTree::DatabaseItem> parent,
      QSharedPointer<OpenKeyIfExistsCallback> callback) = 0;

  virtual QString mode() = 0;

  virtual bool isConnected() const = 0;

  virtual QFuture<bool> connectionSupportsMemoryOperations() = 0;

  using GetUsedMemoryCallback = CallbackWithOwner<TreeItem, qlonglong>;

  virtual void getUsedMemory(
      const QList<QByteArray>& keys, int dbIndex,
      QSharedPointer<GetUsedMemoryCallback> result,
      QSharedPointer<GetUsedMemoryCallback> progress) = 0;

  virtual ~Operations() {}
};
}  // namespace ConnectionsTree
