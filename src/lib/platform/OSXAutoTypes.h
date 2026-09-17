// Modified by tutoupifengxiaa, 2026-09-15 to 2026-09-17: unofficial file-transfer fork; see docs/SECONDARY_DEVELOPMENT.md.
/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2022 Synergy App Ltd
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */
#pragma once

#include <Carbon/Carbon.h>
#include <memory>
#include <dispatch/dispatch.h>
#include <pthread.h>

using CFDeallocator = decltype(&CFRelease);
using AutoCFArray = std::unique_ptr<const __CFArray, CFDeallocator>;
using AutoCFDictionary = std::unique_ptr<const __CFDictionary, CFDeallocator>;
using AutoTISInputSourceRef = std::unique_ptr<__TISInputSource, CFDeallocator>;

// TIS must share the main thread with AppKit; a private mutex cannot
// serialize calls made internally by the macOS/Qt event loop.
template <typename Callback> void runOnOSXMainThread(Callback callback)
{
  if (pthread_main_np()) {
    callback();
  } else {
    dispatch_sync_f(dispatch_get_main_queue(), &callback, [](void *context) {
      (*static_cast<Callback *>(context))();
    });
  }
}
