// Modified by tutoupifengxiaa, 2026-09-15 to 2026-09-17: unofficial file-transfer fork; see docs/SECONDARY_DEVELOPMENT.md.
/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2015 - 2016 Synergy App Ltd
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "server/ClientProxy1_6.h"

#include "base/Log.h"
#include "deskflow/ClipboardChunk.h"
#include "deskflow/ProtocolUtil.h"
#include "deskflow/StreamChunker.h"
#include "io/IStream.h"
#include "server/Server.h"
#include <cstring>

//
// ClientProxy1_6
//

ClientProxy1_6::ClientProxy1_6(const std::string &name, deskflow::IStream *stream, Server *server, IEventQueue *events)
    : ClientProxy1_5(name, stream, server, events),
      m_events(events)
{
  m_files = std::make_unique<FileTransfer>([this](const std::string &message) {
    ProtocolUtil::writef(getStream(), kMsgDFileBlock, &message);
  });
  m_files->setDragHandler([this](const QJsonObject &command) { m_server->fileDragCommand(this, command); });
  m_events->addHandler(EventTypes::ClipboardSending, this, [this](const auto &e) {
    ClipboardChunk::send(getStream(), e.getDataObject());
  });
}

ClientProxy1_6::~ClientProxy1_6()
{
  m_events->removeHandler(EventTypes::ClipboardSending, this);
}

bool ClientProxy1_6::parseMessage(const uint8_t *code)
{
  m_files->poll();
  if (memcmp(code, kMsgDFileBlock, 4) == 0) {
    std::string message;
    ProtocolUtil::readf(getStream(), kMsgDFileBlock + 4, &message);
    return m_files->receive(message);
  }
  return ClientProxy1_5::parseMessage(code);
}

void ClientProxy1_6::setOptions(const OptionsList &options)
{
  m_files->setOptions(options);
  auto extended = options;
  extended.insert(extended.end(), {kOptionFileDrag, 1});
  ClientProxy1_5::setOptions(extended);
}

void ClientProxy1_6::setClipboard(ClipboardID id, const IClipboard *clipboard)
{
  // ignore if this clipboard is already clean
  if (m_clipboard[id].m_dirty) {
    // this clipboard is now clean
    m_clipboard[id].m_dirty = false;
    Clipboard::copy(&m_clipboard[id].m_clipboard, clipboard);

    std::string data = m_clipboard[id].m_clipboard.marshall();

    size_t size = data.size();
    LOG_DEBUG("sending clipboard %d to \"%s\"", id, getName().c_str());

    StreamChunker::sendClipboard(data, size, id, 0, m_events, this);
  }
}

bool ClientProxy1_6::recvClipboard()
{
  // parse message
  ClipboardID id;
  uint32_t seq;

  auto r = ClipboardChunk::assemble(
      getStream(), m_clipboardDataCached, id, seq, m_clipboardChunkState, m_server->getMaximumClipboardSizeBytes()
  );

  if (r == TransferState::Started) {
    size_t size = ClipboardChunk::getExpectedSize(m_clipboardChunkState);
    LOG_DEBUG("receiving clipboard %d size=%zu", id, size);
  } else if (r == TransferState::Finished) {
    LOG(
        (CLOG_DEBUG "received client \"%s\" clipboard %d seqnum=%d, size=%zu", getName().c_str(), id, seq,
         m_clipboardDataCached.size())
    );
    // save clipboard
    m_clipboard[id].m_clipboard.unmarshall(m_clipboardDataCached, 0);
    const auto &clipboard = m_clipboard[id].m_clipboard;
    if (const auto offer = FileTransfer::clipboardOffer(clipboard); offer && !m_files->remember(*offer)) {
      return false;
    }
    m_clipboard[id].m_sequenceNumber = seq;
    m_clipboardDataCached.clear();
    m_clipboardDataCached.shrink_to_fit();

    // notify
    auto *info = new ClipboardInfo;
    info->m_id = id;
    info->m_sequenceNumber = seq;
    m_events->addEvent(Event(EventTypes::ClipboardChanged, getEventTarget(), info));
  } else if (r == TransferState::Error) {
    return false;
  }

  return true;
}
