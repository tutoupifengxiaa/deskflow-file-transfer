// Modified by tutoupifengxiaa, 2026-09-15 to 2026-09-17: unofficial file-transfer fork; see README.md.
/* SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception */
#include "platform/FileClipboard.h"
#include "base/Log.h"
#include "deskflow/FileTransfer.h"

#include <QDir>
#include <QUrl>
#include <optional>

#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "platform/MSWindowsClipboard.h"
#include "platform/MSWindowsHandle.h"
#include <Shellapi.h>
#include <ShlObj.h>
#include <Wtsapi32.h>
#include <atomic>
#include <cstring>
#include <memory>
#include <thread>
#include <vector>
#else
#include "platform/OSXPasteboardPeeker.h"
#include <Carbon/Carbon.h>
#endif

namespace {
std::string currentOffer;
quint64 currentVersion = 0;

quint64 clipboardVersion()
{
#ifdef Q_OS_WIN
  return GetClipboardSequenceNumber();
#else
  return getClipboardChangeCount();
#endif
}

#ifdef Q_OS_WIN
UINT markerFormat()
{
  static const auto format = RegisterClipboardFormatW(L"Deskflow Received Files");
  return format;
}

bool put(UINT format, const QByteArray &bytes)
{
  const auto memory = GlobalAlloc(GMEM_MOVEABLE, bytes.size());
  if (!memory)
    return false;
  auto *data = GlobalLock(memory);
  if (!data) {
    GlobalFree(memory);
    return false;
  }
  std::memcpy(data, bytes.constData(), bytes.size());
  GlobalUnlock(memory);
  if (SetClipboardData(format, memory))
    return true;
  GlobalFree(memory);
  return false;
}

constexpr DWORD maxReaderBytes = 4 * 1024 * 1024;

QStringList readFiles()
{
  if (IsClipboardFormatAvailable(markerFormat()) || !IsClipboardFormatAvailable(CF_HDROP))
    return {};
  MSWindowsClipboard clipboard(nullptr);
  if (!clipboard.open(0))
    return {};
  QStringList paths;
  const auto drop = static_cast<HDROP>(GetClipboardData(CF_HDROP));
  const auto count = drop ? DragQueryFileW(drop, 0xffffffff, nullptr, 0) : 0;
  if (count <= 10000) {
    size_t total = 0;
    for (UINT i = 0; i < count; ++i) {
      const auto length = DragQueryFileW(drop, i, nullptr, 0);
      total += (length + 1) * sizeof(wchar_t);
      if (!length || length > 32767 || total > maxReaderBytes) {
        paths.clear();
        break;
      }
      std::wstring path(length + 1, L'\0');
      if (DragQueryFileW(drop, i, path.data(), length + 1) != length) {
        paths.clear();
        break;
      }
      paths.push_back(QString::fromWCharArray(path.data(), length));
    }
  }
  clipboard.close();
  return paths;
}

struct OleFiles
{
  DWORD version;
  std::atomic<bool> done{false};
  HRESULT result = E_PENDING;
  std::vector<std::wstring> paths;
};
std::shared_ptr<OleFiles> oleRead;
DWORD attemptedVersion = 0;
DWORD oleVersion = 0;
QStringList olePaths;

void readLocalOleFiles(const std::shared_ptr<OleFiles> &read)
{
  // This worker owns its COM apartment. It never accesses Qt, the screen or
  // the input event queue, and may outlive a connection while an RPC finishes.
  struct Apartment
  {
    HRESULT result = OleInitialize(nullptr);
    ~Apartment()
    {
      if (SUCCEEDED(result))
        OleUninitialize();
    }
  };
  struct Medium
  {
    STGMEDIUM value{};
    ~Medium()
    {
      if (value.tymed != TYMED_NULL)
        ReleaseStgMedium(&value);
    }
  };
  {
    Apartment apartment;
    read->result = apartment.result;
    if (SUCCEEDED(apartment.result)) {
      IDataObject *raw = nullptr;
      read->result = OleGetClipboard(&raw);
      const auto release = [](IDataObject *object) {
        if (object)
          object->Release();
      };
      std::unique_ptr<IDataObject, decltype(release)> object(raw, release);
      if (SUCCEEDED(read->result) && object) {
        FORMATETC format{CF_HDROP, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
        Medium medium;
        read->result = object->GetData(&format, &medium.value);
        if (SUCCEEDED(read->result) && medium.value.tymed == TYMED_HGLOBAL && medium.value.hGlobal) {
          const auto drop = static_cast<HDROP>(medium.value.hGlobal);
          const auto count = DragQueryFileW(drop, 0xffffffff, nullptr, 0);
          if (count <= 10000) {
            size_t total = 0;
            for (UINT i = 0; i < count; ++i) {
              const auto length = DragQueryFileW(drop, i, nullptr, 0);
              total += (length + 1) * sizeof(wchar_t);
              if (!length || length > 32767 || total > maxReaderBytes) {
                read->paths.clear();
                break;
              }
              std::wstring path(length + 1, L'\0');
              if (DragQueryFileW(drop, i, path.data(), length + 1) != length) {
                read->paths.clear();
                break;
              }
              path.resize(length);
              read->paths.push_back(std::move(path));
            }
          }
        }
      }
    }
  }
  read->done = true;
}

bool systemAccount()
{
  HANDLE raw = nullptr;
  if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &raw))
    throw std::runtime_error("cannot identify clipboard reader account");
  MSWindowsHandle token(raw);
  DWORD size = 0;
  GetTokenInformation(token.get(), TokenUser, nullptr, 0, &size);
  std::vector<BYTE> bytes(size);
  if (!size || !GetTokenInformation(token.get(), TokenUser, bytes.data(), size, &size))
    throw std::runtime_error("cannot identify clipboard reader account");
  return IsWellKnownSid(reinterpret_cast<TOKEN_USER *>(bytes.data())->User.Sid, WinLocalSystemSid);
}

