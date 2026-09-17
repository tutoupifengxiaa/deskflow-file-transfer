// Modified by tutoupifengxiaa, 2026-09-15 to 2026-09-17: unofficial file-transfer fork; see README.md.
/* SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception */
#pragma once
#include "base/Event.h"
#include <QJsonObject>
#include <QStringList>

class IEventQueue;
class FileTransfer;
namespace FileDrag {
struct Command : EventData
{
  explicit Command(QJsonObject value) : value(std::move(value))
  {
  }
  QJsonObject value;
};
void initialize(IEventQueue *events);
void *target();
void enqueue(const QJsonObject &command);
void gui(const QJsonObject &command);
void receive(const QJsonObject &command, FileTransfer *files = nullptr);
std::string createOffer(const QJsonObject &command);
QString encode(const QJsonObject &command);
QJsonObject decode(const QString &message);
} // namespace FileDrag
