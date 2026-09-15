// In-client updater stub for POSIX: the Windows build downloads and swaps release zips through
// WinHTTP. The Linux build reports that updates are unavailable instead.
// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef _MSC_VER
#include "updater.h"

namespace host::updater {
void check(const std::string&) {}
State state() { return State::UpToDate; }
std::string latest_version() { return {}; }
std::string message() { return "Updates are not available in the Linux build."; }
void download_and_install() {}
void shutdown() {}
}  // namespace host::updater

#endif  // !_MSC_VER