void readUserFiles(const std::shared_ptr<OleFiles> &read)
{
  DWORD session = 0;
  HANDLE raw = nullptr;
  if (!ProcessIdToSessionId(GetCurrentProcessId(), &session) || !session || !WTSQueryUserToken(session, &raw)) {
    read->result = HRESULT_FROM_WIN32(GetLastError());
    return;
  }
  MSWindowsHandle token(raw);
  SECURITY_ATTRIBUTES security{sizeof(security), nullptr, TRUE};
  HANDLE input = nullptr, output = nullptr;
  if (!CreatePipe(&input, &output, &security, 0)) {
    read->result = HRESULT_FROM_WIN32(GetLastError());
    return;
  }
  MSWindowsHandle pipeInput(input), pipeOutput(output);
  if (!SetHandleInformation(input, HANDLE_FLAG_INHERIT, 0))
    return;
  MSWindowsHandle nullHandle(CreateFileW(
      L"NUL", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &security, OPEN_EXISTING, 0, nullptr
  ));
  if (nullHandle.get() == INVALID_HANDLE_VALUE)
    return;
  SIZE_T attributeSize = 0;
  InitializeProcThreadAttributeList(nullptr, 1, 0, &attributeSize);
  std::vector<BYTE> attributeBytes(attributeSize);
  auto *attributes = reinterpret_cast<PPROC_THREAD_ATTRIBUTE_LIST>(attributeBytes.data());
  if (!InitializeProcThreadAttributeList(attributes, 1, 0, &attributeSize))
    return;
  const auto releaseAttributes = [](PPROC_THREAD_ATTRIBUTE_LIST list) { DeleteProcThreadAttributeList(list); };
  std::unique_ptr<_PROC_THREAD_ATTRIBUTE_LIST, decltype(releaseAttributes)> attributeOwner(
      attributes, releaseAttributes
  );
  HANDLE inherited[]{output, nullHandle.get()};
  if (!UpdateProcThreadAttribute(
          attributes, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inherited, sizeof(inherited), nullptr, nullptr
      ))
    return;
  MSWindowsHandle job(CreateJobObjectW(nullptr, nullptr));
  JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
  limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
  if (!job.get() || !SetInformationJobObject(job.get(), JobObjectExtendedLimitInformation, &limits, sizeof(limits)))
    return;
  std::vector<wchar_t> executable(32768);
  const auto length = GetModuleFileNameW(nullptr, executable.data(), static_cast<DWORD>(executable.size()));
  if (!length || length >= executable.size())
    return;
  auto command =
      L"\"" + std::wstring(executable.data()) + L"\" --read-file-clipboard " + std::to_wstring(read->version);
  wchar_t desktop[] = L"winsta0\\default";
  STARTUPINFOEXW startup{};
  startup.StartupInfo.cb = sizeof(startup);
  startup.StartupInfo.lpDesktop = desktop;
  startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
  startup.StartupInfo.hStdInput = nullHandle.get();
  startup.StartupInfo.hStdOutput = output;
  startup.StartupInfo.hStdError = nullHandle.get();
  startup.lpAttributeList = attributes;
  PROCESS_INFORMATION process{};
  if (!CreateProcessAsUserW(
          token.get(), executable.data(), command.data(), nullptr, nullptr, TRUE,
          CREATE_NO_WINDOW | CREATE_SUSPENDED | EXTENDED_STARTUPINFO_PRESENT, nullptr, nullptr, &startup.StartupInfo,
          &process
      )) {
    read->result = HRESULT_FROM_WIN32(GetLastError());
    return;
  }
  MSWindowsHandle processHandle(process.hProcess), threadHandle(process.hThread);
  if (!AssignProcessToJobObject(job.get(), process.hProcess)) {
    TerminateProcess(process.hProcess, 1);
    return;
  }
  if (ResumeThread(process.hThread) == static_cast<DWORD>(-1))
    return;
  std::vector<BYTE> bytes;
  const auto deadline = GetTickCount64() + 10000;
  bool exited = false;
  while (GetTickCount64() < deadline) {
    DWORD available = 0;
    if (!PeekNamedPipe(input, nullptr, 0, nullptr, &available, nullptr))
      break;
    if (available) {
      if (available > maxReaderBytes - bytes.size())
        break;
      const auto before = bytes.size();
      bytes.resize(before + available);
      DWORD received = 0;
      if (!ReadFile(input, bytes.data() + before, available, &received, nullptr))
        break;
      bytes.resize(before + received);
    } else if (exited) {
      DWORD code = 1;
      GetExitCodeProcess(process.hProcess, &code);
      if (code || bytes.size() < sizeof(DWORD) * 2)
        break;
      size_t offset = 0;
      auto word = [&bytes, &offset](DWORD &value) {
        if (bytes.size() - offset < sizeof(value))
          return false;
        std::memcpy(&value, bytes.data() + offset, sizeof(value));
        offset += sizeof(value);
        return true;
      };
      DWORD result = 0, count = 0;
      if (!word(result) || !word(count) || count > 10000)
        break;
      std::vector<std::wstring> paths;
      for (DWORD i = 0; i < count; ++i) {
        DWORD size = 0;
        if (!word(size) || !size || size > 32767 || size * sizeof(wchar_t) > bytes.size() - offset)
          break;
        std::wstring path(size, L'\0');
        std::memcpy(path.data(), bytes.data() + offset, size * sizeof(wchar_t));
        offset += size * sizeof(wchar_t);
        if (path.find(L'\0') != std::wstring::npos)
          break;
        paths.push_back(std::move(path));
      }
      if (paths.size() != count || offset != bytes.size())
        break;
      read->result = static_cast<HRESULT>(result);
      if (SUCCEEDED(read->result))
        read->paths = std::move(paths);
      return;
    }
    exited = WaitForSingleObject(process.hProcess, 10) == WAIT_OBJECT_0;
  }
  read->result = HRESULT_FROM_WIN32(ERROR_TIMEOUT);
}

