// Modified by tutoupifengxiaa, 2026-09-15 to 2026-09-17: unofficial file-transfer fork; see README.md.
/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */
#include "deskflow/FileTransfer.h"
#include "base/Log.h"
#include "deskflow/IClipboard.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSet>
#include <QStorageInfo>
#include <QTemporaryDir>
#include <QUuid>
#include <chrono>
#include <deque>
#include <limits>
#include <map>

#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "platform/MSWindowsHandle.h"
#include <Aclapi.h>
#include <ShlObj.h>
#include <Wtsapi32.h>
#endif

namespace {
FileTransfer::Progress progressHandler;
#ifdef Q_OS_WIN
QByteArray tokenUser(HANDLE token)
{
  DWORD size = 0;
  GetTokenInformation(token, TokenUser, nullptr, 0, &size);
  if (!size)
    return {};
  std::vector<BYTE> buffer(size);
  if (!GetTokenInformation(token, TokenUser, buffer.data(), size, &size))
    return {};
  const auto sid = reinterpret_cast<TOKEN_USER *>(buffer.data())->User.Sid;
  return QByteArray(static_cast<const char *>(sid), GetLengthSid(sid));
}

QByteArray sessionUser()
{
  DWORD session = 0;
  HANDLE handle = nullptr;
  if (!ProcessIdToSessionId(GetCurrentProcessId(), &session) || !session || !WTSQueryUserToken(session, &handle))
    return {};
  MSWindowsHandle token(handle);
  return tokenUser(token.get());
}

// Source paths come from the desktop user. A SYSTEM core must not use its
// broader file permissions to open a path supplied through the clipboard.
class SourceAccess
{
public:
  SourceAccess()
  {
    HANDLE raw = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &raw))
      return;
    MSWindowsHandle processToken(raw);
    const auto sid = tokenUser(processToken.get());
    if (sid.isEmpty())
      return;
    if (!IsWellKnownSid(const_cast<char *>(sid.data()), WinLocalSystemSid)) {
      okay = true;
      return;
    }
    DWORD session = 0;
    raw = nullptr;
    if (!ProcessIdToSessionId(GetCurrentProcessId(), &session) || !session || !WTSQueryUserToken(session, &raw))
      return;
    MSWindowsHandle userToken(raw);
    switched = ImpersonateLoggedOnUser(userToken.get());
    okay = switched;
  }
  ~SourceAccess()
  {
    if (switched && !RevertToSelf())
      LOG_ERR("file clipboard: cannot restore source reader context");
  }
  bool okay = false;

private:
  bool switched = false;
};

// SYSTEM's default temp path is SystemTemp, which Explorer cannot read. Use a
// private random directory in ProgramData; grant only the receiving session's
// user read access after the entire batch has been verified.
std::unique_ptr<QTemporaryDir> downloadDirectory(QByteArray &reader)
{
  HANDLE handle = nullptr;
  if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &handle))
    return {};
  MSWindowsHandle token(handle);
  auto owner = tokenUser(token.get());
  if (owner.isEmpty())
    return {};
  if (!IsWellKnownSid(owner.data(), WinLocalSystemSid))
    return std::make_unique<QTemporaryDir>();
  reader = sessionUser();
  if (reader.isEmpty()) {
    LOG_WARN("file clipboard: cannot identify the receiving desktop user (%lu)", GetLastError());
    return {};
  }
  PWSTR rawPath = nullptr;
  if (FAILED(SHGetKnownFolderPath(FOLDERID_ProgramData, 0, nullptr, &rawPath)))
    return {};
  const auto base = QString::fromWCharArray(rawPath);
  CoTaskMemFree(rawPath);
  return std::make_unique<QTemporaryDir>(QDir(base).filePath("Deskflow-received-XXXXXX"));
}

