#include "abstractnamespaceitem.h"

#include <QApplication>
#include <QLocale>
#include <QMessageBox>
#include <QSettings>
#include <QThread>
#include <QTimer>
#include <algorithm>
#include <functional>

#include "app/applog.h"
#include "connections-tree/keysrendering.h"
#include "connections-tree/model.h"
#include "connections-tree/operations.h"
#include "keyitem.h"
#include "loadmoreitem.h"
#include "namespaceitem.h"

using namespace ConnectionsTree;

AbstractNamespaceItem::AbstractNamespaceItem(
    Model &model, QWeakPointer<TreeItem> parent,
    QSharedPointer<Operations> operations, uint dbIndex, QRegExp filter)
    : TreeItem(model),
      m_parent(parent),
      m_operations(operations),
      m_filter(filter.isEmpty() ? QRegExp(operations->defaultFilter())
                                : filter),      
      m_dbIndex(dbIndex),
      m_runningOperation(nullptr) {
  QSettings settings;
  m_showNsOnTop = settings
                      .value("app/showNamespacesOnTop",
#if defined(Q_OS_WINDOWS)
                             true
#else
                             false
#endif
                             )
                      .toBool();
}

QList<QSharedPointer<TreeItem>> AbstractNamespaceItem::getAllChilds() const {
  return m_childItems;
}

QList<QSharedPointer<AbstractNamespaceItem>>
AbstractNamespaceItem::getAllChildNamespaces() const {
  return m_childNamespaces.values();
}

QSharedPointer<TreeItem> AbstractNamespaceItem::child(uint row) {
  if (row < m_childItems.size()) return m_childItems.at(row);

  return QSharedPointer<TreeItem>();
}

QWeakPointer<TreeItem> AbstractNamespaceItem::parent() const {
  return m_parent;
}

void AbstractNamespaceItem::append(QSharedPointer<TreeItem> item, bool notifyModel) {
  if (notifyModel)
    m_model.beforeChildLoadedAtPos(getSelf(), m_childItems.size());
  m_childItems.append(item);
  if (notifyModel) m_model.childLoaded(getSelf());
}

bool compareTreeItemsByName(QSharedPointer<TreeItem> first,
                            QSharedPointer<TreeItem> second) {
  return first->getDisplayName() < second->getDisplayName();
}

bool compareTreeItemsByNameAndNsOnTop(QSharedPointer<TreeItem> first,
                                      QSharedPointer<TreeItem> second) {
  if (first->type() != second->type()) return first->type() > second->type();

  return first->getDisplayName() < second->getDisplayName();
}

void AbstractNamespaceItem::insertChild(QSharedPointer<TreeItem> item) {
  auto pos = std::upper_bound(m_childItems.begin(), m_childItems.end(), item,
                              m_showNsOnTop ? compareTreeItemsByNameAndNsOnTop
                                            : compareTreeItemsByName);

  int index = std::distance(m_childItems.begin(), pos);

  m_model.beforeChildLoadedAtPos(getSelf(), index);
  m_childItems.insert(pos, item);
  m_model.childLoaded(getSelf());
}

void AbstractNamespaceItem::appendKeyToIndex(QSharedPointer<KeyItem> key) {
  if (!key) return;

  m_keysIndex.insert(key->getFullPath(), key.toWeakRef());
}

void AbstractNamespaceItem::removeNamespacedKeysFromIndex(QByteArray nsPrefix) {
  auto keys = m_keysIndex.keys();

  for (auto const &fullPath : qAsConst(keys)) {
    if (fullPath.startsWith(nsPrefix)) {
      m_keysIndex.remove(fullPath);
    }
  }
}

QHash<QByteArray, QWeakPointer<KeyItem>> AbstractNamespaceItem::getKeysIndex()
    const {
  return m_keysIndex;
}