void readOleFiles(const std::shared_ptr<OleFiles> &read)
{
  if (systemAccount()) {
    readUserFiles(read);
    read->done = true;
  } else {
    readLocalOleFiles(read);
  }
}

bool pollOleFiles()
{
  const auto version = GetClipboardSequenceNumber();
  bool available = false;
  if (oleRead) {
    if (!oleRead->done)
      return false;
    if (oleRead->version == version && !IsClipboardFormatAvailable(markerFormat())) {
      olePaths.clear();
      for (const auto &path : oleRead->paths)
        olePaths.push_back(QString::fromStdWString(path));
      oleVersion = version;
      available = !olePaths.isEmpty();
      LOG_DEBUG(
          "file clipboard: OLE file list result=0x%08lx count=%d", static_cast<unsigned long>(oleRead->result),
          static_cast<int>(olePaths.size())
      );
    }
    oleRead.reset();
  }
  // Explorer's OLE object can be enumerated while native format availability
  // reports false. Ask IDataObject for files once per foreign clipboard change.
  if (version && version != attemptedVersion && !MSWindowsClipboard::isOwnedByDeskflow() &&
      !IsClipboardFormatAvailable(markerFormat()) && !IsClipboardFormatAvailable(CF_HDROP)) {
    attemptedVersion = version;
    auto read = std::make_shared<OleFiles>();
    read->version = version;
    oleRead = read;
    try {
      std::thread([read] {
        try {
          readOleFiles(read);
        } catch (...) {
          read->result = E_UNEXPECTED;
          read->done = true;
        }
      }).detach();
    } catch (...) {
      oleRead.reset();
      LOG_WARN("file clipboard: cannot start OLE reader");
    }
  }
  return available;
}

