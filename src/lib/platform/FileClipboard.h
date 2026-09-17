// Modified by tutoupifengxiaa, 2026-09-15 to 2026-09-17: unofficial file-transfer fork; see docs/SECONDARY_DEVELOPMENT.md.
/* SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception */
#pragma once
#include <string>

namespace FileClipboard {
std::string offer();
bool receive(const std::string &offer, void *window = nullptr);
bool isRemote();
// Poll a background OLE file-list read; true requests a new clipboard notification.
bool poll();

#ifdef _WIN32
// Short-lived user-session reader; writes only to its inherited private pipe.
int runReader(unsigned long version);
#endif
} // namespace FileClipboard
