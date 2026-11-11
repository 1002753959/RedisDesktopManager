#pragma once
#include "abstractnamespaceitem.h"

namespace ConnectionsTree {

class ServerItem;

class DatabaseItem : public AbstractNamespaceItem {
 public:
  DatabaseItem(unsigned int index, int keysCount,
               QSharedPointer<Operations> operations,
               QWeakPointer<TreeItem> parent, Model& model);

  ~DatabaseItem();

  QByteArray getName() const override;

  QByteArray getFullPath() const override;

  QString getDisplayName() const override;

  QString type() const override { return "database"; }

  bool isEnabled() const override;

  QVariantMap metadata() const override;

  void setMetadata(const QString&, QVariant) override;

  void getMemoryUsage(std::function<void(qlonglong)> callback) override;

  void reload(std::function<void()> callback = std::function<void()>());

  /*
   * "Load all" button: reloads the database dropping every loading limit
   * (whole filtered keyspace in one page, chunked rendering).
   */
  void loadAllKeys() override;

 protected:
  /*
   * Amount of keys reported by the server (INFO keyspace) - used as the total
   * for the scan progress percentage.
   */
  long loadingTotalKeys() const override;

  void loadKeys(std::function<void()> callback = std::function<void()>(),
                bool partialReload=false);
  void unload(bool notify = true);
  void performLiveUpdate();
  void filterKeys(const QRegExp& filter);
  void resetFilter();

  QHash<QString, std::function<bool()>> eventHandlers() override;

 private:
  QSharedPointer<QTimer> liveUpdateTimer();
  bool isLiveUpdateEnabled() const;
  QVariantList filterHistoryTop10() const;

 private:
  void startLoadWatchdog(int seq);

  bool isKeysListTruncated(int keysCount) const;

  void loadNextPage() override;

 private:
  unsigned int m_keysCount;
  QSharedPointer<QTimer> m_liveUpdateTimer;
  int m_loadSeq = 0;
  int m_loadFinishedSeq = -1;
  bool m_keysTruncated = false;
  int m_loadedKeysCount = 0;
};

}  // namespace ConnectionsTree