bool publish(const QStringList &paths, void *window, std::optional<quint64> expected = {})
{
  // Use the existing screen window and eagerly rendered CF_HDROP. OLE's lazy
  // data object can wait on another thread while the core is handling input.
  if (!window)
    return false;
  MSWindowsClipboard clipboard(static_cast<HWND>(window));
  if (!clipboard.open(0))
    return false;
  bool okay = (!expected || clipboardVersion() == *expected) && clipboard.empty();
  if (okay)
    okay = put(markerFormat(), QByteArray(1, '\0'));
  if (okay && !paths.isEmpty()) {
    std::wstring list;
    for (const auto &path : paths) {
      list += QDir::toNativeSeparators(path).toStdWString();
      list += L'\0';
    }
    list += L'\0';
    DROPFILES header{};
    header.pFiles = sizeof(header);
    header.fWide = TRUE;
    QByteArray bytes(reinterpret_cast<const char *>(&header), sizeof(header));
    bytes.append(reinterpret_cast<const char *>(list.data()), list.size() * sizeof(wchar_t));
    const DWORD effect = DROPEFFECT_COPY;
    okay = put(CF_HDROP, bytes) && put(RegisterClipboardFormatW(L"Preferred DropEffect"),
                                       QByteArray(reinterpret_cast<const char *>(&effect), sizeof(effect)));
  }
  clipboard.close();
  return okay;
}
#else
class Pasteboard
{
public:
  explicit Pasteboard(CFStringRef name = kPasteboardClipboard)
  {
    if (PasteboardCreate(name, &ref) == noErr)
      PasteboardSynchronize(ref);
  }
  ~Pasteboard()
  {
    if (ref)
      CFRelease(ref);
  }
  PasteboardRef ref = nullptr;
};

bool remote(PasteboardRef board)
{
  PasteboardItemID item;
  PasteboardFlavorFlags flags;
  return board && PasteboardGetItemIdentifier(board, 1, &item) == noErr &&
         PasteboardGetItemFlavorFlags(board, item, CFSTR("org.deskflow.received-files"), &flags) == noErr;
}

QStringList readFiles(PasteboardRef board)
{
  ItemCount count = 0;
  if (!board || remote(board) || PasteboardGetItemCount(board, &count) != noErr || count > 10000)
    return {};
  QStringList paths;
  for (ItemCount i = 1; i <= count; ++i) {
    PasteboardItemID item;
    CFDataRef data = nullptr;
    if (PasteboardGetItemIdentifier(board, i, &item) != noErr ||
        PasteboardCopyItemFlavorData(board, item, CFSTR("public.file-url"), &data) != noErr)
      return {};
    const auto url =
        CFURLCreateWithBytes(nullptr, CFDataGetBytePtr(data), CFDataGetLength(data), kCFStringEncodingUTF8, nullptr);
    CFRelease(data);
    if (!url)
      return {};
    // Finder supplies file-reference URLs (/.file/id=...), not ordinary paths.
    const auto resolved = CFURLCreateFilePathURL(nullptr, url, nullptr);
    CFRelease(url);
    if (!resolved)
      return {};
    const auto path = CFURLCopyFileSystemPath(resolved, kCFURLPOSIXPathStyle);
    CFRelease(resolved);
    if (!path)
      return {};
    paths.push_back(QString::fromCFString(path));
    CFRelease(path);
  }
  return paths;
}

