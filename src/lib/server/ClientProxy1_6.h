// Modified by tutoupifengxiaa, 2026-09-15 to 2026-09-17: unofficial file-transfer fork; see README.md.
/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2015 - 2016 Synergy App Ltd
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#pragma once

#include "deskflow/ClipboardChunk.h"
#include "deskflow/FileTransfer.h"
#include "server/ClientProxy1_5.h"

#include <string>

class Server;
class IEventQueue;

//! Proxy for client implementing protocol version 1.6
class ClientProxy1_6 : public ClientProxy1_5
{
public:
  FileTransfer &fileTransfer() { return *m_files; }
  ClientProxy1_6(const std::string &name, deskflow::IStream *adoptedStream, Server *server, IEventQueue *events);
  ~ClientProxy1_6() override;

  void setClipboard(ClipboardID id, const IClipboard *clipboard) override;
  bool recvClipboard() override;
  void setOptions(const OptionsList &options) override;

private:
  bool parseMessage(const uint8_t *code) override;
  IEventQueue *m_events;
  std::unique_ptr<FileTransfer> m_files;
  std::string m_clipboardDataCached;
  ClipboardChunkAssemblyState m_clipboardChunkState;
};
