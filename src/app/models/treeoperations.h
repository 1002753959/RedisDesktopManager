#pragma once
#include <QEnableSharedFromThis>
#include <QObject>
#include <QSharedPointer>
#include <functional>

#include "app/models/connectionconf.h"
#include "connections-tree/items/keyitem.h"
#include "modules/bulk-operations/bulkoperationsmanager.h"
#include "modules/connections-tree/operations.h"

class Events;

namespace ConnectionsTree {
class ServerItem;
class TreeItem;
}  // namespace ConnectionsTree

class TreeOperations : public QObject,
                       public ConnectionsTree::Operations,
                       public QEnableSharedFromThis<TreeOperations> {
  Q_OBJECT
 public:
  TreeOperations(const ServerConfig& config, QSharedPointer<Events> events);

  QFuture<void> getDatabases(
      QSharedPointer<GetDatabasesCallback> callback) override;

  void loadNamespaceItems(
      uint dbIndex, const QString& filter,
      QSharedPointer<LoadNamespaceItemsCallback> callback,
      const KeysLoadOptions& options = KeysLoadOptions()) override;

  void disconnect() override;

  void resetConnection() override;

  QString getNamespaceSeparator() override;

  QString defaultFilter() override;

  qlonglong keysLoadLimit() override;

  qlonglong scanAggregateLimit() override;

  QVariantMap getFilterHistory() override;

  QString connectionName() const override;

  void openKeyTab(QSharedPointer<ConnectionsTree::KeyItem> key,
                  bool openInNewTab = false) override;

  void openConsoleTab(int dbIndex = 0) override;

  void openNewKeyDialog(int dbIndex,
                        QSharedPointer<OpenNewKeyDialogCallback> callback,
                        QString keyPrefix = QString()) override;

  void openServerStats() override;

  void duplicateConnection() override;

  void notifyDbWasUnloaded(int dbIndex) override;

  void deleteDbKey(ConnectionsTree::KeyItem& key,
                   QSharedPointer<DeleteDbKeyCallback> callback) override;

  virtual void deleteDbKeys(ConnectionsTree::DatabaseItem& db) override;

  void deleteDbNamespace(ConnectionsTree::NamespaceItem& ns) override;

  virtual void setTTL(ConnectionsTree::AbstractNamespaceItem& ns) override;

  virtual void copyKeys(ConnectionsTree::AbstractNamespaceItem& ns) override;

  virtual void importKeysFromRdb(ConnectionsTree::DatabaseItem& ns) override;

  virtual void flushDb(int dbIndex,
                       QSharedPointer<FlushDbCallback> callback) override;

  virtual QFuture<bool> connectionSupportsMemoryOperations() override;

  virtual void openKeyIfExists(
      const QByteArray& key,
      QSharedPointer<ConnectionsTree::DatabaseItem> parent,
      QSharedPointer<OpenKeyIfExistsCallback> callback) override;

  virtual void getUsedMemory(const QList<QByteArray>& keys, int dbIndex,
                             QSharedPointer<GetUsedMemoryCallback> result,
                             QSharedPointer<GetUsedMemoryCallback> progress) override;

  virtual QString mode() override;

  virtual bool isConnected() const override;

  QSharedPointer<RedisClient::Connection> connection();

  void setConnection(QSharedPointer<RedisClient::Connection> c);

  ServerConfig config();

  void setConfig(const ServerConfig& c);

  void proceedWithSecret(const ServerConfig& c);

  QString iconColor() override;

signals:
  void createNewConnection(const ServerConfig& config);

  void configUpdated();

  void filterHistoryUpdated();

  void secretRequired(const ServerConfig& config, const QString& id);

 protected:
  void loadDatabases(
      QSharedPointer<RedisClient::Connection> c,
      QSharedPointer<AsyncFuture::Deferred<void>> d,
      std::function<void(RedisClient::DatabaseList, const QString&)> callback);

  void recursiveSelectScan(
      QSharedPointer<AsyncFuture::Deferred<void>> d,
      QSharedPointer<RedisClient::Connection> c,
      QSharedPointer<RedisClient::DatabaseList> dbList,
      std::function<void(RedisClient::DatabaseList, const QString&)> callback);

  bool connect(QSharedPointer<RedisClient::Connection> c);

  void requestBulkOperation(
      ConnectionsTree::AbstractNamespaceItem& ns,
      BulkOperations::Manager::Operation op,
      BulkOperations::AbstractOperation::OperationCallback callback);

 private:
  typedef std::function<void(QSharedPointer<RedisClient::Connection>)>
      PendingOperation;
  void getReadyConnection(PendingOperation callback);

  /*
   * Returns a dedicated connection which is used only for keys loading
   * (scan operations). Prevents the keys scan from blocking the main
   * connection (and vice versa) on huge databases.
   */
  QSharedPointer<RedisClient::Connection> keysLoader(
      QSharedPointer<RedisClient::Connection> main);

  void resetKeysLoader();

 private:
  /*
   * In-memory keys cache: pressing the expand arrow on a namespace twice
   * (or returning to it after switching tabs) doesn't trigger a new SCAN.
   * Entries are invalidated on Reload and by TTL.
   */
  struct CachedKeysPage {
    KeysPage page;
    qint64 timestamp = 0;
  };

  QString keysCacheKey(uint dbIndex, const QString &filter,
                       const QByteArray &cursor) const;

  bool lookupKeysCache(const QString &key, KeysPage *page);

  void putKeysCache(const QString &key, const KeysPage &page);

  void invalidateKeysCache(const QString &keyPrefix);

 private:
  QSharedPointer<RedisClient::Connection> m_connection;
  QSharedPointer<Events> m_events;
  QSharedPointer<RedisClient::Connection> m_keysLoader;
  uint m_dbCount;
  RedisClient::Connection::Mode m_connectionMode;
  ServerConfig m_config;
  QVariantMap m_filterHistory;
  QWeakPointer<ConnectionsTree::ServerItem> m_serverItem;
  QSharedPointer<AsyncFuture::Deferred<void>> m_dbScanOp;
  PendingOperation m_pendingOperation;
  QHash<QString, CachedKeysPage> m_keysCache;
  QList<QString> m_keysCacheOrder;
};