QSharedPointer<TreeItem> resolveItemToRemove(QSharedPointer<TreeItem> item) {
  if (!item) return item;

  auto parent = item->parent().toStrongRef();

  if (!parent || parent->type() == "database") return item;

  if (parent->type() == "namespace" && parent->getAllChilds().empty())
    return resolveItemToRemove(parent);

  return item;
}

void AbstractNamespaceItem::removeObsoleteKeys(
    QList<QWeakPointer<KeyItem>> keys) {
  for (const auto &obsoleteKey : keys) {
    auto key = obsoleteKey.toStrongRef();

    if (!key) continue;

    m_keysIndex.remove(key->getFullPath());

    auto parent = key->parent().toStrongRef();

    if (!parent) continue;

    QSharedPointer<TreeItem> itemToRemoveFromModel = key;

    if (parent->type() == "namespace" && parent->getAllChilds().size() == 1) {
      itemToRemoveFromModel = resolveItemToRemove(parent);
    }

    if (!itemToRemoveFromModel) continue;

    int row = itemToRemoveFromModel->row();

    m_model.beforeItemChildRemoved(itemToRemoveFromModel->parent(), row);

    auto parentHoldsItemToRemove =
        itemToRemoveFromModel->parent().toStrongRef();

    if (!parentHoldsItemToRemove) continue;

    parentHoldsItemToRemove->removeChild(row);

    m_model.itemChildRemoved(itemToRemoveFromModel);
  }
}

void AbstractNamespaceItem::removeChild(int index)
{
    bool validIndex = 0 < index && index < m_childItems.size();
    if (!validIndex)
        return;

    m_childItems.removeAt(index);
}

void AbstractNamespaceItem::appendRawKey(const QByteArray &k) {
  m_rawChildKeys.append(k);
}

void AbstractNamespaceItem::appendNamespace(
    QSharedPointer<AbstractNamespaceItem> item) {
  m_childNamespaces[item->getName()] = item;
  insertChild(item.staticCast<TreeItem>());
}

uint AbstractNamespaceItem::childCount(bool recursive) const {
  uint count = 0;

  if (!recursive) {
    count += m_childItems.size();
    return count;
  }

  for (const auto &item : m_childItems) {
    if (item->supportChildItems()) {
      count += item->childCount(true);
    } else {
      count += 1;
    }
  }
  return count;
}

uint AbstractNamespaceItem::keysCount() const {
  uint count = m_rawChildKeys.size();

  for (const auto &item : m_childItems) {
    if (item->supportChildItems()) {
      auto ns = item.dynamicCast<AbstractNamespaceItem>();
      if (ns) {
        count += ns->keysCount();
      }
    } else if (item->type() == "key") {
      count += 1;
    }
  }
  return count;
}

uint AbstractNamespaceItem::keysRenderingLimit() const {
  QSettings appSettings;
  return appSettings.value("app/treeItemMaxChilds", 1000).toUInt();
}

bool AbstractNamespaceItem::keysShortNameRendering() const {
  QSettings appSettings;
  return appSettings.value("app/namespacedKeysShortName", true).toBool();
}

void AbstractNamespaceItem::clear() {
  clearLoader();

  /*
   * Pending "Load all" state is dropped together with the loaded keys.
   * Note: m_loadAll (the mode flag) is managed by the concrete items.
   */
  m_loadAllRendering = false;
  m_loadAllTotal = 0;
  m_loadAllRendered = 0;

  bool notifyModel = false;

  if (m_childItems.size() > 0) {
    notifyModel = true;
    m_model.beforeItemChildsUnloaded(getSelf());
  }

  m_childItems.clear();
  m_childNamespaces.clear();
  m_rawChildKeys.clear();
  m_usedMemory = 0;

  if (type() == "database") {
    m_keysIndex.clear();
  } else {
    auto selfRef = getSelf().toStrongRef().dynamicCast<AbstractNamespaceItem>();
    if (selfRef) {
      auto root = resolveRootItem(selfRef);

      if (root) {
        root->removeNamespacedKeysFromIndex(getFullPath());
      }
    }
  }

  if (notifyModel) {
    m_model.itemChildRemoved(getSelf());
  }
}

