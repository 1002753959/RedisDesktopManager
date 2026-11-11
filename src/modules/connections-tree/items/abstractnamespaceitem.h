#pragma once
#include <qredisclient/connection.h>
#include <QRegExp>
#include <QSharedPointer>
#include <QString>
#include <QtConcurrent>

#include "memoryusage.h"
#include "treeitem.h"
#include "modules/common/callbackwithowner.h"

namespace ConnectionsTree {

class Operations;
class AbstractNamespaceItem;
class Model;
class KeyItem;

class AbstractNamespaceItem : public QObject, public TreeItem, public MemoryUsage {

 public:
  AbstractNamespaceItem(Model& model, QWeakPointer<TreeItem> parent,
                        QSharedPointer<Operations> operations, uint dbIndex,
                        QRegExp filter = QRegExp());

  virtual ~AbstractNamespaceItem() {}

  QList<QSharedPointer<TreeItem>> getAllChilds() const override;

  QList<QSharedPointer<AbstractNamespaceItem>> getAllChildNamespaces() const;

  uint childCount(bool recursive = false) const override;

  uint keysCount() const;

  uint keysRenderingLimit() const;

  bool keysShortNameRendering() const;

  QSharedPointer<TreeItem> child(uint row) override;

  QWeakPointer<TreeItem> parent() const override;

  virtual void append(QSharedPointer<TreeItem> item, bool notifyModel=true);

  virtual void insertChild(QSharedPointer<TreeItem> item);

  virtual void appendKeyToIndex(QSharedPointer<KeyItem> key);

  virtual void removeNamespacedKeysFromIndex(QByteArray nsPrefix);

  virtual QHash<QByteArray, QWeakPointer<KeyItem>> getKeysIndex() const;

  virtual void removeObsoleteKeys(QList<QWeakPointer<KeyItem>> keys);

  void removeChild(int index) override;

  virtual void appendRawKey(const QByteArray& k);

  virtual void appendNamespace(QSharedPointer<AbstractNamespaceItem> item);

  virtual QSharedPointer<AbstractNamespaceItem> findChildNamespace(
      const QByteArray& name) {
    if (!m_childNamespaces.contains(name))
      return QSharedPointer<AbstractNamespaceItem>();

    return m_childNamespaces[name];
  }

  virtual uint getDbIndex() { return m_dbIndex; }

  virtual QSharedPointer<Operations> operations() { return m_operations; }

  virtual QRegExp getFilter() const { return m_filter; }

  virtual void showLoadingError(const QString& err);

  void cancelCurrentOperation() override;

  void getMemoryUsage(std::function<void(qlonglong)>) override; 

  void fetchMore() override;

  /*
   * Cursor of the interrupted SCAN session. "0" means that the whole
   * (filtered) keyspace was already traversed and there is nothing to load.
   */
  QByteArray nextScanCursor() const { return m_nextCursor; }

  void setNextScanCursor(const QByteArray& cursor) { m_nextCursor = cursor; }

  /*
   * Progress of the running scan (amount of keys collected by the server
   * side SCAN session). Reported from the connection thread.
   */
  void notifyLoadingProgress(long scannedKeys);

  void showLoadingProgress(long scannedKeys);

  void beginScanProgress(bool loadAll = false);

  void endScanProgress();

  /*
   * "Load all" support -----------------------------------------------------
   *
   * loadAllKeys() is triggered by the dedicated inline menu button: every
   * limit (keysLoadLimit / scanMaxIterations / aggregate limit) is dropped
   * and the whole filtered keyspace is returned in one page. The received
   * keys are then rendered in background chunks, so the UI stays responsive
   * and reports the progress.
   */
  virtual void loadAllKeys() {}

  bool isLoadAllMode() const { return m_loadAll; }

  /*
   * 0..100 when the total amount of keys is known, -1 otherwise
   * (indeterminate progress).
   */
  int loadingProgressPercent() const;

  bool isLoadingActive() const;

  /*
   * Exposes the loading progress to the tree view delegate
   * ("loading_progress": 0..100 or -1; "loading_active": bool).
   */
  QVariantMap metadata() const override;

 protected:
  /*
   * Chunked rendering of the keys received by "Load all".
   */
  void startAutoRender();

  void autoRenderStep();

  /*
   * Total amount of keys the scanned keyspace is expected to contain
   * (0 - unknown, progress is reported as an indeterminate/counter value).
   */
  virtual long loadingTotalKeys() const { return 0; }

  /*
   * Safety net: the item must never stay in "loading" state forever. The
   * watchdog is re-armed while the scan keeps reporting progress, so a long
   * "Load all" on a huge keyspace is not killed by the timeout.
   */
  void startProgressWatchdog(int seq, std::function<bool()> isFinished,
                             const QString& logTag);

 protected:
  /*
   * Loads the next page of keys from the interrupted SCAN session.
   * Implemented by the concrete items (database / namespace).
   */
  virtual void loadNextPage() {}

  QString loadingProgressSuffix() const;

  virtual void clear();

  virtual void ensureLoaderIsCreated();

  virtual void clearLoader();

  void sortChilds();

  using RenderRawKeysCallback = CallbackWithOwner<TreeItem>;

  void renderRawKeys(const RedisClient::Connection::RawKeysList& keylist, QRegExp filter,
                     QSharedPointer<RenderRawKeysCallback> callback,
                     bool appendNewItems,
                     bool checkPreRenderedItems,
                     int maxChildItems=-1);  

  QHash<QString, std::function<bool()>> eventHandlers() override;

  void calculateUsedMemory(QSharedPointer<AsyncFuture::Deferred<qlonglong>> parentD, std::function<void(qlonglong)> callback);

  void restoreOpenedNamespaces(QSharedPointer<AbstractNamespaceItem> ns);

 protected:
  QWeakPointer<TreeItem> m_parent;
  QSharedPointer<Operations> m_operations;
  QList<QSharedPointer<TreeItem>> m_childItems;
  QHash<QByteArray, QSharedPointer<AbstractNamespaceItem>> m_childNamespaces;
  QList<QByteArray> m_rawChildKeys;
  QRegExp m_filter;  
  uint m_dbIndex;
  QSharedPointer<AsyncFuture::Deferred<qlonglong>> m_runningOperation;
  bool m_showNsOnTop;  
  QHash<QByteArray, QWeakPointer<KeyItem>> m_keysIndex;  
  QByteArray m_nextCursor = QByteArray("0");
  long m_loadingProgress = 0;
  bool m_isScanning = false;
  bool m_bypassCache = false;

  /*
   * "Load all" state:
   *  - m_loadAll       - the item is in "load all" mode (persists until next
   *                      regular reload);
   *  - m_loadAllRendering - the received keys are being rendered in chunks;
   *  - m_loadAllTotal / m_loadAllRendered - rendering progress;
   *  - m_loadAllLoaded - amount of keys produced by the last "load all";
   *  - m_loadAllDone   - the last "load all" was completed successfully.
   */
  bool m_loadAll = false;
  bool m_loadAllRendering = false;
  long m_loadAllTotal = 0;
  long m_loadAllRendered = 0;
  long m_loadAllLoaded = 0;
  bool m_loadAllDone = false;
  int m_loadAllStalledSteps = 0;
  /*
   * Amount of keys returned by the last keys page: the exact total for the
   * "load all" rendering progress (keysCount() of a database is the size of
   * the whole keyspace, which is bigger than a filtered result).
   */
  long m_lastPageKeys = 0;
};
}  // namespace ConnectionsTree