bool allowReader(const QString &path, QByteArray &reader, bool directory)
{
  BYTE system[SECURITY_MAX_SID_SIZE];
  DWORD size = sizeof(system);
  if (!CreateWellKnownSid(WinLocalSystemSid, nullptr, system, &size))
    return false;
  EXPLICIT_ACCESSW access[2]{};
  access[0].grfAccessPermissions = FILE_ALL_ACCESS;
  access[1].grfAccessPermissions = FILE_GENERIC_READ | (directory ? FILE_GENERIC_EXECUTE : 0);
  for (auto &entry : access) {
    entry.grfAccessMode = SET_ACCESS;
    entry.Trustee.TrusteeForm = TRUSTEE_IS_SID;
    entry.Trustee.TrusteeType = TRUSTEE_IS_USER;
  }
  access[0].Trustee.ptstrName = reinterpret_cast<LPWSTR>(system);
  access[1].Trustee.ptstrName = reinterpret_cast<LPWSTR>(reader.data());
  PACL acl = nullptr;
  if (SetEntriesInAclW(2, access, nullptr, &acl) != ERROR_SUCCESS)
    return false;
  auto nativePath = QDir::toNativeSeparators(path).toStdWString();
  const auto result = SetNamedSecurityInfoW(
      nativePath.data(), SE_FILE_OBJECT, DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION, nullptr,
      nullptr, acl, nullptr
  );
  LocalFree(acl);
  return result == ERROR_SUCCESS;
}
#endif

constexpr qint64 chunkSize = 64 * 1024;
constexpr size_t maxMetadata = 1024 * 1024;
struct Entry
{
  QString name;
  qint64 size;
  bool operator==(const Entry &) const = default;
};
struct LocalOffer
{
  QStringList paths;
  std::vector<Entry> entries;
  QList<QDateTime> modified;
  std::string encoded;
};
std::map<QString, LocalOffer> localOffers;
std::deque<QString> offerOrder;
std::map<QString, FileTransfer *> routes;
// Clipboard URLs must stay readable while Finder/Explorer copies them. Their
// temporary directories are retained for this core session and removed on exit.
std::vector<std::unique_ptr<QTemporaryDir>> completedDirectories;

std::string encode(const QJsonObject &object)
{
  return QJsonDocument(object).toJson(QJsonDocument::Compact).toStdString();
}

bool number(const QJsonValue &value, qint64 &out)
{
  bool ok = false;
  out = value.toString().toLongLong(&ok);
  return ok && out >= 0 && QString::number(out) == value.toString();
}

bool parseOffer(const std::string &data, QString &id, std::vector<Entry> &entries)
{
  if (data.empty() || data.size() > maxMetadata)
    return false;
  const auto object = QJsonDocument::fromJson(QByteArray::fromStdString(data)).object();
  id = object.value("id").toString();
  const auto files = object.value("files").toArray();
  if (object.value("version").toInt() != 1 || QUuid(id).isNull() || files.isEmpty() || files.size() > 10000)
    return false;
  QSet<QString> names;
  static const QRegularExpression reserved(
      "^(CON|PRN|AUX|NUL|COM[1-9]|LPT[1-9])(?:\\.|$)", QRegularExpression::CaseInsensitiveOption
  );
  static const QRegularExpression forbidden("[\\x00-\\x1f<>:\"/\\\\|?*]");
  for (const auto &file : files) {
    const auto item = file.toObject();
    Entry entry{item.value("name").toString(), 0};
    const auto normalized = entry.name.normalized(QString::NormalizationForm_C).toCaseFolded();
    if (entry.name.isEmpty() || entry.name == "." || entry.name == ".." || entry.name.endsWith('.') ||
        entry.name.endsWith(' ') || entry.name.size() > 255 || forbidden.match(entry.name).hasMatch() ||
        reserved.match(entry.name).hasMatch() || names.contains(normalized) || !number(item.value("size"), entry.size))
      return false;
    names.insert(normalized);
    entries.push_back(entry);
  }
  return true;
}
} // namespace