bool put(PasteboardRef board, PasteboardItemID item, CFStringRef format, const QByteArray &bytes)
{
  const auto data = CFDataCreate(nullptr, reinterpret_cast<const UInt8 *>(bytes.constData()), bytes.size());
  if (!data)
    return false;
  const bool okay = PasteboardPutItemFlavor(board, item, format, data, kPasteboardFlavorNoFlags) == noErr;
  CFRelease(data);
  return okay;
}

bool publishFiles(PasteboardRef board, const QStringList &paths)
{
  if (!board || PasteboardClear(board) != noErr ||
      !put(board, reinterpret_cast<PasteboardItemID>(1), CFSTR("org.deskflow.received-files"), QByteArrayLiteral("1")))
    return false;
  for (qsizetype i = 0; i < paths.size(); ++i) {
    if (!put(
            board, reinterpret_cast<PasteboardItemID>(i + 1), CFSTR("public.file-url"),
            QUrl::fromLocalFile(paths[i]).toEncoded()
        ))
      return false;
  }
  return true;
}

bool publish(const QStringList &paths, void *, std::optional<quint64> expected = {})
{
  Pasteboard board;
  return (!expected || clipboardVersion() == *expected) && publishFiles(board.ref, paths);
}

QStringList readFiles()
{
  Pasteboard board;
  return readFiles(board.ref);
}
#endif
} // namespace

std::string FileClipboard::offer()
{
  auto paths = readFiles();
#ifdef Q_OS_WIN
  if (paths.isEmpty() && oleVersion == GetClipboardSequenceNumber())
    paths = olePaths;
#endif
  return FileTransfer::offer(paths);
}

bool FileClipboard::isRemote()
{
#ifdef Q_OS_WIN
  return IsClipboardFormatAvailable(markerFormat());
#else
  Pasteboard board;
  return remote(board.ref);
#endif
}

bool FileClipboard::receive(const std::string &offer, void *window)
{
  // Both logical clipboards map to one native clipboard. A second notification
  // for the same offer must not clear it or restart the active download.
  if (currentOffer == offer && currentVersion == clipboardVersion() && isRemote())
    return true;
  if (!publish({}, window))
    return false;
  currentOffer = offer;
  currentVersion = clipboardVersion();
  const auto before = currentVersion;
  const bool started = FileTransfer::fetch(
      offer,
      [before, window](const QStringList &paths) {
        if (!paths.isEmpty() && publish(paths, window, before)) {
          currentVersion = clipboardVersion();
          LOG_INFO("file clipboard: %d files ready to paste", static_cast<int>(paths.size()));
          FileTransfer::reportProgress(QStringLiteral("ready"));
        } else if (currentVersion == before) {
          currentOffer.clear();
        }
      },
      [before] { return clipboardVersion() != before; }
  );
  if (!started)
    currentOffer.clear();
  return started;
}

bool FileClipboard::poll()
{
#ifdef Q_OS_WIN
  return pollOleFiles();
#else
  return false;
#endif
}

#ifdef Q_OS_WIN
int FileClipboard::runReader(unsigned long version)
{
  try {
    if (systemAccount() || GetClipboardSequenceNumber() != version)
      return 1;
    auto read = std::make_shared<OleFiles>();
    readLocalOleFiles(read);
    if (GetClipboardSequenceNumber() != version)
      return 1;
    std::vector<BYTE> bytes;
    auto word = [&bytes](DWORD value) {
      const auto *data = reinterpret_cast<const BYTE *>(&value);
      bytes.insert(bytes.end(), data, data + sizeof(value));
    };
    word(static_cast<DWORD>(read->result));
    word(static_cast<DWORD>(read->paths.size()));
    for (const auto &path : read->paths) {
      if (bytes.size() + sizeof(DWORD) + path.size() * sizeof(wchar_t) > maxReaderBytes)
        return 1;
      word(static_cast<DWORD>(path.size()));
      const auto *data = reinterpret_cast<const BYTE *>(path.data());
      bytes.insert(bytes.end(), data, data + path.size() * sizeof(wchar_t));
    }
    DWORD offset = 0;
    while (offset < bytes.size()) {
      DWORD written = 0;
      if (!WriteFile(
              GetStdHandle(STD_OUTPUT_HANDLE), bytes.data() + offset, static_cast<DWORD>(bytes.size()) - offset,
              &written, nullptr
          ) ||
          !written)
        return 1;
      offset += written;
    }
    return 0;
  } catch (...) {
    return 1;
  }
}
#endif
