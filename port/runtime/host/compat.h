// Small shims so the runtime's MSVC-isms compile with GCC/Clang on POSIX hosts.
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#ifdef _MSC_VER

#include <io.h>
#include <windows.h>

#else

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <strings.h>
#include <sys/stat.h>

inline int _fseeki64(std::FILE* f, long long off, int whence) {
  return fseeko(f, (off_t)off, whence);
}
inline long long _ftelli64(std::FILE* f) { return (long long)ftello(f); }
inline int _strnicmp(const char* a, const char* b, std::size_t n) { return strncasecmp(a, b, n); }
inline int _stricmp(const char* a, const char* b) { return strcasecmp(a, b); }
inline int _mkdir(const char* p) { return ::mkdir(p, 0755); }
inline bool _CreateDirectory(const char* p, void*) {
  std::error_code ec;
  std::filesystem::create_directories(p, ec);
  return !ec;
}
#define CreateDirectoryA(p, s) _CreateDirectory((p), (s))

#endif