struct FileTransfer::Impl
{
  std::function<void(const QJsonObject &)> dragHandler;
  struct Download
  {
    QString id;
    QString token = QUuid::createUuid().toString(QUuid::WithoutBraces);
    std::vector<Entry> entries;
    size_t index = 0;
    qint64 offset = 0;
    qint64 requested = 0;
    int pending = 0;
    qint64 received = 0;
    qint64 total = 0;
    std::chrono::steady_clock::time_point progressAt;
#ifdef Q_OS_WIN
    QByteArray reader;
    std::unique_ptr<QTemporaryDir> directory = downloadDirectory(reader);
#else
    std::unique_ptr<QTemporaryDir> directory = std::make_unique<QTemporaryDir>();
#endif
    std::unique_ptr<QSaveFile> file;
    QCryptographicHash hash{QCryptographicHash::Sha256};
    QStringList paths;
    Complete complete;
    std::function<bool()> cancelled;
    std::chrono::steady_clock::time_point deadline;
  };
  struct Upload
  {
    QString token;
    QString id;
    size_t index = 0;
    qint64 offset = 0;
    std::unique_ptr<QFile> file;
    QCryptographicHash hash{QCryptographicHash::Sha256};
  };
  Send send;
  bool enabled = true;
  bool sizeEnabled = true;
  std::unique_ptr<Download> download;
  std::unique_ptr<Upload> upload;
  std::map<QString, QStringList> cached;
  std::map<QString, std::string> metadata;

  void cancel(bool notify = false, const QString &reason = QStringLiteral("failed"))
  {
    if (!download)
      return;
    if (notify)
      send(encode({{"op", "cancel"}, {"token", download->token}}));
    FileTransfer::reportProgress(reason, download->received, download->total);
    auto complete = std::move(download->complete);
    download.reset();
    if (complete)
      complete({});
  }