void AbstractNamespaceItem::clearLoader() {
  if (m_childItems.empty()) {
    return;
  }

  auto lastItem = m_childItems.last();

  if (!lastItem || lastItem->type() != "loader") return;

  m_model.beforeItemChildRemoved(getSelf(), m_childItems.size() - 1);
  m_childItems.removeLast();
  m_model.itemChildRemoved(lastItem.toWeakRef());
}

void AbstractNamespaceItem::showLoadingError(const QString &err) {
  emit m_model.itemChanged(getSelf());
  emit m_model.error(err);
}

void AbstractNamespaceItem::cancelCurrentOperation() {
  /*
   * "Load all" rendering runs in the GUI thread in chunks: it must be stopped
   * explicitly, otherwise the keys keep coming into the tree after the
   * connection was reset.
   */
  if (m_loadAllRendering) {
    m_loadAllRendering = false;
    AppLog::put(
        QString("LOADALL render canceled type=%1 name=%2 rendered=%3/%4")
            .arg(type())
            .arg(getDisplayName())
            .arg(m_loadAllRendered)
            .arg(m_loadAllTotal));
    emit m_model.itemChanged(getSelf());
  }

  if (m_runningOperation) {
    m_runningOperation->future().cancel();
    m_operations->resetConnection();
    unlock();
  }
}

bool compareChilds(QSharedPointer<TreeItem> first,
                   QSharedPointer<TreeItem> second) {
  auto firstMemoryItem = first.dynamicCast<MemoryUsage>();
  auto secondMemoryItem = second.dynamicCast<MemoryUsage>();

  if (!firstMemoryItem)
    qDebug() << "Invalid tree item:" << first->getDisplayName();
  if (!secondMemoryItem)
    qDebug() << "Invalid tree item:" << second->getDisplayName();

  return (firstMemoryItem ? firstMemoryItem->usedMemory() : 0) >
         (secondMemoryItem ? secondMemoryItem->usedMemory() : 0);
}

void AbstractNamespaceItem::sortChilds() {
  m_model.beforeItemLayoutChanged(getSelf());
  std::sort(m_childItems.begin(), m_childItems.end(), compareChilds);
  m_model.itemLayoutChanged(getSelf());
  emit m_model.itemChanged(getSelf());
}

void AbstractNamespaceItem::renderRawKeys(
    const RedisClient::Connection::RawKeysList &keylist, QRegExp filter,
    QSharedPointer<RenderRawKeysCallback> callback, bool appendNewItems,
    bool checkPreRenderedItems, int maxChildItems) {
  if (!m_operations) {
    return;
  }

  uint renderingLimit =
      qMax(static_cast<uint>(m_childItems.size()), keysRenderingLimit());

  if (maxChildItems > 0) {
    renderingLimit = static_cast<uint>(maxChildItems);
  } 

  auto settings = ConnectionsTree::KeysTreeRenderer::RenderingSettigns{
      filter,         m_operations->getNamespaceSeparator(),
      getDbIndex(),   renderingLimit,
      appendNewItems, checkPreRenderedItems, keysShortNameRendering()};

  auto future = QtConcurrent::run(
      [](QList<QByteArray> keylist) {
        std::sort(keylist.begin(), keylist.end());
        return keylist;
      },
      keylist);

  auto selfWPtr = getSelf();

  AsyncFuture::observe(future).subscribe([selfWPtr, this, settings, callback, future]() {

    auto self = selfWPtr.toStrongRef();

    if (!self)
      return;

    ConnectionsTree::KeysTreeRenderer::renderKeys(
        m_operations, future.result(),
        qSharedPointerDynamicCast<AbstractNamespaceItem>(self), settings,
        m_model.expandedNamespaces);
    if (callback)
      callback->call();
  });
}

