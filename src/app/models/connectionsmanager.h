#pragma once
#include <qredisclient/connection.h>

#include <QSharedPointer>
#include <functional>

#include "app/models/connectionconf.h"
#include "bulk-operations/connections.h"
#include "connections-tree/model.h"
#include "treeoperations.h"
#include "connectiongroup.h"

namespace ValueEditor {
class TabsModel;
}

namespace ConnectionsTree {
class ServerGroup;
}

class Events;

class ConnectionsManager : public ConnectionsTree::Model,
                           public BulkOperations::ConnectionsModel {
  Q_OBJECT

  Q_PROPERTY(int connectionsCount READ size NOTIFY sizeChanged)

 public:
  ConnectionsManager(const QString& m_configPath,
                     QSharedPointer<Events> events);  

  void loadConnections();

  Q_INVOKABLE void addNewConnection(const ServerConfig& config,
                                    bool saveToConfig = true,
                                    QSharedPointer<ConnectionsTree::ServerGroup> group =
                                        QSharedPointer<ConnectionsTree::ServerGroup>());

  Q_INVOKABLE void addNewGroup(const QString& name);

  Q_INVOKABLE void updateGroup(const ConnectionGroup& group);

  Q_INVOKABLE void updateConnection(const ServerConfig& config);

  Q_INVOKABLE bool importConnections(const QString&);

  Q_INVOKABLE bool saveConnectionsConfigToFile(const QString&);

  Q_INVOKABLE void testConnectionSettings(const ServerConfig& config, QJSValue jsCallback);

  Q_INVOKABLE void proceedWithConnectionSecret(const ServerConfig& config);

  Q_INVOKABLE ServerConfig createEmptyConfig() const;

  Q_INVOKABLE ServerConfig parseConfigFromRedisConnectionString(const QString&) const;

  Q_INVOKABLE bool isRedisConnectionStringValid(const QString&);

  void saveConfig();

  Q_INVOKABLE int size() override;

  // BulkOperations model methods
  QSharedPointer<RedisClient::Connection> getByIndex(int index) override;

  QStringList getConnections() override;

  void applyGroupChanges() override;

  /*
   * Bookmark support
   */

  // Returns {"connection", "db", "pattern", "name", "full_path"} for the
  // namespace/database/key item under the given index.
  Q_INVOKABLE QVariantMap bookmarkContext(const QModelIndex& index);

  // Applies the pattern as a database filter and expands matched namespaces.
  // Returns false when the connection or database is not loaded yet.
  Q_INVOKABLE bool openKeysPattern(const QString& connection, int dbIndex,
                                   const QString& pattern);

 signals:
  void editConnection(ServerConfig config);

  void editConnectionGroup(ConnectionGroup group);

  void connectionAboutToBeEdited(QString name);

  void sizeChanged();

  void connectionsLoaded();

  void askUserForConnectionSecret(const ServerConfig& config, const QString& id);

 protected:
  bool loadConnectionsConfigFromFile(const QString& config,
                                     bool saveChangesToFile = false);

  void tryToConnect(const ServerConfig& config, QJSValue jsCallback);

 private:
  void createServerItemForConnection(const ServerConfig &config,
      QSharedPointer<ConnectionsTree::ServerGroup> group=QSharedPointer<ConnectionsTree::ServerGroup>());

  void addGroup(QSharedPointer<ConnectionsTree::ServerGroup> serverGroup);

  void buildConnectionsCache();

  QSharedPointer<ConnectionsTree::ServerItem> findServerItem(
      const QString& connectionName);

  void expandNamespaceWhenLoaded(
      QWeakPointer<ConnectionsTree::TreeItem> databaseItem,
      const QString& namespacePath, const QString& separator);

 private:
  QString m_configPath;
  QSharedPointer<Events> m_events;
  QJSValue m_jsCallback;
  QMap<QString, QSharedPointer<ConnectionsTree::ServerItem>> m_connectionsCache;
};
