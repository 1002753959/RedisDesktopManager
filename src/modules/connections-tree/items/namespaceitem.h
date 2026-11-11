#pragma once
#include "abstractnamespaceitem.h"

namespace ConnectionsTree {

class NamespaceItem : public AbstractNamespaceItem {
  Q_OBJECT

 public:
  NamespaceItem(const QByteArray& fullPath,
                QSharedPointer<Operations> operations,
                QWeakPointer<TreeItem> parent, Model& model, uint dbIndex);

 public:
  NamespaceItem(const QByteArray& fullPath,
                QSharedPointer<Operations> operations,
                QWeakPointer<TreeItem> parent, Model& model, uint dbIndex,
                QRegExp filter);

  QString getDisplayName() const override;

  QByteArray getName() const override;

  QByteArray getFullPath() const override;

  QString type() const override { return "namespace"; }

  bool isEnabled() const override;

  void setRemoved();

  QVariantMap metadata() const override;

  /*
   * "Load all" button: reloads the namespace dropping every loading limit.
   */
  void loadAllKeys() override;

 protected:
  /*
   * No server-side total is known for a namespace prefix - the scan progress
   * is reported as an amount of scanned keys.
   */
  long loadingTotalKeys() const override { return 0; }

  void load();

  void reload();

  QHash<QString, std::function<bool()>> eventHandlers() override;

 private:
  void startLoadWatchdog(int seq);

  bool isKeysListTruncated(int keysCount) const;

  QString buildFilter() const;

  void loadNextPage() override;

 private:
  QByteArray m_fullPath;
  bool m_removed;
  int m_loadSeq = 0;
  int m_loadFinishedSeq = -1;
  bool m_keysTruncated = false;
};
}  // namespace ConnectionsTree
