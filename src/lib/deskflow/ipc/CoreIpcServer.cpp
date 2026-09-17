// Modified by tutoupifengxiaa, 2026-09-15 to 2026-09-17: unofficial file-transfer fork; see docs/SECONDARY_DEVELOPMENT.md.
/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2025-2026 Synergy App Ltd
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "CoreIpcServer.h"

#include "base/Log.h"
#include "deskflow/FileDrag.h"
#include "common/Constants.h"

#include <QLocalSocket>
#ifdef Q_OS_WIN
#include <Windows.h>
#include <Wtsapi32.h>
#include <vector>
#else
#include <unistd.h>
#include <sys/socket.h>
#endif

namespace {
bool sameDesktopUser(QLocalSocket *socket)
{
#ifdef Q_OS_WIN
  ULONG pid = 0;
  if (!GetNamedPipeClientProcessId(reinterpret_cast<HANDLE>(socket->socketDescriptor()), &pid))
    return false;
  DWORD peerSession = 0, session = 0;
  if (!ProcessIdToSessionId(pid, &peerSession) ||
      !ProcessIdToSessionId(GetCurrentProcessId(), &session) || !session || session != peerSession)
    return false;
  HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
  HANDLE peer = nullptr, user = nullptr;
  if (process) {
    OpenProcessToken(process, TOKEN_QUERY, &peer);
    CloseHandle(process);
  }
  if (!WTSQueryUserToken(session, &user))
    OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &user);
  auto sid = [](HANDLE token) {
    DWORD size = 0;
    GetTokenInformation(token, TokenUser, nullptr, 0, &size);
    std::vector<BYTE> buffer(size);
    if (!size || !GetTokenInformation(token, TokenUser, buffer.data(), size, &size))
      return QByteArray{};
    const auto value = reinterpret_cast<TOKEN_USER *>(buffer.data())->User.Sid;
    return QByteArray(static_cast<const char *>(value), GetLengthSid(value));
  };
  const auto a = peer ? sid(peer) : QByteArray{};
  const auto b = user ? sid(user) : QByteArray{};
  if (peer) CloseHandle(peer);
  if (user) CloseHandle(user);
  return !a.isEmpty() && a == b;
#else
  uid_t user = 0;
  gid_t group = 0;
  return getpeereid(static_cast<int>(socket->socketDescriptor()), &user, &group) == 0 && user == geteuid();
#endif
}
}


namespace deskflow::core::ipc {

static CoreIpcServer *s_instance = nullptr;

CoreIpcServer::CoreIpcServer(QObject *parent) : IpcServer(parent, kCoreIpcName, QStringLiteral("core"))
{
  assert(s_instance == nullptr);
  s_instance = this;
}

CoreIpcServer &CoreIpcServer::instance()
{
  assert(s_instance != nullptr);
  return *s_instance;
}

void CoreIpcServer::sendFileDrag(const QString &message)
{
  const auto action = FileDrag::decode(message).value("action").toString();
  if (action == "begin" || action == "ready" || action == "release" || action == "abort")
    LOG_INFO("file drag: sending GUI %s, connected=%d", qPrintable(action), !m_dragClient.isNull());
  if (auto *socket = m_dragClient.data()) {
    writeToClientSocket(socket, QStringLiteral("fileDrag=") + message);
    socket->flush();
  }
}

void CoreIpcServer::processCommand(QLocalSocket *clientSocket, const QString &command, const QStringList &parts)
{
  if (command == QStringLiteral("fileDrag")) {
    if (parts.size() != 2 || !sameDesktopUser(clientSocket)) {
      LOG_WARN("file drag: local GUI identity check failed");
      return;
    }
    const auto value = FileDrag::decode(parts[1]);
    if (value.value("action").toString() == "hello") {
      if (m_dragClient != clientSocket) LOG_INFO("file drag: local GUI registered");
      m_dragClient = clientSocket;
    }
    if (m_dragClient == clientSocket)
      FileDrag::enqueue(value);
    return;
  }
  if (command == QStringLiteral("stop")) {
    LOG_DEBUG("core ipc server got stop message");
    writeToClientSocket(clientSocket, QStringLiteral("ok"));
    broadcastCommand(QStringLiteral("bye"));
    Q_EMIT stopProcessRequested();
    return;
  }
  LOG_WARN("core ipc server got unknown command: %s", command.toUtf8().constData());
}

} // namespace deskflow::core::ipc
