#include "connectionsmanager.h"

#include <QAbstractItemModel>
#include <QDebug>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSet>
#include <QTimer>
#include <QUrlQuery>

#include "app/events.h"
#include "configmanager.h"
#include "modules/bulk-operations/bulkoperationsmanager.h"
#include "modules/connections-tree/items/abstractnamespaceitem.h"
#include "modules/connections-tree/items/databaseitem.h"
#include "modules/connections-tree/items/namespaceitem.h"
#include "modules/connections-tree/items/serveritem.h"
#include "modules/connections-tree/items/servergroup.h"
#include "modules/value-editor/tabsmodel.h"

using ConnectionsTree::AbstractNamespaceItem;
using ConnectionsTree::DatabaseItem;
using ConnectionsTree::ServerItem;
using ConnectionsTree::TreeItem;

namespace {

QSharedPointer<AbstractNamespaceItem> findChildNamespaceSafe(
    QSharedPointer<AbstractNamespaceItem> parent, const QString& name) {
  if (!parent || name.isEmpty()) return QSharedPointer<AbstractNamespaceItem>();

  return parent->findChildNamespace(name.toUtf8());
}

}  // namespace

ConnectionsManager::ConnectionsManager(const QString& configPath,
                                       QSharedPointer<Events> events)
    : ConnectionsTree::Model(), m_configPath(configPath), m_events(events) {
  connect(this, &ConnectionsTree::Model::error, m_events.data(),
          &Events::error);
}

void ConnectionsManager::loadConnections() {
  if (!m_configPath.isEmpty() && QFile::exists(m_configPath)) {
    loadConnectionsConfigFromFile(m_configPath);
  }

  emit connectionsLoaded();
}

void ConnectionsManager::addNewConnection(
    const ServerConfig& config, bool saveToConfig,
    QSharedPointer<ConnectionsTree::ServerGroup> group) {
  createServerItemForConnection(config, group);

  if (saveToConfig) saveConfig();

  buildConnectionsCache();
}

void ConnectionsManager::addNewGroup(const QString& name) {
  auto group = QSharedPointer<ConnectionsTree::ServerGroup>(
      new ConnectionsTree::ServerGroup(
          name, *static_cast<ConnectionsTree::Model*>(this)));

  addGroup(group);

  saveConfig();
}

void ConnectionsManager::updateGroup(const ConnectionGroup &group)
{
    auto serverGroup = group.serverGroup();

    if (!serverGroup){
        qWarning() << "invalid server group";
        return;
    }

    emit itemChanged(serverGroup);

    saveConfig();

    buildConnectionsCache();
}

void ConnectionsManager::updateConnection(const ServerConfig& config) {
  if (!config.owner()) return addNewConnection(config);

  auto treeOperations = config.owner().toStrongRef();

  if (!treeOperations) return;

  treeOperations->setConfig(config);

  saveConfig();
}

bool ConnectionsManager::importConnections(const QString& path) {
  if (loadConnectionsConfigFromFile(path, true)) {
    emit sizeChanged();
    return true;
  }
  return false;
}