  bool request()
  {
    auto &d = *download;
    d.deadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
    if (!d.file) {
      d.requested = 0;
      d.pending = 0;
      const auto path = d.directory->filePath(d.entries[d.index].name);
      d.file = std::make_unique<QSaveFile>(path);
      d.file->setDirectWriteFallback(false);
      if (!d.file->open(QIODevice::WriteOnly) ||
          !d.file->setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner)) {
        LOG_WARN("file clipboard: could not create temporary file");
        cancel();
        return false;
      }
    }
    // Keep a bounded window in flight instead of waiting for a round trip
    // after every 64 KiB. Each block remains independently interleaved with input.
    while (d.pending < 8 && (d.requested < d.entries[d.index].size || (d.entries[d.index].size == 0 && d.pending == 0))
    ) {
      const auto offset = d.requested;
      d.requested += std::min(chunkSize, d.entries[d.index].size - d.requested);
      ++d.pending;
      send(encode(
          {{"op", "get"},
           {"id", d.id},
           {"token", d.token},
           {"index", static_cast<int>(d.index)},
           {"offset", QString::number(offset)}}
      ));
    }
    return true;
  }

  bool get(const QJsonObject &object)
  {
    const auto id = object.value("id").toString();
    const auto token = object.value("token").toString();
    const int index = object.value("index").toInt(-1);
    qint64 offset;
    const auto found = localOffers.find(id);
    if (QUuid(token).isNull() || !number(object.value("offset"), offset))
      return false;
    auto fail = [&]() {
      upload.reset();
      send(encode({{"op", "error"}, {"token", token}}));
      return true;
    };
    if (!enabled || !sizeEnabled)
      return fail();
    if (found == localOffers.end() || index < 0 || static_cast<size_t>(index) >= found->second.entries.size())
      return fail();
    const auto &entry = found->second.entries[index];
    if (offset > entry.size)
      return fail();
    const QFileInfo current(found->second.paths[index]);
    if (current.isSymLink() || current.size() != entry.size || current.lastModified() != found->second.modified[index])
      return fail();
    if (offset == 0) {
#ifdef Q_OS_WIN
      SourceAccess access;
      if (!access.okay)
        return fail();
#endif
      const QFileInfo info(found->second.paths[index]);
      if (!info.isFile() || info.isSymLink() || info.size() != entry.size)
        return fail();
      upload = std::make_unique<Upload>();
      upload->token = token;
      upload->id = id;
      upload->index = index;
      upload->file = std::make_unique<QFile>(info.absoluteFilePath());
      if (!upload->file->open(QIODevice::ReadOnly) || upload->file->size() != entry.size)
        return fail();
    }
    if (!upload || upload->token != token || upload->id != id || upload->index != static_cast<size_t>(index) ||
        upload->offset != offset || upload->file->size() != entry.size)
      return fail();
    const auto wanted = std::min(chunkSize, entry.size - offset);
    const auto bytes = upload->file->read(wanted);
    if (bytes.size() != wanted)
      return fail();
    upload->hash.addData(bytes);
    upload->offset += bytes.size();
    const bool done = upload->offset == entry.size;
    QJsonObject reply{
        {"op", "data"},
        {"token", token},
        {"index", index},
        {"offset", QString::number(offset)},
        {"bytes", QString::fromLatin1(bytes.toBase64())}
    };
    if (done)
      reply.insert("sha256", QString::fromLatin1(upload->hash.result().toHex()));
    send(encode(reply));
    if (done)
      upload.reset();
    return true;
  }

  bool data(const QJsonObject &object)
  {
    // Late responses to a cancelled/changed clipboard must not publish files.
    if (!download || object.value("token").toString() != download->token)
      return true;
    if (download->cancelled && download->cancelled()) {
      cancel(true, QStringLiteral("cancelled"));
      return true;
    }
    auto &d = *download;
    qint64 offset;
    if (object.value("index").toInt(-1) != static_cast<int>(d.index) || !number(object.value("offset"), offset) ||
        offset != d.offset)
      return false;
    const auto decoded = QByteArray::fromBase64Encoding(
        object.value("bytes").toString().toLatin1(), QByteArray::AbortOnBase64DecodingErrors
    );
    const auto wanted = std::min(chunkSize, d.entries[d.index].size - d.offset);
    if (!decoded || decoded.decoded.size() != wanted || d.file->write(decoded.decoded) != wanted) {
      LOG_WARN("file clipboard: invalid data or disk write failure");
      cancel();
      return false;
    }
    d.hash.addData(decoded.decoded);
    d.offset += wanted;
    --d.pending;
    d.received += wanted;
    const auto now = std::chrono::steady_clock::now();
    if (now - d.progressAt >= std::chrono::milliseconds(500)) {
      d.progressAt = now;
      FileTransfer::reportProgress(QStringLiteral("receiving"), d.received, d.total);
    }
    if (d.offset == d.entries[d.index].size) {
      if (object.value("sha256").toString().toLatin1() != d.hash.result().toHex() || !d.file->commit()) {
        LOG_WARN("file clipboard: checksum or commit failed");
        cancel();
        return false;
      }
      d.paths.push_back(d.directory->filePath(d.entries[d.index].name));
      d.file.reset();
      d.hash.reset();
      d.offset = 0;
      ++d.index;
      if (d.index == d.entries.size()) {
#ifdef Q_OS_WIN
        if (!d.reader.isEmpty()) {
          bool readable = sessionUser() == d.reader;
          for (const auto &path : d.paths)
            readable = readable && allowReader(path, d.reader, false);
          readable = readable && allowReader(d.directory->path(), d.reader, true);
          if (!readable) {
            LOG_WARN("file clipboard: could not grant the receiving user read access");
            cancel();
            return false;
          }
        }
#endif
        auto paths = d.paths;
        cached[d.id] = paths;
        completedDirectories.push_back(std::move(d.directory));
        auto complete = std::move(d.complete);
        download.reset();
        LOG_INFO("file clipboard: received %d files", static_cast<int>(paths.size()));
        complete(paths);
        return true;
      }
    }
    return request();
  }
};