void AbstractNamespaceItem::ensureLoaderIsCreated() {
  if (m_rawChildKeys.empty() || m_childItems.empty()) {
    return;
  }

  auto lastItem = m_childItems.last();

  if (lastItem->type() == "loader") return;

  m_model.beforeChildLoaded(getSelf(), 1);
  m_childItems.append(
      QSharedPointer<TreeItem>(new LoadMoreItem(getSelf(), m_model)));
  m_model.childLoaded(getSelf());
}

QHash<QString, std::function<bool ()> > AbstractNamespaceItem::eventHandlers() {
  auto events = TreeItem::eventHandlers();

  events.insert("analyze_memory_usage", [this]() {
    if (m_usedMemory > 0) return true;

    auto future = m_operations->connectionSupportsMemoryOperations();

    auto selfWPtr = getSelf();

    AsyncFuture::observe(future).subscribe([selfWPtr, this](bool isSupported) {
      auto self = selfWPtr.toStrongRef();

      if (!self)
        return;

      if (!isSupported) {
        emit m_model.error(QCoreApplication::translate(
            "RESP",
            "Your redis-server doesn't support <a "
            "href='https://redis.io/commands/memory-usage'><b>MEMORY</b></a> "
            "commands."));
        unlock();
        return;
      }

      getMemoryUsage([selfWPtr, this](qlonglong) {
        QTimer::singleShot(0, this, [this]() {
          sortChilds();
          unlock();
          m_runningOperation.clear();
        });
      });
    });
    return false;
  });

  return events;
}

void AbstractNamespaceItem::getMemoryUsage(
    std::function<void(qlonglong)> callback) {
  m_usedMemory = 0;

  m_runningOperation = QSharedPointer<AsyncFuture::Deferred<qlonglong>>(
      new AsyncFuture::Deferred<qlonglong>());

  QtConcurrent::run(this, &AbstractNamespaceItem::calculateUsedMemory,
                    m_runningOperation, callback);

  return;
}

void AbstractNamespaceItem::fetchMore() {
  /*
   * 1) Keys which were already received but not rendered yet (rendering
   *    limit). The whole scan result is kept in m_rawChildKeys, so "Load more"
   *    is a pure UI operation: no server round-trip at all.
   */
  if (!m_rawChildKeys.isEmpty()) {
    clearLoader();

    int childsCount = m_childItems.size();
    auto rawKeys = m_rawChildKeys;

    emit m_model.itemChanged(getSelf());

    m_rawChildKeys.clear();

    auto callback = QSharedPointer<RenderRawKeysCallback>(
        new RenderRawKeysCallback(getSelf(), [this]() {
          ensureLoaderIsCreated();

          unlock();
        }));

    return renderRawKeys(
        rawKeys, m_filter, callback,
        true, false, static_cast<uint>(childsCount) + keysRenderingLimit());
  }

  /*
   * 2) The SCAN session was interrupted (aggregate limit / first screen
   *    limit). Continue it from the saved cursor - real pagination.
   */
  if (m_nextCursor != QByteArray("0")) {
    loadNextPage();
    return;
  }

  /*
   * 3) The keyspace was traversed completely - just drop the "Load more"
   *    pseudo item.
   */
  clearLoader();
  emit m_model.itemChanged(getSelf());
}

void AbstractNamespaceItem::showLoadingProgress(long scannedKeys) {
  if (scannedKeys <= 0) return;

  m_loadingProgress = scannedKeys;

  if (!m_isScanning) return;

  emit m_model.itemChanged(getSelf());
}

void AbstractNamespaceItem::notifyLoadingProgress(long scannedKeys) {
  /*
   * The progress is reported directly from the connection thread, the model
   * must be touched from the GUI thread only.
   */
  QMetaObject::invokeMethod(
      this, [this, scannedKeys]() { showLoadingProgress(scannedKeys); },
      Qt::QueuedConnection);
}

void AbstractNamespaceItem::beginScanProgress(bool loadAll) {
  m_isScanning = true;
  m_loadingProgress = 0;
  m_loadAll = loadAll;
  m_loadAllDone = false;
  m_loadAllLoaded = 0;
}

