// Modified by tutoupifengxiaa, 2026-09-15 to 2026-09-17: unofficial file-transfer fork; see README.md.
/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2013 - 2016 Synergy App Ltd
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#pragma once

#import <CoreFoundation/CoreFoundation.h>

#if defined(__cplusplus)
extern "C"
{
#endif

  CFStringRef getDraggedFileURL();
  CFIndex getClipboardChangeCount();

#if defined(__cplusplus)
}
#endif