FileTransfer::FileTransfer(Send send) : m_impl(std::make_unique<Impl>())
{
  m_impl->send = std::move(send);
}

void FileTransfer::setProgressHandler(Progress progress)
{
  progressHandler = std::move(progress);
}

void FileTransfer::reportProgress(const QString &state, qint64 received, qint64 total)
{
  if (progressHandler)
    progressHandler(state, received, total);
}

FileTransfer::~FileTransfer()
{
  for (auto it = routes.begin(); it != routes.end();) {
    if (it->second == this)
      it = routes.erase(it);
    else
      ++it;
  }
  m_impl->cancel();
}

std::optional<std::string> FileTransfer::clipboardOffer(const IClipboard &clipboard)
{
  // Clipboard::unmarshall and copy close their result. Format reads require
  // opening it again, including when the payload is already in memory.
  if (!clipboard.open(clipboard.getTime()))
    return std::nullopt;
  std::optional<std::string> offer;
  if (clipboard.has(IClipboard::Format::Files))
    offer = clipboard.get(IClipboard::Format::Files);
  clipboard.close();
  return offer;
}

std::string FileTransfer::offer(const QStringList &paths)
{
  if (paths.isEmpty())
    return {};
#ifdef Q_OS_WIN
  SourceAccess access;
  if (!access.okay)
    return {};
#endif
  LocalOffer local;
  QJsonArray files;
  for (const auto &path : paths) {
    QFileInfo info(path);
    if (!info.isFile() || info.isSymLink() || !info.isReadable()) {
      LOG_WARN("file clipboard: only readable regular files are supported");
      return {};
    }
    QFile readable(path);
    if (!readable.open(QIODevice::ReadOnly))
      return {};
    local.paths.push_back(info.canonicalFilePath());
    local.modified.push_back(info.lastModified());
    files.push_back(QJsonObject{{"name", info.fileName()}, {"size", QString::number(info.size())}});
  }
  const auto id = QUuid::createUuid().toString(QUuid::WithoutBraces);
  auto encoded = encode({{"version", 1}, {"id", id}, {"files", files}});
  QString checkedId;
  if (!parseOffer(encoded, checkedId, local.entries)) {
    LOG_WARN("file clipboard: file names or metadata are not portable");
    return {};
  }
  // Preserve identity across the two logical clipboards and repeated screen
  // switches. Changed files get a new offer, even when paths are unchanged.
  for (const auto &[existingId, existing] : localOffers) {
    if (existing.paths == local.paths && existing.modified == local.modified && existing.entries == local.entries)
      return existing.encoded;
  }
  local.encoded = encoded;
  localOffers.emplace(id, std::move(local));
  offerOrder.push_back(id);
  while (offerOrder.size() > 32) {
    localOffers.erase(offerOrder.front());
    offerOrder.pop_front();
  }
  return encoded;
}

bool FileTransfer::remember(const std::string &offer)
{
  QString id;
  std::vector<Entry> entries;
  if (!parseOffer(offer, id, entries))
    return false;
  // Never let a peer rebind an existing local or other-peer offer identifier.
  if (localOffers.contains(id))
    return false;
  auto it = routes.find(id);
  if (it != routes.end() && it->second != this)
    return false;
  const auto previous = m_impl->metadata.find(id);
  if (previous != m_impl->metadata.end() && previous->second != offer)
    return false;
  if (previous == m_impl->metadata.end() && m_impl->metadata.size() >= 32) {
    auto oldest = m_impl->metadata.begin();
    if (m_impl->download && oldest->first == m_impl->download->id)
      ++oldest;
    routes.erase(oldest->first);
    m_impl->cached.erase(oldest->first);
    m_impl->metadata.erase(oldest);
  }
  m_impl->metadata[id] = offer;
  routes[id] = this;
  return true;
}