void AbstractNamespaceItem::endScanProgress() {
  if (!m_isScanning) return;

  m_isScanning = false;
  m_loadingProgress = 0;
  emit m_model.itemChanged(getSelf());
}

int AbstractNamespaceItem::loadingProgressPercent() const {
  if (m_loadAllRendering) {
    if (m_loadAllTotal <= 0) return -1;
    return qBound(
        0, static_cast<int>(m_loadAllRendered * 100 / m_loadAllTotal), 100);
  }

  if (!m_isScanning) return -1;

  const long total = loadingTotalKeys();

  if (total <= 0 || m_loadingProgress <= 0) return -1;

  return qBound(0, static_cast<int>(m_loadingProgress * 100 / total), 100);
}

bool AbstractNamespaceItem::isLoadingActive() const {
  return m_isScanning || m_loadAllRendering;
}

QVariantMap AbstractNamespaceItem::metadata() const {
  QVariantMap meta = TreeItem::metadata();

  meta["loading_active"] = isLoadingActive();
  meta["loading_progress"] = loadingProgressPercent();

  return meta;
}

QString AbstractNamespaceItem::loadingProgressSuffix() const {
  if (m_loadAllRendering) {
    const int pct = loadingProgressPercent();

    if (pct >= 0)
      return QString(" <i>[load all: rendering %1 / %2 - %3%]</i>")
          .arg(QLocale().toString(static_cast<qlonglong>(m_loadAllRendered)))
          .arg(QLocale().toString(static_cast<qlonglong>(m_loadAllTotal)))
          .arg(pct);

    return QString(" <i>[load all: rendering %1 keys]</i>")
        .arg(QLocale().toString(static_cast<qlonglong>(m_loadAllRendered)));
  }

  if (m_isScanning) {
    if (m_loadAll) {
      const int pct = loadingProgressPercent();

      if (pct >= 0)
        return QString(" <i>[load all: %1% - %2 keys scanned]</i>")
            .arg(pct)
            .arg(QLocale().toString(static_cast<qlonglong>(m_loadingProgress)));

      if (m_loadingProgress > 0)
        return QString(" <i>[load all: %1 keys scanned]</i>")
            .arg(QLocale().toString(static_cast<qlonglong>(m_loadingProgress)));

      return QString(" <i>[load all: scanning...]</i>");
    }

    if (m_loadingProgress <= 0) return QString();

    return QString(" <i>[scanning: %1]</i>")
        .arg(QLocale().toString(static_cast<qlonglong>(m_loadingProgress)));
  }

  if (m_loadAllDone && m_loadAllLoaded > 0)
    return QString(" <i>[all keys: %1]</i>")
        .arg(QLocale().toString(static_cast<qlonglong>(m_loadAllLoaded)));

  return QString();
}

void AbstractNamespaceItem::startAutoRender() {
  if (!m_loadAll || m_loadAllRendering) return;

  if (m_rawChildKeys.isEmpty()) {
    m_loadAllDone = true;
    m_loadAllLoaded = static_cast<long>(keysCount());
    emit m_model.itemChanged(getSelf());
    return;
  }

  /*
   * The keys were sorted by the renderer of the first screen, so the
   * remaining tail is already in ascending order: the chunks can simply be
   * appended one after another.
   */
  m_loadAllRendering = true;
  m_loadAllTotal = m_lastPageKeys > 0
                       ? m_lastPageKeys
                       : static_cast<long>(keysCount());
  m_loadAllTotal =
      qMax(m_loadAllTotal, static_cast<long>(m_rawChildKeys.size()));
  m_loadAllRendered =
      qMax(0L, m_loadAllTotal - static_cast<long>(m_rawChildKeys.size()));
  m_loadAllStalledSteps = 0;

  AppLog::put(QString("LOADALL render start type=%1 name=%2 keys=%3")
                  .arg(type())
                  .arg(getDisplayName())
                  .arg(m_loadAllTotal));

  emit m_model.itemChanged(getSelf());

  autoRenderStep();
}

