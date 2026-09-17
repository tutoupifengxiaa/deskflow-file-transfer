// Modified by tutoupifengxiaa, 2026-09-15 to 2026-09-17: unofficial file-transfer fork; see README.md.
/* SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception */
#include "FileDrag.h"
#include "FileTransfer.h"
#include "base/IEventQueue.h"
#include "deskflow/ipc/CoreIpcServer.h"
#include <QJsonArray>
#include <QJsonDocument>
#include <QMetaObject>
#include <QUuid>

namespace {
IEventQueue *queue = nullptr;
int eventTarget;
QString receivingId;
std::shared_ptr<bool> cancelled;
} // namespace
namespace FileDrag {
void initialize(IEventQueue *events)
{
  queue = events;
}
void *target()
{
  return &eventTarget;
}
QString encode(const QJsonObject &command)
{
  return QString::fromLatin1(QJsonDocument(command)
                                 .toJson(QJsonDocument::Compact)
                                 .toBase64(QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals));
}
QJsonObject decode(const QString &message)
{
  if (message.size() > 180000)
    return {};
  const auto decoded = QByteArray::fromBase64Encoding(
      message.toLatin1(), QByteArray::Base64UrlEncoding | QByteArray::AbortOnBase64DecodingErrors
  );
  return decoded ? QJsonDocument::fromJson(decoded.decoded).object() : QJsonObject{};
}
void enqueue(const QJsonObject &command)
{
  if (queue && !command.isEmpty())
    queue->addEvent(Event(EventTypes::FileDragCommand, target(), new Command(command)));
}
void gui(const QJsonObject &command)
{
  auto &server = deskflow::core::ipc::CoreIpcServer::instance();
  const auto message = encode(command);
  QMetaObject::invokeMethod(&server, [&server, message] { server.sendFileDrag(message); }, Qt::QueuedConnection);
}
std::string createOffer(const QJsonObject &command)
{
  const auto list = command.value("paths").toArray();
  if (list.isEmpty() || list.size() > 256)
    return {};
  QStringList paths;
  for (const auto &path : list) {
    if (!path.isString() || path.toString().size() > 32767)
      return {};
    paths.push_back(path.toString());
  }
  auto offer = FileTransfer::offer(paths);
  return offer.size() < 120000 ? offer : std::string{};
}
void receive(const QJsonObject &command, FileTransfer *files)
{
  const auto action = command.value("action").toString();
  const auto id = command.value("id").toString();
  if (action == "begin") {
    if (QUuid(id).isNull() || !files)
      return;
    const auto offer = QJsonDocument(command.value("offer").toObject()).toJson(QJsonDocument::Compact).toStdString();
    if (!files->remember(offer))
      return;
    if (cancelled)
      *cancelled = true;
    receivingId = id;
    cancelled = std::make_shared<bool>(false);
    const auto flag = cancelled;
    gui(command);
    if (!FileTransfer::fetch(
            offer,
            [id, flag](const QStringList &paths) {
              if (*flag)
                return;
              if (!paths.isEmpty())
                FileTransfer::reportProgress(QStringLiteral("dragReady"));
              gui(
                  {{"action", paths.isEmpty() ? "abort" : "ready"},
                   {"id", id},
                   {"paths", QJsonArray::fromStringList(paths)}}
              );
            },
            [flag] { return *flag; }
        )) {
      gui({{"action", "abort"}, {"id", id}});
    }
  } else {
    if (action == "abort" && id == receivingId && cancelled)
      *cancelled = true;
    gui(command);
  }
}
} // namespace FileDrag
