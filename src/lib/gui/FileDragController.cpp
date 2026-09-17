// Modified by tutoupifengxiaa, 2026-09-15 to 2026-09-17: unofficial file-transfer fork; see docs/SECONDARY_DEVELOPMENT.md.
/* SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception */
#include "FileDragController.h"
#include "gui/core/CoreProcess.h"
#include <QApplication>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QMimeData>
#include <QPainter>
#include <QPointer>
#include <QScreen>
#include <QUuid>
#include <QWidget>

namespace {
class Edge : public QWidget
{
public:
  Edge(FileDragController *controller, int direction)
      : QWidget(nullptr, Qt::Tool | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint | Qt::WindowDoesNotAcceptFocus),
        controller(controller),
        direction(direction)
  {
    setAttribute(Qt::WA_ShowWithoutActivating);
#ifdef Q_OS_MACOS
    setAttribute(Qt::WA_MacAlwaysShowToolWindow);
#endif
    setAttribute(Qt::WA_TranslucentBackground);
#ifndef Q_OS_MACOS
    setAcceptDrops(true);
#endif
    setWindowTitle(QStringLiteral("Deskflow file drag edge"));
  }
#ifndef Q_OS_MACOS
  void dragEnterEvent(QDragEnterEvent *event) override
  {
    qInfo(
        "file drag: edge entered, urls=%d, formats=%d", event->mimeData()->hasUrls(),
        static_cast<int>(event->mimeData()->formats().size())
    );
    if (event->mimeData()->hasUrls() && event->possibleActions().testFlag(Qt::CopyAction)) {
      event->setDropAction(Qt::CopyAction);
      event->accept();
      controller->offer(event->mimeData()->urls(), direction);
    }
  }
  void dragMoveEvent(QDragMoveEvent *event) override
  {
    event->setDropAction(Qt::CopyAction);
    event->accept();
    controller->offer(event->mimeData()->urls(), direction);
  }
  // Source cancellation normally ends the original drag. Never report a move
  // or accept a local drop: source files must remain untouched.
  void dropEvent(QDropEvent *event) override
  {
    event->ignore();
  }
#endif
  void paintEvent(QPaintEvent *) override
  {
    QPainter(this).fillRect(rect(), QColor(60, 140, 240, 45));
  }
#ifdef Q_OS_MACOS
  void showEvent(QShowEvent *event) override
  {
    QWidget::showEvent(event);
    configureNativeFileDragEdge(
        this, [this](const QList<QUrl> &urls) { controller->offer(urls, direction); },
        [this] { controller->sourceEnded(); }
    );
  }
#endif

private:
  FileDragController *controller;
  int direction;
};
} // namespace
FileDragController::FileDragController(deskflow::gui::CoreProcess &core, QWidget *owner)
    : QObject(owner),
      m_core(core),
      m_owner(owner)
{
  connect(&core, &deskflow::gui::CoreProcess::fileDragMessage, this, &FileDragController::receive);
  connect(&core, &deskflow::gui::CoreProcess::fileTransferProgress, this, [this](const QString &, qint64, qint64) {
    if (m_files)
      m_files->changed();
  });
  connect(&m_timer, &QTimer::timeout, this, &FileDragController::updateEdges);
  m_hello.start();
  m_seen.start();
  m_timer.start(25);
}
FileDragController::~FileDragController()
{
  if (m_files) {
    m_files->cancelled = true;
    m_files->changed();
  }
  qDeleteAll(m_edges);
}
void FileDragController::send(QJsonObject command)
{
  const auto bytes = QJsonDocument(command).toJson(QJsonDocument::Compact);
  if (bytes.size() <= 128 * 1024)
    m_core.sendFileDrag(
        QString::fromLatin1(bytes.toBase64(QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals))
    );
}
void FileDragController::clear()
{
  m_id.clear();
  m_files.reset();
  m_sourceEnding = false;
}
void FileDragController::sourceEnded()
{
  if (!m_sourceEnding || m_id.isEmpty())
    return;
  m_sourceEnding = false;
  qInfo("file drag: native source ended, ready for handoff");
  send({{"action", "sourceReady"}, {"id", m_id}});
}
void FileDragController::offer(const QList<QUrl> &urls, int edge)
{
  if (!m_id.isEmpty() || urls.isEmpty() || urls.size() > 256)
    return;
  const auto paths = nativeFileDragPaths(urls);
  if (paths.size() != urls.size())
    return;
  for (const auto &path : paths) {
    if (!QFileInfo(path).isFile() || QFileInfo(path).isSymLink())
      return;
  }
  m_id = QUuid::createUuid().toString(QUuid::WithoutBraces);
  qInfo("file drag: captured %d files at edge %d", static_cast<int>(paths.size()), edge);
  send({{"action", "offer"}, {"id", m_id}, {"edge", edge}, {"paths", QJsonArray::fromStringList(paths)}});
}
void FileDragController::updateEdges()
{
  if (m_hello.elapsed() >= 3000) {
    m_hello.restart();
    send({{"action", "hello"}});
  }
  if (m_seen.elapsed() > 10000 || !m_core.isStarted()) {
    m_edgeMask = 0;
    if (m_files) {
      m_files->cancelled = true;
      m_files->changed();
    }
  }
  const bool down = m_primaryLeftDown ? *m_primaryLeftDown : nativeFileDragButtonDown();
#ifdef Q_OS_WIN
  if (m_sourceEnding && nativeFileDragSourceEnded())
    sourceEnded();
#endif
  if (!down)
    m_waitForSourceRelease = false;
  const bool sourceOffered = !m_id.isEmpty() && !m_files && !m_waitForSourceRelease;
  const bool visible = m_edgeMask &&
                       (m_sourceEnding || sourceOffered || (m_id.isEmpty() && down && !m_waitForSourceRelease));
  if (visible != m_edgesVisible) {
    qInfo("file drag: edge windows visible=%d", visible);
    m_edgesVisible = visible;
  }
  if (!visible) {
    for (auto *edge : m_edges)
      edge->hide();
    return;
  }
  QRect bounds;
  for (auto *screen : QGuiApplication::screens())
    bounds = bounds.united(screen->geometry());
  if (m_edges.isEmpty()) {
    qInfo("file drag: desktop bounds %d,%d %dx%d", bounds.x(), bounds.y(), bounds.width(), bounds.height());
    for (int direction = 1; direction <= 4; ++direction)
      m_edges.push_back(new Edge(this, direction));
  }
  const int thickness = 12;
  const QList<QRect> shapes = {
      {bounds.left(), bounds.top(), thickness, bounds.height()},
      {bounds.right() - thickness + 1, bounds.top(), thickness, bounds.height()},
      {bounds.left(), bounds.top(), bounds.width(), thickness},
      {bounds.left(), bounds.bottom() - thickness + 1, bounds.width(), thickness}
  };
  for (int i = 0; i < 4; ++i) {
    m_edges[i]->setGeometry(shapes[i]);
    m_edges[i]->setVisible(m_edgeMask & (1 << (i + 1)));
  }
}
void FileDragController::receive(const QString &message)
{
  if (message.size() > 180000)
    return;
  const auto bytes = QByteArray::fromBase64Encoding(
      message.toLatin1(), QByteArray::Base64UrlEncoding | QByteArray::AbortOnBase64DecodingErrors
  );
  if (!bytes)
    return;
  const auto command = QJsonDocument::fromJson(bytes.decoded).object();
  const auto action = command.value("action").toString();
  const auto id = command.value("id").toString();
  if (action == "edges") {
    if (command.contains("primaryLeftDown"))
      m_primaryLeftDown = command.value("primaryLeftDown").toBool();
    else
      m_primaryLeftDown.reset();
    m_seen.restart();
    const auto edges = command.value("edges").toInt() & 30;
    if (edges != m_edgeMask)
      qInfo("file drag: enabled edges %d", edges);
    m_edgeMask = edges;
    return;
  }
  if (action == "button") {
    m_primaryLeftDown = command.value("down").toBool();
    return;
  }
  if (action == "begin") {
    qInfo("file drag: received native begin, busy=%d", !m_id.isEmpty());
    if (!m_id.isEmpty() || QUuid(id).isNull()) {
      send({{"action", "cancel"}, {"id", id}});
      return;
    }
    m_id = id;
    m_files = std::make_shared<DragFiles>();
    for (const auto &item : command.value("offer").toObject().value("files").toArray()) {
      const auto file = item.toObject();
      m_files->entries.push_back({file.value("name").toString(), file.value("size").toString().toLongLong()});
    }
    const auto files = m_files;
    for (auto *edge : m_edges)
      edge->hide();
    QTimer::singleShot(0, this, [this, files, id] {
      if (files->released || files->cancelled || files->entries.isEmpty()) {
        send({{"action", "cancel"}, {"id", id}});
        clear();
        return;
      }
      QPointer<FileDragController> guard(this);
      startNativeFileDrag(m_owner, files, [this, guard, id](bool okay) {
        if (!guard)
          return;
        send({{"action", okay ? "done" : "cancel"}, {"id", id}});
        if (m_id == id)
          clear();
      });
    });
  } else if (id == m_id) {
    if (action == "finishSource") {
      m_waitForSourceRelease = true;
      m_sourceEnding = true;
      if (!cancelNativeFileDragSource()) {
        m_sourceEnding = false;
        send({{"action", "cancel"}, {"id", id}});
      } else {
        QTimer::singleShot(2000, this, [this, id] {
          if (m_id == id && m_sourceEnding) {
            qWarning("file drag: native source did not finish; handoff cancelled");
            m_sourceEnding = false;
            send({{"action", "cancel"}, {"id", id}});
          }
        });
      }
    } else if (action == "sourceDone" || action == "rejected") {
      clear();
    } else if (action == "ready" && m_files) {
      qInfo("file drag: native files ready");
      for (const auto &path : command.value("paths").toArray())
        m_files->paths.push_back(path.toString());
      m_files->ready = m_files->paths.size() == m_files->entries.size();
      if (!m_files->ready)
        m_files->cancelled = true;
      m_files->changed();
    } else if (action == "release" && m_files) {
      qInfo("file drag: native button released");
      m_files->released = true;
      m_files->changed();
    } else if (action == "abort") {
      if (m_files) {
        m_files->cancelled = true;
        m_files->changed();
      } else
        clear();
    }
  }
}