void AbstractNamespaceItem::autoRenderStep() {
  if (!m_loadAllRendering) return;

  if (m_rawChildKeys.isEmpty()) {
    m_loadAllRendering = false;
    m_loadAllDone = true;
    m_loadAllLoaded = m_loadAllTotal;

    ensureLoaderIsCreated();

    AppLog::put(QString("LOADALL render done type=%1 name=%2 keys=%3 "
                        "childs=%4")
                    .arg(type())
                    .arg(getDisplayName())
                    .arg(m_loadAllLoaded)
                    .arg(m_childItems.size()));

    emit m_model.itemChanged(getSelf());
    return;
  }

  QSettings settings;
  const int chunk = qBound(
      100, settings.value("app/loadAllRenderChunk", 2000).toInt(), 20000);
  const int take = qMin(chunk, static_cast<int>(m_rawChildKeys.size()));

  const auto part = m_rawChildKeys.mid(0, take);
  m_rawChildKeys = m_rawChildKeys.mid(take);

  const long renderedBefore = m_loadAllRendered;

  m_loadAllRendered =
      qMax(0L, m_loadAllTotal - static_cast<long>(m_rawChildKeys.size()));

  clearLoader();

  /*
   * The render limit is absolute (amount of child items the renderer may
   * keep), and the renderer compares it with the recursive child count.
   */
  const int renderLimit =
      static_cast<int>(getAllChilds().size()) + take + 1;

  if (renderedBefore == m_loadAllRendered) {
    /*
     * Safety net: the renderer rejected the whole chunk (every key went back
     * to the raw list). Never loop on it - stop and let the user continue
     * with the regular "Load more keys" item.
     */
    ++m_loadAllStalledSteps;

    if (m_loadAllStalledSteps >= 2) {
      m_loadAllRendering = false;
      m_rawChildKeys = part + m_rawChildKeys;

      AppLog::put(QString("LOADALL render stalled type=%1 name=%2 rendered=%3 "
                          "left=%4 -> stopped")
                      .arg(type())
                      .arg(getDisplayName())
                      .arg(m_loadAllRendered)
                      .arg(m_rawChildKeys.size()));

      ensureLoaderIsCreated();
      emit m_model.itemChanged(getSelf());
      return;
    }
  } else {
    m_loadAllStalledSteps = 0;
  }

  auto rendered = QSharedPointer<RenderRawKeysCallback>(
      new RenderRawKeysCallback(getSelf(), [this]() {
        if (!m_loadAllRendering) return;

        /*
         * Repaint the counter between the chunks and give the event loop a
         * chance to process user input (the tree stays responsive while
         * hundreds of thousands of keys are being rendered).
         */
        emit m_model.itemChanged(getSelf());

        QTimer::singleShot(0, this, [this]() { autoRenderStep(); });
      }));

  renderRawKeys(part, m_filter, rendered, true, false, renderLimit);
}

