// Modified by tutoupifengxiaa, 2026-09-15 to 2026-09-17: unofficial file-transfer fork; see docs/SECONDARY_DEVELOPMENT.md.
/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2025-2026 Synergy App Ltd
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#pragma once

#include "IpcServer.h"

#include <QObject>
#include <QSet>
#include <QPointer>

class QLocalSocket;

namespace deskflow::core::ipc {

class CoreIpcServer : public IpcServer
{
  Q_OBJECT

public:
  explicit CoreIpcServer(QObject *parent);

  static CoreIpcServer &instance();
  void sendFileDrag(const QString &message);

private:
  QPointer<QLocalSocket> m_dragClient;
  void processCommand(QLocalSocket *clientSocket, const QString &command, const QStringList &parts) override;
};

} // namespace deskflow::core::ipc
