#include "namespaceitem.h"
#include <qredisclient/utils/text.h>
#include <QCoreApplication>
#include <QMenu>
#include <QMessageBox>
#include <QSettings>
#include <QTimer>
#include "app/applog.h"
#include "app/apputils.h"
#include "connections-tree/model.h"
#include "connections-tree/utils.h"
#include "connections-tree/keysrendering.h"
#include "databaseitem.h"
#include "keyitem.h"
#include "loadmoreitem.h"

using namespace ConnectionsTree;

NamespaceItem::NamespaceItem(const QByteArray &fullPath,
                             QSharedPointer<Operations> operations,
                             QWeakPointer<TreeItem> parent, Model &model,
                             uint dbIndex, QRegExp filter)
    : AbstractNamespaceItem(model, parent, operations, dbIndex, filter),
      m_fullPath(fullPath),
      m_removed(false) {}

QString NamespaceItem::getDisplayName() const {
  QString title = QString("%1 (%2)")
                      .arg(printableString(getName(), true))
                      .arg(keysCount());

  title.append(loadingProgressSuffix());

  if (m_keysTruncated) {
    title.append(QString(" %1").arg(QCoreApplication::translate(
                                       "RESP", "[limited]")));
  }

  if (m_usedMemory > 0) {
    title.append(QString(" <b>[%1]</b>").arg(humanReadableSize(m_usedMemory)));
  }

  return title;
}

QByteArray NamespaceItem::getName() const {
  qsizetype pos = m_fullPath.lastIndexOf(m_operations->getNamespaceSeparator());

  if (pos >= 0) {
      return m_fullPath.mid(pos + m_operations->getNamespaceSeparator().size());
  } else {
      return m_fullPath;
  }
}

bool NamespaceItem::isEnabled() const { return m_removed == false; }

QByteArray NamespaceItem::getFullPath() const { return m_fullPath; }

void NamespaceItem::setRemoved() {
  m_removed = true;

  clear();

  emit m_model.itemChanged(getSelf());
}

QVariantMap NamespaceItem::metadata() const {
  QVariantMap metadata = AbstractNamespaceItem::metadata();
  metadata["full_path"] = getFullPath();
  return metadata;
}

QString NamespaceItem::buildFilter() const {
  QString nsFilter = QString("%1%2*")
                         .arg(QString::fromUtf8(m_fullPath))
                         .arg(m_operations->getNamespaceSeparator());

  if (m_filter.isEmpty()) return nsFilter;

  const QString filterPattern = m_filter.pattern();
  const QString nsPrefix = nsFilter.chopped(1);  // "fullPath:"

  if (filterPattern.startsWith(nsPrefix)) {
    // Already relative to this namespace (e.g. "A6J:ORDER*")
    return filterPattern;
  }

  // Parent filter is a key-level glob (e.g. "A*"). Its literal head must
  // not be glued to the namespace path, otherwise we build dead patterns
  // like "A6J:A*" which match nothing.
  QString literalHead;
  for (const QChar &ch : filterPattern) {
    if (ch == '*' || ch == '?' || ch == '[') break;
    literalHead.append(ch);
  }

  if (!literalHead.isEmpty() &&
      QString::fromUtf8(m_fullPath).startsWith(literalHead)) {
    // Namespace path already satisfies the parent filter: "fullPath:*"
    return nsFilter;
  }

  return QString("%1%2").arg(nsPrefix).arg(filterPattern);
}