void AbstractNamespaceItem::startProgressWatchdog(
    int seq, std::function<bool()> isFinished, const QString &logTag) {
  QSettings settings;

  int timeoutSec = settings.value("app/keysLoadTimeout", 120).toInt();

  if (timeoutSec <= 0) return;

  auto lastSeen = QSharedPointer<long>(new long(m_loadingProgress));
  auto tick = QSharedPointer<std::function<void()>>::create();

  auto arm = [this, seq, timeoutSec, lastSeen, isFinished, logTag,
              tick]() {
    QTimer::singleShot(
        timeoutSec * 1000, this,
        [this, seq, timeoutSec, lastSeen, isFinished, logTag, tick]() {
          if (isFinished()) return;

          /*
           * Re-arm: the scan is still running and reports progress. "Load all"
           * on a huge keyspace legitimately takes longer than the timeout,
           * while a dead connection produces no progress at all.
           */
          if (m_loadingProgress != *lastSeen) {
            *lastSeen = m_loadingProgress;

            AppLog::put(QString("%1 WATCHDOG seq=%2 re-armed (progress=%3)")
                            .arg(logTag)
                            .arg(seq)
                            .arg(m_loadingProgress));

            if (*tick) (*tick)();
            return;
          }

          m_loadAllRendering = false;
          endScanProgress();
          unlock();

          AppLog::put(
              QString("%1 WATCHDOG seq=%2 timeout=%3s progress=%4 -> unlocked "
                      "with error")
                  .arg(logTag)
                  .arg(seq)
                  .arg(timeoutSec)
                  .arg(m_loadingProgress));

          showLoadingError(QCoreApplication::translate(
                               "RESP",
                               "Keys loading timed out (%1s).<br/>"
                               "Try to reduce the amount of loaded keys "
                               "(connection settings) or use a more specific "
                               "keys filter.")
                               .arg(timeoutSec));
        });
  };

  *tick = arm;
  arm();
}

void AbstractNamespaceItem::calculateUsedMemory(
    QSharedPointer<AsyncFuture::Deferred<qlonglong>> parentDeffered,
    std::function<void(qlonglong)> callback) {
  if (parentDeffered && parentDeffered->future().isCanceled()) {
    return;
  }

  if (m_rawChildKeys.size() > 0) {
    auto resultCallback = QSharedPointer<Operations::GetUsedMemoryCallback>(
        new Operations::GetUsedMemoryCallback(
            getSelf(), [this, callback](qlonglong result) {
              m_usedMemory = result;
              emit m_model.itemChanged(getSelf());

              if (m_childItems.empty()) callback(result);
            }));

    auto progressCallback = QSharedPointer<Operations::GetUsedMemoryCallback>(
        new Operations::GetUsedMemoryCallback(
            getSelf(), [this](qlonglong progress) {
              m_usedMemory = progress;
              emit m_model.itemChanged(getSelf());
            }));

    operations()->getUsedMemory(m_rawChildKeys, m_dbIndex, resultCallback,
                                progressCallback);
  }

  auto resultsRemaining = QSharedPointer<qlonglong>(new qlonglong(0));

  auto updateUsedMemoryValue = [this, resultsRemaining,
                                callback](qlonglong result) {
    if (!resultsRemaining) return;

    QMutexLocker locker(&m_updateUsedMemoryMutex);
    Q_UNUSED(locker);
    m_usedMemory += result;
    emit m_model.itemChanged(getSelf());

    (*resultsRemaining)--;

    if (*resultsRemaining <= 0) {
      callback(m_usedMemory);
    }
  };

  (*resultsRemaining) += m_childNamespaces.size();

  for (auto childNs : qAsConst(m_childNamespaces)) {
    if (parentDeffered->future().isCanceled()) {
      return;
    }
    childNs->calculateUsedMemory(parentDeffered, updateUsedMemoryValue);
  }

  for (const QSharedPointer<TreeItem> &child : qAsConst(m_childItems)) {
    if (parentDeffered->future().isCanceled()) {
      return;
    }

    if (!child || child->type() != "key") continue;

    auto memoryItem = child.dynamicCast<MemoryUsage>();

    if (!memoryItem) continue;

    QMutexLocker locker(&m_updateUsedMemoryMutex);
    (*resultsRemaining)++;
    memoryItem->getMemoryUsage(updateUsedMemoryValue);
  }
}

void AbstractNamespaceItem::restoreOpenedNamespaces(QSharedPointer<AbstractNamespaceItem> ns) {
  if (ns->type() == "namespace" && !ns->isExpanded()) return;

  if (ns->isExpanded())
      m_model.expandItem(ns.staticCast<TreeItem>().toWeakRef());

  auto childs = ns->getAllChildNamespaces();

  for (auto childNs : childs) {
    restoreOpenedNamespaces(childNs);
  }
}