bool FileTransfer::fetch(const std::string &offer, Complete complete, std::function<bool()> cancelled)
{
  QString id;
  std::vector<Entry> entries;
  if (!parseOffer(offer, id, entries))
    return false;
  if (const auto local = localOffers.find(id); local != localOffers.end()) {
    complete(local->second.paths);
    return true;
  }
  const auto route = routes.find(id);
  if (route == routes.end())
    return false;
  auto &impl = *route->second->m_impl;
  if (!impl.enabled || !impl.sizeEnabled)
    return false;
  if (impl.metadata.at(id) != offer)
    return false;
  if (const auto cached = impl.cached.find(id); cached != impl.cached.end()) {
    complete(cached->second);
    return true;
  }
  impl.cancel(true, QStringLiteral("cancelled"));
  auto download = std::make_unique<Impl::Download>();
  if (!download->directory || !download->directory->isValid())
    return false;
  const auto free = QStorageInfo(download->directory->path()).bytesAvailable();
  qint64 remaining = free;
  for (const auto &entry : entries) {
    if (remaining >= 0 && entry.size > remaining) {
      LOG_WARN("file clipboard: insufficient temporary disk space");
      return false;
    }
    if (remaining >= 0)
      remaining -= entry.size;
    if (entry.size > (std::numeric_limits<qint64>::max)() - download->total)
      return false;
    download->total += entry.size;
  }
  download->id = id;
  download->entries = std::move(entries);
  download->complete = std::move(complete);
  download->cancelled = std::move(cancelled);
  impl.download = std::move(download);
  impl.download->progressAt = std::chrono::steady_clock::now();
  reportProgress(QStringLiteral("receiving"), 0, impl.download->total);
  LOG_INFO("file clipboard: receiving %d files", static_cast<int>(impl.download->entries.size()));
  return impl.request();
}

void FileTransfer::setDragHandler(std::function<void(const QJsonObject &)> handler)
{
  m_impl->dragHandler = std::move(handler);
}

void FileTransfer::sendDrag(const QJsonObject &command)
{
  auto message = encode({{"op", "drag"}, {"command", command}});
  if (message.size() <= 2 * chunkSize)
    m_impl->send(message);
}

bool FileTransfer::receive(const std::string &message)
{
  if (message.size() > 2 * chunkSize)
    return false;
  const auto object = QJsonDocument::fromJson(QByteArray::fromStdString(message)).object();
  const auto op = object.value("op").toString();
  if (op == "drag") {
    if (m_impl->dragHandler)
      m_impl->dragHandler(object.value("command").toObject());
    return true;
  }
  if (op == "get")
    return m_impl->get(object);
  if (op == "data")
    return m_impl->data(object);
  if (op == "cancel") {
    if (m_impl->upload && object.value("token").toString() == m_impl->upload->token)
      m_impl->upload.reset();
    return true;
  }
  if (op == "error") {
    if (m_impl->download && object.value("token").toString() == m_impl->download->token) {
      LOG_WARN("file clipboard: source unavailable or changed");
      m_impl->cancel();
    }
    return true;
  }
  return false;
}

void FileTransfer::poll()
{
  if (m_impl->download) {
    const bool changed = m_impl->download->cancelled && m_impl->download->cancelled();
    const bool timeout = std::chrono::steady_clock::now() > m_impl->download->deadline;
    if (changed || timeout) {
      LOG_INFO(
          "file clipboard: %s",
          changed ? "transfer cancelled by its requester" : "no data for 60 seconds; transfer timed out"
      );
      m_impl->cancel(true, changed ? QStringLiteral("cancelled") : QStringLiteral("timeout"));
    }
  }
}

void FileTransfer::setOptions(const OptionsList &options)
{
  for (size_t i = 0; i + 1 < options.size(); i += 2) {
    if (options[i] == kOptionClipboardSharing)
      m_impl->enabled = options[i + 1] != 0;
    if (options[i] == kOptionClipboardSharingSize)
      m_impl->sizeEnabled = options[i + 1] != 0;
  }
  if (!m_impl->enabled || !m_impl->sizeEnabled) {
    m_impl->cancel(true);
    m_impl->upload.reset();
  }
}
