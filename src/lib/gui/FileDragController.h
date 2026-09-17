// Modified by tutoupifengxiaa, 2026-09-15 to 2026-09-17: unofficial file-transfer fork; see docs/SECONDARY_DEVELOPMENT.md.
/* SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception */
#pragma once
#include <QElapsedTimer>
#include <QJsonObject>
#include <QObject>
#include <QStringList>
#include <QTimer>
#include <QUrl>
#include <functional>
#include <memory>
#include <optional>

class QWidget;
namespace deskflow::gui {
class CoreProcess;
}

struct DragFileEntry
{
  QString name;
  qint64 size;
};
class DragFiles : public QObject
{
  Q_OBJECT
public:
  QList<DragFileEntry> entries;
  QStringList paths;
  bool ready = false;
  bool cancelled = false;
  bool released = false;
  void changed()
  {
    Q_EMIT updated();
  }
Q_SIGNALS:
  void updated();
};

bool nativeFileDragButtonDown();
#ifdef Q_OS_MACOS
void configureNativeFileDragEdge(
    QWidget *edge, std::function<void(const QList<QUrl> &)> offer, std::function<void()> ended
);
#else
bool nativeFileDragSourceEnded();
#endif
QStringList nativeFileDragPaths(const QList<QUrl> &urls);
bool cancelNativeFileDragSource();
void startNativeFileDrag(QWidget *owner, const std::shared_ptr<DragFiles> &files, std::function<void(bool)> finished);

class FileDragController : public QObject
{
public:
  FileDragController(deskflow::gui::CoreProcess &core, QWidget *owner);
  ~FileDragController() override;
  void offer(const QList<QUrl> &urls, int edge);
  void sourceEnded();

private:
  void send(QJsonObject command);
  void receive(const QString &message);
  void updateEdges();
  void clear();
  deskflow::gui::CoreProcess &m_core;
  QWidget *m_owner;
  QList<QWidget *> m_edges;
  QTimer m_timer;
  QElapsedTimer m_hello, m_seen;
  int m_edgeMask = 0;
  bool m_edgesVisible = false;
  bool m_waitForSourceRelease = false;
  bool m_sourceEnding = false;
  std::optional<bool> m_primaryLeftDown;
  QString m_id;
  std::shared_ptr<DragFiles> m_files;
};