bool ConnectionsManager::loadConnectionsConfigFromFile(const QString& config,
                                                       bool saveChangesToFile) {
  QJsonArray connections;

  QFile conf(config);

  if (!conf.open(QIODevice::ReadOnly)) return false;

  QByteArray data = conf.readAll();
  conf.close();

  QJsonDocument jsonConfig = QJsonDocument::fromJson(data);

  if (jsonConfig.isEmpty()) return true;

  if (!jsonConfig.isArray()) {
    return false;
  }

  connections = jsonConfig.array();

  for (QJsonValue connection : connections) {
    if (!connection.isObject()) continue;

    auto obj = connection.toObject();

    bool isValidGroup = obj.contains("type") && obj.contains("connections") &&
            obj.contains("name") && obj["connections"].isArray() &&
            obj["type"].toString().toLower() == "group";

    if (isValidGroup) {
      auto groupConnections = obj["connections"].toArray();

      auto group = QSharedPointer<ConnectionsTree::ServerGroup>(
          new ConnectionsTree::ServerGroup(
              obj["name"].toString(),
              *static_cast<ConnectionsTree::Model*>(this)));

      for (const QJsonValue &c : qAsConst(groupConnections)) {
        if (!c.isObject()) continue;

        ServerConfig conf(c.toObject().toVariantHash());

        if (conf.isNull()) continue;

        conf.setId(QUuid::createUuid().toByteArray());
        addNewConnection(conf, false, group);
      }

      addGroup(group);      
    } else {
        ServerConfig conf(obj.toVariantHash());
        if (conf.isNull()) continue;
        addNewConnection(conf, false);
    }
  }

  if (saveChangesToFile) saveConfig();

  buildConnectionsCache();

  return true;
}

void ConnectionsManager::tryToConnect(const ServerConfig &config, QJSValue jsCallback)
{
    RedisClient::Connection testConnection(config);
    m_events->registerLoggerForConnection(testConnection);

    try {
      jsCallback.call(QJSValueList{testConnection.connect()});
    } catch (const RedisClient::Connection::Exception&) {
      jsCallback.call(QJSValueList{false});
    }
}

void ConnectionsManager::saveConfig() {
  saveConnectionsConfigToFile(m_configPath);
}

bool ConnectionsManager::saveConnectionsConfigToFile(
    const QString& pathToFile) {
  QJsonArray connections;

  auto addConfig = [](QSharedPointer<ConnectionsTree::TreeItem> i,
                      QJsonArray& connections) {
    auto srvItem = i.dynamicCast<ConnectionsTree::ServerItem>();

    if (!srvItem) return;

    auto op = srvItem->getOperations().dynamicCast<TreeOperations>();

    if (!op) return;

    auto config = op->config();
    QSet<QString> ignoreFields {"id"};

    if (config.askForSshPassword()) {
        ignoreFields.insert(ServerConfig::SSH_SECRET_ID);
    }

    connections.push_back(QJsonValue(config.toJsonObject(ignoreFields)));
  };

  for (auto item : m_treeItems) {
    if (item->type() == "server_group") {
      QJsonObject group;
      group["type"] = "group";
      group["name"] = item->getDisplayName();

      QJsonArray groupConnections;

      for (auto srv : item->getAllChilds()) {
        addConfig(srv, groupConnections);
      }

      group["connections"] = groupConnections;
      connections.push_back(QJsonValue(group));

    } else if (item->type() == "server") {
      addConfig(item, connections);
    }
  }

  return saveJsonArrayToFile(connections, pathToFile);
}

void ConnectionsManager::testConnectionSettings(const ServerConfig& config,
                                                QJSValue jsCallback) {
  if (!jsCallback.isCallable()) {
    qDebug() << "JS callback is not callable";
    return;
  }

  if (config.askForSshPassword()) {
    m_jsCallback = jsCallback;

    emit askUserForConnectionSecret(config, ServerConfig::SSH_SECRET_ID);
  } else {
    tryToConnect(config, jsCallback);
  }
}

void ConnectionsManager::proceedWithConnectionSecret(const ServerConfig &config)
{
    if (m_jsCallback.isCallable()) {
        tryToConnect(config, m_jsCallback);
        m_jsCallback = QJSValue();
        return;
    }

    if (!config.owner()) {
        qWarning() << "Invalid config with secret";
        return;
    }

    auto treeOperations = config.owner().toStrongRef();

    if (!treeOperations) {
        qWarning() << "Config with secret doesn't have owner";
        return;
    }

    treeOperations->proceedWithSecret(config);
}

ServerConfig ConnectionsManager::createEmptyConfig() const {
  return ServerConfig();
}

