// Modified by tutoupifengxiaa, 2026-09-15 to 2026-09-17: unofficial file-transfer fork; see docs/SECONDARY_DEVELOPMENT.md.
/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */
#pragma once

#include "deskflow/OptionTypes.h"

#include <QStringList>
#include <QJsonObject>
#include <functional>
#include <memory>
#include <optional>
#include <string>

class IClipboard;

inline constexpr char kMsgDFileBlock[] = "DFT2%s";

// A file offer is clipboard metadata. File contents travel in bounded request/
// response messages on the same connection, without filling the input queue.
class FileTransfer
{
public:
  using Send = std::function<void(const std::string &)>;
  using Complete = std::function<void(const QStringList &)>;
  using Progress = std::function<void(const QString &, qint64, qint64)>;
  static void setProgressHandler(Progress progress);
  static void reportProgress(const QString &state, qint64 received = 0, qint64 total = 0);
  explicit FileTransfer(Send send);
  ~FileTransfer();
  FileTransfer(const FileTransfer &) = delete;
  FileTransfer &operator=(const FileTransfer &) = delete;

  static std::optional<std::string> clipboardOffer(const IClipboard &clipboard);
  static std::string offer(const QStringList &paths);
  bool remember(const std::string &offer);
  static bool fetch(const std::string &offer, Complete complete, std::function<bool()> cancelled = {});
  bool receive(const std::string &message);
  void setDragHandler(std::function<void(const QJsonObject &)> handler);
  void sendDrag(const QJsonObject &command);
  void poll();
  void setOptions(const OptionsList &options);

private:
  struct Impl;
  std::unique_ptr<Impl> m_impl;
};