void NamespaceItem::load() {
  const int seq = ++m_loadSeq;
  m_keysTruncated = false;

  /*
   * Reload drops the cached page (see Operations::KeysLoadOptions).
   */
  const bool bypassCache = m_bypassCache;
  m_bypassCache = false;

  auto onKeysRendered = QSharedPointer<RenderRawKeysCallback>(
      new RenderRawKeysCallback(getSelf(), [this, seq]() {
        m_loadFinishedSeq = seq;
        endScanProgress();
        ensureLoaderIsCreated();

        AppLog::put(QString("NS render done seq=%1 path=%2 children=%3")
                        .arg(seq)
                        .arg(QString::fromUtf8(m_fullPath))
                        .arg(m_childItems.size()));

        unlock();
        setExpanded(true);
        emit m_model.itemChanged(getSelf());
        m_model.expandItem(getSelf());

        /*
         * "Load all": render the whole keyspace in background chunks.
         */
        if (m_loadAll) startAutoRender();
      }));

  if (m_rawChildKeys.size() > 0) {
    auto rawKeys = m_rawChildKeys;
    m_rawChildKeys.clear();

    m_loadFinishedSeq = seq;
    endScanProgress();

    return renderRawKeys(rawKeys, m_filter, onKeysRendered, true, false);
  }

  const QString nsFilter = buildFilter();

  Operations::KeysLoadOptions options;
  /*
   * Aggregated scan: the whole filtered keyspace is traversed, so every
   * child namespace gets an exact keys counter instead of a counter
   * computed from a truncated sample of the first N keys.
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

  auto callback = QSharedPointer<Operations::LoadNamespaceItemsCallback>(
      new Operations::LoadNamespaceItemsCallback(
          getSelf(), [this, onKeysRendered, seq](
                         const Operations::KeysPage &page,
                         const QString &err) {
            m_loadFinishedSeq = seq;
            endScanProgress();
            m_lastPageKeys = static_cast<long>(page.keys.size());

            AppLog::put(
                QString("NS keys returned seq=%1 path=%2 keys=%3 next=%4 err=%5")
                    .arg(seq)
                    .arg(QString::fromUtf8(m_fullPath))
                    .arg(page.keys.size())
                    .arg(QString::fromUtf8(page.nextCursor))
                    .arg(err.isEmpty() ? QString("<none>") : err));

            if (!err.isEmpty()) {
              m_keysTruncated = false;
              m_nextCursor = QByteArray("0");
              unlock();
              return showLoadingError(err);
            }

            /*
             * Remember the cursor of the interrupted SCAN session:
             * "Load more keys" continues the traversal from it.
             */
            m_nextCursor = page.nextCursor;

            m_keysTruncated = m_loadAll
                                  ? false
                                  : (page.nextCursor != QByteArray("0") ||
                                     isKeysListTruncated(page.keys.size()));

            /*
             * "Load all": only the first screen is rendered synchronously,
             * the remaining keys are appended in background chunks
             * (startAutoRender / m_rawChildKeys) to keep the tree responsive.
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

            return renderRawKeys(page.keys, m_filter, onKeysRendered, true,
                                 false, firstScreenLimit);
          }));

  AppLog::put(QString("NS load() seq=%1 path=%2 nsFilter=%3 rawChildKeys=%4 "
                      "aggregate=%5 useCache=%6 loadAll=%7")
                  .arg(seq)
                  .arg(QString::fromUtf8(m_fullPath))
                  .arg(nsFilter)
                  .arg(m_rawChildKeys.size())
                  .arg(options.aggregate)
                  .arg(options.useCache)
                  .arg(options.loadAll));

  beginScanProgress(m_loadAll);
  startLoadWatchdog(seq);

  try {
    m_operations->loadNamespaceItems(m_dbIndex, nsFilter, callback, options);
  } catch (const RedisClient::Connection::Exception &error) {
    m_loadFinishedSeq = seq;
    endScanProgress();
    unlock();
    showLoadingError(error.what());
  }
}

void NamespaceItem::reload() {
  /*
   * Regular reload (context menu "Reload" / Ctrl+R): bounded first screen.
   */
  m_loadAll = false;

  clear();
  m_nextCursor = QByteArray("0");
  m_bypassCache = true;
  load();
}

void NamespaceItem::loadAllKeys() {
  AppLog::put(QString("NS loadAllKeys() path=%1 filter=%2")
                  .arg(QString::fromUtf8(m_fullPath))
                  .arg(m_filter.pattern()));

  /*
   * Same pipeline as reload(), but every loading limit is dropped: the whole
   * filtered keyspace is requested and rendered in background chunks.
   */
  m_loadAll = true;

  clear();
  m_nextCursor = QByteArray("0");
  m_bypassCache = true;
  load();
}

bool NamespaceItem::isKeysListTruncated(int keysCount) const {
  qlonglong limit = m_operations->keysLoadLimit();

  return limit > 0 && keysCount >= limit;
}

void NamespaceItem::startLoadWatchdog(int seq) {
  startProgressWatchdog(
      seq, [this, seq]() { return seq != m_loadSeq || m_loadFinishedSeq == seq; },
      QString("NS[%1]").arg(QString::fromUtf8(m_fullPath)));
}

void NamespaceItem::loadNextPage() {
  const QByteArray startCursor = m_nextCursor;

  if (startCursor == QByteArray("0")) return;

  const QString nsFilter = buildFilter();

  AppLog::put(QString("NS loadNextPage path=%1 filter=%2 cursor=%3")
                  .arg(QString::fromUtf8(m_fullPath))
                  .arg(nsFilter)
                  .arg(QString::fromUtf8(startCursor)));

  lock();
  beginScanProgress();

  Operations::KeysLoadOptions options;
  /*
   * Continuation page: bounded scan, the aggregation limit must not be
   * applied again (otherwise "Load more" repeats the full scan).
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
          getSelf(), [this, startCursor](const Operations::KeysPage &page,
                                         const QString &err) {
            clearLoader();
            endScanProgress();

            if (!err.isEmpty()) {
              unlock();
              return showLoadingError(err);
            }

            m_nextCursor = page.nextCursor;

            AppLog::put(QString("NS page loaded path=%1 cursor=%2 keys=%3 next=%4")
                            .arg(QString::fromUtf8(m_fullPath))
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
    m_operations->loadNamespaceItems(m_dbIndex, nsFilter, callback, options);
  } catch (const RedisClient::Connection::Exception &error) {
    endScanProgress();
    unlock();
    showLoadingError(error.what());
  }
}

QHash<QString, std::function<bool()>> NamespaceItem::eventHandlers() {
  auto events = AbstractNamespaceItem::eventHandlers();

  events.insert("click", [this]() {
    if (m_childItems.size() == 0) {
      load();
      return false;
    } else if (!isExpanded()) {
      setExpanded(true);
      emit m_model.itemChanged(getSelf());
      m_model.expandItem(getSelf());      
    }
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
                  "selected namespace?"),
              [this]() { reload(); },
              QCoreApplication::translate("RESP", "Key was added"));
        }));
    m_operations->openNewKeyDialog(
        m_dbIndex, callback,
        QString("%1%2")
            .arg(QString::fromUtf8(getFullPath()))
            .arg(m_operations->getNamespaceSeparator()));
    return true;
  });

  events.insert("reload", [this]() { reload(); return false; });

  events.insert("load_all", [this]() { loadAllKeys(); return false; });

  events.insert("delete", [this]() { m_operations->deleteDbNamespace(*this); return true; });

  return events;
}