ServerConfig ConnectionsManager::parseConfigFromRedisConnectionString(const QString& connectionString) const {
    QUrl url = QUrl(connectionString);

    QUrlQuery query = QUrlQuery(url.query());

    ServerConfig config;

    config.setHost(url.host().isEmpty() || url.host() == "localhost" ? "127.0.0.1" : url.host());

    config.setPort(url.port() == -1 ? 6379 : url.port());

    config.setUsername(url.userName());

    config.setAuth(url.password().isEmpty() ? query.queryItemValue("password") : url.password());

    if (url.scheme() == "rediss" || (!query.isEmpty() && query.queryItemValue("ssl") == "true")) {
        config.setSsl(true);
    }

    return config;
}

bool ConnectionsManager::isRedisConnectionStringValid(
    const QString& connectionString) {
  QUrl url;
  if (connectionString.startsWith("redis://") ||
      connectionString.startsWith("rediss://")) {
    url = QUrl(connectionString);
  } else {
    url = QUrl(QString("redis://%1").arg(connectionString));
  }

  return url.isValid() &&
         (url.scheme() == "redis" || url.scheme() == "rediss") &&
         !url.host().isEmpty();
}

int ConnectionsManager::size() {
  int connectionsCount = 0;

  for (auto item : qAsConst(m_treeItems)) {
    if (item->type() == "server_group") {
      connectionsCount += item->childCount();
    } else if (item->type() == "server") {
      connectionsCount++;
    }
  }
  return connectionsCount;
}

QSharedPointer<RedisClient::Connection> ConnectionsManager::getByIndex(
    int index) {
  auto op = m_connectionsCache.values().at(index)->getOperations();

  if (!op) return QSharedPointer<RedisClient::Connection>();

  auto treeOp = op.dynamicCast<TreeOperations>();

  if (!treeOp) return QSharedPointer<RedisClient::Connection>();

  return treeOp->connection();
}

QStringList ConnectionsManager::getConnections() {
  return m_connectionsCache.keys();
}

void ConnectionsManager::applyGroupChanges() {
  ConnectionsTree::Model::applyGroupChanges();

  buildConnectionsCache();

  saveConfig();
}

void ConnectionsManager::createServerItemForConnection(
    const ServerConfig& config,
    QSharedPointer<ConnectionsTree::ServerGroup> group) {
  using namespace ConnectionsTree;

  auto treeModel =
      QSharedPointer<TreeOperations>(new TreeOperations(config, m_events));

  connect(treeModel.data(), &TreeOperations::createNewConnection, this,
          [this](const ServerConfig& config) { addNewConnection(config); });

  connect(treeModel.data(), &TreeOperations::secretRequired, this,
          &ConnectionsManager::askUserForConnectionSecret);

  QWeakPointer<TreeItem> parent;

  if (group) {
    parent = group.toWeakRef();
  }

  auto serverItem = QSharedPointer<ServerItem>(
      new ServerItem(treeModel.dynamicCast<ConnectionsTree::Operations>(),
                     *static_cast<ConnectionsTree::Model*>(this), parent));
  serverItem->setWeakPointer(serverItem.toWeakRef());

  connect(
      treeModel.data(), &TreeOperations::configUpdated, this,
      [this, serverItem]() {
        if (!serverItem) return;

        emit itemChanged(
            serverItem.dynamicCast<ConnectionsTree::TreeItem>().toWeakRef());
      });

  connect(treeModel.data(), &TreeOperations::filterHistoryUpdated,
          this, [this]() {
      saveConfig();
  });

  connect(serverItem.data(), &ConnectionsTree::ServerItem::editActionRequested,
          this, [this, treeModel]() {
            if (!treeModel) return;

            auto config = treeModel->config();

            emit connectionAboutToBeEdited(config.name());

            // NOTE(u_glide): Do not show temproary stored password in the UI
            if (config.askForSshPassword()) {
                config.setSshPassword(QString());
            }

            emit editConnection(config);
          });

  connect(serverItem.data(),
          &ConnectionsTree::ServerItem::deleteActionRequested, this,
          [this, serverItem, treeModel, group]() {
            if (!serverItem || !treeModel) return;

            emit connectionAboutToBeEdited(treeModel->config().name());

            if (group) {
              group->removeConnection(serverItem);
            } else {
              removeRootItem(serverItem);
            }

            buildConnectionsCache();

            emit sizeChanged();
            saveConfig();
          });

  if (group) {
    group->addServer(serverItem);
  } else {
    addRootItem(serverItem);
  }
}

