// Modified by tutoupifengxiaa, 2026-09-15 to 2026-09-17: unofficial file-transfer fork; see README.md.
/* SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception */
#include "FileDragController.h"
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <QCoreApplication>
#include <QEventLoop>
#include <QTimer>
#include <QUrl>
#include <ShlObj.h>
#include <Shlwapi.h>
#include <Windows.h>
#include <atomic>
#include <cstring>

static DWORD sourceThread = 0;
static HWND sourceCapture = nullptr;

bool nativeFileDragButtonDown()
{
  return (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;
}
QStringList nativeFileDragPaths(const QList<QUrl> &urls)
{
  sourceThread = GetWindowThreadProcessId(GetForegroundWindow(), nullptr);
  GUITHREADINFO info{sizeof(info)};
  sourceCapture = sourceThread && GetGUIThreadInfo(sourceThread, &info) ? info.hwndCapture : nullptr;
  QStringList paths;
  for (const auto &url : urls) {
    if (!url.isLocalFile())
      return {};
    paths.push_back(url.toLocalFile());
  }
  return paths;
}
bool cancelNativeFileDragSource()
{
  if (!sourceCapture) {
    qWarning("file drag: cannot confirm the native source capture");
    return false;
  }
  INPUT events[2]{};
  for (auto &event : events) {
    event.type = INPUT_KEYBOARD;
    event.ki.wVk = VK_ESCAPE;
  }
  events[1].ki.dwFlags = KEYEVENTF_KEYUP;
  return SendInput(2, events, sizeof(INPUT)) == 2;
}

bool nativeFileDragSourceEnded()
{
  GUITHREADINFO info{sizeof(info)};
  return sourceThread && sourceCapture &&
         (!GetGUIThreadInfo(sourceThread, &info) || info.hwndCapture != sourceCapture);
}

namespace {
CLIPFORMAT descriptorFormat()
{
  return static_cast<CLIPFORMAT>(RegisterClipboardFormatW(CFSTR_FILEDESCRIPTORW));
}
CLIPFORMAT contentsFormat()
{
  return static_cast<CLIPFORMAT>(RegisterClipboardFormatW(CFSTR_FILECONTENTS));
}
CLIPFORMAT effectFormat()
{
  return static_cast<CLIPFORMAT>(RegisterClipboardFormatW(CFSTR_PREFERREDDROPEFFECT));
}

HRESULT globalData(const void *bytes, size_t size, STGMEDIUM *medium)
{
  HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE | GMEM_ZEROINIT, size);
  if (!memory)
    return E_OUTOFMEMORY;
  void *target = GlobalLock(memory);
  if (!target) {
    GlobalFree(memory);
    return E_OUTOFMEMORY;
  }
  std::memcpy(target, bytes, size);
  GlobalUnlock(memory);
  medium->tymed = TYMED_HGLOBAL;
  medium->hGlobal = memory;
  return S_OK;
}
class FileDragDataObject final : public IDataObject, public IDropSource
{
public:
  explicit FileDragDataObject(std::shared_ptr<DragFiles> files) : files(std::move(files))
  {
  }
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void **out) override
  {
    if (!out)
      return E_POINTER;
    *out = nullptr;
    if (iid == IID_IUnknown || iid == IID_IDataObject)
      *out = static_cast<IDataObject *>(this);
    else if (iid == IID_IDropSource)
      *out = static_cast<IDropSource *>(this);
    else
      return E_NOINTERFACE;
    AddRef();
    return S_OK;
  }
  ULONG STDMETHODCALLTYPE AddRef() override
  {
    return ++references;
  }
  ULONG STDMETHODCALLTYPE Release() override
  {
    const auto left = --references;
    if (!left)
      delete this;
    return left;
  }
  HRESULT STDMETHODCALLTYPE QueryContinueDrag(BOOL escape, DWORD) override
  {
    // Keep the GUI IPC alive inside OLE's native modal loop. The physical
    // button is on the other computer; its release arrives through the core.
    QCoreApplication::processEvents(QEventLoop::AllEvents, 1);
    if (escape || files->cancelled) {
      qInfo(
          "file drag: OLE cancelled, escape=%d, cancelled=%d, released=%d", escape, files->cancelled, files->released
      );
      return DRAGDROP_S_CANCEL;
    }
    if (files->released) {
      // OLE checks continuation before updating the drop target. Let it
      // resolve the final remote cursor position once before committing.
      if (!releasePending) {
        releasePending = true;
        const auto window = GetCapture();
        POINT point{};
        if (window && GetCursorPos(&point) && ScreenToClient(window, &point))
          PostMessageW(window, WM_MOUSEMOVE, MK_LBUTTON, MAKELPARAM(point.x, point.y));
        return S_OK;
      }
      return DRAGDROP_S_DROP;
    }
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE GiveFeedback(DWORD effect) override
  {
    if (effect != lastEffect) {
      POINT point{};
      wchar_t windowClass[128]{};
      if (GetCursorPos(&point))
        GetClassNameW(WindowFromPoint(point), windowClass, 128);
      qInfo("file drag: native target effect=%lu, class=%s", effect,
            qPrintable(QString::fromWCharArray(windowClass)));
      lastEffect = effect;
    }
    return DRAGDROP_S_USEDEFAULTCURSORS;
  }
  HRESULT STDMETHODCALLTYPE QueryGetData(FORMATETC *format) override
  {
    if (!format)
      return E_POINTER;
    if (format->dwAspect != DVASPECT_CONTENT)
      return DV_E_DVASPECT;
    if (format->cfFormat == contentsFormat()) {
      if (format->lindex < 0 || format->lindex >= files->entries.size())
        return DV_E_LINDEX;
      return format->tymed & TYMED_ISTREAM ? S_OK : DV_E_TYMED;
    }
    if (format->cfFormat == descriptorFormat() || format->cfFormat == effectFormat())
      return format->tymed & TYMED_HGLOBAL ? S_OK : DV_E_TYMED;
    return DV_E_FORMATETC;
  }
  HRESULT STDMETHODCALLTYPE GetData(FORMATETC *format, STGMEDIUM *medium) override
  {
    if (!medium)
      return E_POINTER;
    *medium = {};
    const auto result = QueryGetData(format);
    if (FAILED(result))
      return result;
    if (format->cfFormat == descriptorFormat()) {
      QByteArray bytes(
          static_cast<qsizetype>(offsetof(FILEGROUPDESCRIPTORW, fgd) + files->entries.size() * sizeof(FILEDESCRIPTORW)),
          0
      );
      auto *group = reinterpret_cast<FILEGROUPDESCRIPTORW *>(bytes.data());
      group->cItems = static_cast<UINT>(files->entries.size());
      for (qsizetype i = 0; i < files->entries.size(); ++i) {
        auto &item = group->fgd[i];
        const auto &entry = files->entries[i];
        item.dwFlags = FD_ATTRIBUTES | FD_FILESIZE | FD_PROGRESSUI | FD_UNICODE;
        item.dwFileAttributes = FILE_ATTRIBUTE_NORMAL;
        item.nFileSizeHigh = static_cast<DWORD>(static_cast<quint64>(entry.size) >> 32);
        item.nFileSizeLow = static_cast<DWORD>(entry.size);
        const auto name = entry.name.toStdWString();
        if (name.size() >= MAX_PATH)
          return DV_E_FORMATETC;
        std::memcpy(item.cFileName, name.c_str(), (name.size() + 1) * sizeof(wchar_t));
      }
      return globalData(bytes.constData(), bytes.size(), medium);
    }
    if (format->cfFormat == effectFormat()) {
      const DWORD effect = DROPEFFECT_COPY;
      return globalData(&effect, sizeof(effect), medium);
    }
    if (!files->ready && !files->cancelled) {
      QEventLoop loop;
      QTimer deadline;
      deadline.setSingleShot(true);
      deadline.start(60000);
      QObject::connect(&deadline, &QTimer::timeout, &loop, [&] {
        files->cancelled = true;
        loop.quit();
      });
      QObject::connect(files.get(), &DragFiles::updated, &loop, [&] {
        deadline.start(60000);
        if (files->ready || files->cancelled)
          loop.quit();
      });
      loop.exec();
    }
    if (files->cancelled || !files->ready)
      return E_ABORT;
    IStream *stream = nullptr;
    const auto path = files->paths[format->lindex].toStdWString();
    const auto opened =
        SHCreateStreamOnFileEx(path.c_str(), STGM_READ | STGM_SHARE_DENY_WRITE, 0, FALSE, nullptr, &stream);
    if (FAILED(opened)) {
      qWarning("file drag: could not open received stream 0x%08lx", static_cast<unsigned long>(opened));
      return opened;
    }
    qInfo("file drag: target requested file stream %ld", format->lindex);
    medium->tymed = TYMED_ISTREAM;
    medium->pstm = stream;
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE GetDataHere(FORMATETC *, STGMEDIUM *) override
  {
    return DATA_E_FORMATETC;
  }
  HRESULT STDMETHODCALLTYPE GetCanonicalFormatEtc(FORMATETC *, FORMATETC *out) override
  {
    if (!out)
      return E_POINTER;
    out->ptd = nullptr;
    return E_NOTIMPL;
  }
  HRESULT STDMETHODCALLTYPE SetData(FORMATETC *, STGMEDIUM *medium, BOOL release) override
  {
    if (release && medium)
      ReleaseStgMedium(medium);
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE EnumFormatEtc(DWORD direction, IEnumFORMATETC **out) override
  {
    if (!out)
      return E_POINTER;
    *out = nullptr;
    if (direction != DATADIR_GET)
      return E_NOTIMPL;
    FORMATETC formats[] = {
        {descriptorFormat(), nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL},
        {contentsFormat(), nullptr, DVASPECT_CONTENT, 0, TYMED_ISTREAM},
        {effectFormat(), nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL}
    };
    return SHCreateStdEnumFmtEtc(3, formats, out);
  }
  HRESULT STDMETHODCALLTYPE DAdvise(FORMATETC *, DWORD, IAdviseSink *, DWORD *) override
  {
    return OLE_E_ADVISENOTSUPPORTED;
  }
  HRESULT STDMETHODCALLTYPE DUnadvise(DWORD) override
  {
    return OLE_E_ADVISENOTSUPPORTED;
  }
  HRESULT STDMETHODCALLTYPE EnumDAdvise(IEnumSTATDATA **) override
  {
    return OLE_E_ADVISENOTSUPPORTED;
  }

private:
  std::atomic<ULONG> references{1};
  DWORD lastEffect = static_cast<DWORD>(-1);
  bool releasePending = false;
  std::shared_ptr<DragFiles> files;
};
} // namespace
void startNativeFileDrag(QWidget *, const std::shared_ptr<DragFiles> &files, std::function<void(bool)> finished)
{
  const auto initialized = OleInitialize(nullptr);
  if (FAILED(initialized)) {
    qWarning("file drag: OleInitialize failed 0x%08lx", static_cast<unsigned long>(initialized));
    finished(false);
    return;
  }
  // The originating mouse event belongs to the other computer. OLE waits for
  // a mouse message before entering its drag loop, so seed this thread's queue
  // through a private message-only window. No desktop click is injected.
  const auto origin = CreateWindowExW(
      0, L"STATIC", L"Deskflow file drag source", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, GetModuleHandleW(nullptr),
      nullptr
  );
  if (!origin || !PostMessageW(origin, WM_MOUSEMOVE, MK_LBUTTON, 0)) {
    qWarning("file drag: could not initialize the mouse message queue (%lu)", GetLastError());
    if (origin)
      DestroyWindow(origin);
    OleUninitialize();
    finished(false);
    return;
  }
  auto *object = new FileDragDataObject(files);
  // Refresh the native target when data or button state arrives over IPC,
  // including when the mouse is stationary throughout the handoff.
  const auto refreshTarget = [] {
    const auto window = GetCapture();
    POINT point{};
    if (window && GetCursorPos(&point) && ScreenToClient(window, &point))
      PostMessageW(window, WM_MOUSEMOVE, MK_LBUTTON, MAKELPARAM(point.x, point.y));
  };
  const auto wake = QObject::connect(files.get(), &DragFiles::updated, files.get(), refreshTarget);
  // The background GUI does not reliably receive the core's injected motion
  // outside its own windows. Keep OLE's private capture window up to date.
  QTimer motion;
  POINT lastPoint{};
  GetCursorPos(&lastPoint);
  QObject::connect(&motion, &QTimer::timeout, files.get(), [&lastPoint, refreshTarget] {
    POINT point{};
    if (GetCursorPos(&point) && (point.x != lastPoint.x || point.y != lastPoint.y)) {
      lastPoint = point;
      refreshTarget();
    }
  });
  motion.start(16);
  DWORD effect = DROPEFFECT_NONE;
  const auto result = DoDragDrop(object, object, DROPEFFECT_COPY, &effect);
  motion.stop();
  qInfo(
      "file drag: OLE ended result=0x%08lx, effect=%lu, released=%d, cancelled=%d", static_cast<unsigned long>(result),
      effect, files->released, files->cancelled
  );
  QObject::disconnect(wake);
  object->Release();
  DestroyWindow(origin);
  OleUninitialize();
  finished(result == DRAGDROP_S_DROP && effect == DROPEFFECT_COPY && !files->cancelled);
}