void ConnectionsManager::addGroup(
    QSharedPointer<ConnectionsTree::ServerGroup> group) {
  connect(group.data(), &ConnectionsTree::ServerGroup::editActionRequested,
          this, [this, group]() {
            if (!group) return;

            ConnectionGroup g(group);

            emit editConnectionGroup(g);
          });

  connect(group.data(), &ConnectionsTree::ServerGroup::deleteActionRequested,
          this, [this, group]() {
            if (!group) return;

            removeRootItem(group);

            buildConnectionsCache();

            emit sizeChanged();
            saveConfig();
          });

  addRootItem(group);

  buildConnectionsCache();
}

void ConnectionsManager::buildConnectionsCache() {
  m_connectionsCache.clear();

  for (auto item : m_treeItems) {
    if (item->type() == "server_group") {
      QString nameTemplate = QString("[%1] %2").arg(item->getDisplayName());

      for (auto srv : item->getAllChilds()) {
        QString name = nameTemplate.arg(srv->getDisplayName());
        m_connectionsCache[name] =
            srv.dynamicCast<ConnectionsTree::ServerItem>();
      }
    } else if (item->type() == "server") {
      m_connectionsCache[item->getDisplayName()] =
          item.dynamicCast<ConnectionsTree::ServerItem>();
    }
  }
}

/*
 * Bookmarks support
 */

QVariantMap ConnectionsManager::bookmarkContext(const QModelIndex& index) {
  QVariantMap result;

  TreeItem* rawItem = getItemFromIndex(index);

  if (!rawItem || !m_rawPointers) return result;

  QSharedPointer<TreeItem> item = m_rawPointers->value(rawItem).toStrongRef();

  if (!item) return result;

  QString connection;
  QString separator(":");
  int dbIndex = -1;

  QSharedPointer<TreeItem> current = item;

  while (current) {
    if (dbIndex < 0 && current->type() == "database") {
      auto namespaceItem = current.dynamicCast<AbstractNamespaceItem>();

      if (namespaceItem) dbIndex = static_cast<int>(namespaceItem->getDbIndex());
    }

    if (current->type() == "server") {
      auto server = current.dynamicCast<ServerItem>();

      if (server && server->getOperations()) {
        connection = server->getOperations()->connectionName();
        separator = server->getOperations()->getNamespaceSeparator();
      }

      break;
    }

    current = current->parent().toStrongRef();
  }

  if (connection.isEmpty() || dbIndex < 0) return result;

  const QString fullPath = QString::fromUtf8(item->getFullPath());

  QString pattern = fullPath;

  if (item->type() == "namespace") {
    pattern = fullPath + separator + "*";
  } else if (item->type() == "database") {
    pattern = "*";
  }

  QString name = fullPath;

  if (name.endsWith(separator)) name.chop(separator.size());

  const int pos = name.lastIndexOf(separator);

  if (pos >= 0) name = name.mid(pos + separator.size());

  result["connection"] = connection;
  result["db"] = dbIndex;
  result["pattern"] = pattern;
  result["name"] = name;
  result["full_path"] = fullPath;
  result["separator"] = separator;

  return result;
}

bool ConnectionsManager::openKeysPattern(const QString& connection, int dbIndex,
                                         const QString& pattern) {
  auto server = findServerItem(connection);

  if (!server) return false;

  QSharedPointer<DatabaseItem> database;

  for (auto child : server->getAllChilds()) {
    auto dbItem = child.dynamicCast<DatabaseItem>();

    if (dbItem && static_cast<int>(dbItem->getDbIndex()) == dbIndex) {
      database = dbItem;
      break;
    }
  }

  if (!database) return false;

  QModelIndex serverIndex =
      getIndexFromItem(server.staticCast<TreeItem>().toWeakRef());

  if (serverIndex.isValid()) {
    setExpanded(serverIndex);
    emit expand(serverIndex);
  }

  QModelIndex databaseIndex =
      getIndexFromItem(database.staticCast<TreeItem>().toWeakRef());

  if (!databaseIndex.isValid()) return false;

  setExpanded(databaseIndex);
  emit expand(databaseIndex);

  setMetadata(databaseIndex, "filter", pattern);

  if (pattern.endsWith('*')) {
    QString separator = server->getOperations()
                            ? server->getOperations()->getNamespaceSeparator()
                            : QString(":");

    QString nsPath = pattern.left(pattern.size() - 1);

    if (nsPath.endsWith(separator)) nsPath.chop(separator.size());

    if (!nsPath.isEmpty() && nsPath != "*") {
      expandNamespaceWhenLoaded(database.staticCast<TreeItem>().toWeakRef(),
                                nsPath, separator);
    }
  }

  return true;
}

QSharedPointer<ConnectionsTree::ServerItem> ConnectionsManager::findServerItem(
    const QString& connectionName) {
  auto matches = [&connectionName](QSharedPointer<TreeItem> item) {
    auto server = item.dynamicCast<ConnectionsTree::ServerItem>();

    if (!server) return false;

    if (server->getOperations() &&
        server->getOperations()->connectionName() == connectionName) {
      return true;
    }

    return server->getDisplayName() == connectionName;
  };

  for (auto item : m_treeItems) {
    if (item->type() == "server_group") {
      for (auto srv : item->getAllChilds()) {
        if (matches(srv)) return srv.dynamicCast<ConnectionsTree::ServerItem>();
      }
    } else if (matches(item)) {
      return item.dynamicCast<ConnectionsTree::ServerItem>();
    }
  }

  return QSharedPointer<ConnectionsTree::ServerItem>();
}

void ConnectionsManager::expandNamespaceWhenLoaded(
    QWeakPointer<TreeItem> databaseItem, const QString& namespacePath,
    const QString& separator) {
  QTimer* timer = new QTimer(this);
  timer->setInterval(250);

  auto attempts = QSharedPointer<int>::create(0);
  auto clicked = QSharedPointer<QSet<QByteArray>>::create();

  connect(timer, &QTimer::timeout, this,
          [this, timer, attempts, clicked, databaseItem, namespacePath,
           separator]() {
            ++(*attempts);

            if (*attempts > 120) {
              timer->stop();
              timer->deleteLater();
              return;
            }

            auto database =
                databaseItem.toStrongRef().dynamicCast<AbstractNamespaceItem>();

            if (!database) {
              timer->stop();
              timer->deleteLater();
              return;
            }

            const QStringList segments =
                namespacePath.split(separator, Qt::SkipEmptyParts);

            QSharedPointer<AbstractNamespaceItem> deepest = database;

            for (const QString& segment : segments) {
              auto next = findChildNamespaceSafe(deepest, segment);

              if (!next) break;

              deepest = next;

              if (!clicked->contains(deepest->getFullPath())) {
                QModelIndex index =
                    getIndexFromItem(deepest.staticCast<TreeItem>().toWeakRef());

                if (index.isValid()) {
                  clicked->insert(deepest->getFullPath());
                  sendEvent(index, "click");
                }
              }
            }

            if (deepest != database &&
                deepest->getFullPath() == namespacePath.toUtf8()) {
              timer->stop();
              timer->deleteLater();
            }
          });

  timer->start();
}
