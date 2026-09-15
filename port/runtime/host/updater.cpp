// SPDX-License-Identifier: GPL-2.0-or-later
#ifdef _MSC_VER
#include "updater.h"
#include "host.h"
#define NOMINMAX
#include <windows.h>
#include <winhttp.h>
#include <nlohmann/json.hpp>
#include <atomic>
#include <cstdio>
#include <fstream>
#include <mutex>
#include <thread>

#pragma comment(lib, "winhttp.lib")

namespace host::updater {
namespace {
// The release list rather than /releases/latest: that endpoint skips pre-releases (betas).
const char* REPO_API = "https://api.github.com/repos/hero88go/melee-unlocked/releases?per_page=10";
std::atomic<State> g_state{State::Idle};
std::mutex g_mutex;
std::string g_current, g_latest, g_zip_url, g_message, g_zip_path;
std::thread g_thread;

std::wstring widen(const std::string& s) { int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0); std::wstring w(n ? n - 1 : 0, 0); if (n) MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, w.data(), n); return w; }
void set_message(const std::string& m) { std::lock_guard<std::mutex> lk(g_mutex); g_message = m; }

// GET with redirects (GitHub release assets redirect to a CDN). Returns the body.
bool http_get(const std::string& url, std::string* out, int* status) {
  std::wstring wurl = widen(url);
  URL_COMPONENTS uc{}; uc.dwStructSize = sizeof uc;
  wchar_t host[256]{}, path[4096]{};
  uc.lpszHostName = host; uc.dwHostNameLength = 256; uc.lpszUrlPath = path; uc.dwUrlPathLength = 4096;
  if (!WinHttpCrackUrl(wurl.c_str(), 0, 0, &uc)) return false;
  HINTERNET session = WinHttpOpen(L"MeleeUnlocked updater", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
  if (!session) return false;
  WinHttpSetTimeouts(session, 8000, 8000, 30000, 120000);
  bool ok = false;
  HINTERNET conn = WinHttpConnect(session, host, uc.nPort, 0);
  if (conn) {
    HINTERNET req = WinHttpOpenRequest(conn, L"GET", path, nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, uc.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0);
    if (req) {
      DWORD redirect = WINHTTP_OPTION_REDIRECT_POLICY_ALWAYS;
      WinHttpSetOption(req, WINHTTP_OPTION_REDIRECT_POLICY, &redirect, sizeof redirect);
      if (WinHttpSendRequest(req, L"User-Agent: MeleeUnlocked\r\nAccept: application/vnd.github+json\r\n", (DWORD)-1, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) && WinHttpReceiveResponse(req, nullptr)) {
        DWORD code = 0, size = sizeof code;
        WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &code, &size, WINHTTP_NO_HEADER_INDEX);
        if (status) *status = (int)code;
        std::string body;
        for (;;) {
          DWORD avail = 0;
          if (!WinHttpQueryDataAvailable(req, &avail) || !avail) break;
          std::string chunk(avail, 0); DWORD got = 0;
          if (!WinHttpReadData(req, chunk.data(), avail, &got)) break;
          body.append(chunk.data(), got);
        }
        if (out) *out = std::move(body);
        ok = true;
      }
      WinHttpCloseHandle(req);
    }
    WinHttpCloseHandle(conn);
  }
  WinHttpCloseHandle(session);
  return ok;
}

// "0.1.2-beta" -> (0,1,2); pre-release suffixes are ignored for ordering.
bool newer(const std::string& a, const std::string& b) {
  int x[3] = {0, 0, 0}, y[3] = {0, 0, 0};
  std::sscanf(a.c_str(), "%d.%d.%d", &x[0], &x[1], &x[2]);
  std::sscanf(b.c_str(), "%d.%d.%d", &y[0], &y[1], &y[2]);
  for (int i = 0; i < 3; ++i) if (x[i] != y[i]) return x[i] > y[i];
  return false;
}

void join() { if (g_thread.joinable()) g_thread.join(); }
}  // namespace

void check(const std::string& current_version) {
  join();
  g_current = current_version;
  g_state = State::Checking;
  g_thread = std::thread([] {
    std::string body; int status = 0;
    if (!http_get(REPO_API, &body, &status) || status != 200) { set_message(status ? "Update check failed (HTTP " + std::to_string(status) + ")" : "Update check failed (no connection)"); host::log("updater: GET %s failed, HTTP %d, error %lu", REPO_API, status, (unsigned long)GetLastError()); g_state = State::Failed; return; }
    auto list = nlohmann::json::parse(body, nullptr, false);
    nlohmann::json j;
    if (list.is_array()) for (auto& r : list) if (r.is_object() && r.count("tag_name") && !r.value("draft", false)) { j = r; break; }
    if (!j.is_object()) { set_message("Update check failed (bad response)"); g_state = State::Failed; return; }
    std::string tag = j["tag_name"].get<std::string>();
    if (!tag.empty() && tag[0] == 'v') tag.erase(0, 1);
    std::string zip;
    if (j.count("assets") && j["assets"].is_array())
      for (auto& a : j["assets"]) if (a.is_object() && a.value("name", std::string()).find("win64.zip") != std::string::npos) zip = a.value("browser_download_url", std::string());
    { std::lock_guard<std::mutex> lk(g_mutex); g_latest = tag; g_zip_url = zip; }
    if (newer(tag, g_current) && !zip.empty()) { set_message("Update available: " + tag); g_state = State::UpdateAvailable; host::log("updater: version %s available (running %s)", tag.c_str(), g_current.c_str()); }
    else { set_message("Up to date (" + g_current + ")"); g_state = State::UpToDate; }
  });
}

void shutdown() { join(); }
State state() { return g_state.load(); }
std::string latest_version() { std::lock_guard<std::mutex> lk(g_mutex); return g_latest; }
std::string message() { std::lock_guard<std::mutex> lk(g_mutex); return g_message; }

void download_and_install() {
  if (g_state != State::UpdateAvailable) return;
  join();
  g_state = State::Downloading;
  set_message("Downloading update...");
  g_thread = std::thread([] {
    std::string url; { std::lock_guard<std::mutex> lk(g_mutex); url = g_zip_url; }
    std::string body; int status = 0;
    if (!http_get(url, &body, &status) || status != 200 || body.size() < 1000000) { set_message("Download failed"); g_state = State::Failed; return; }
    char exe[MAX_PATH]; GetModuleFileNameA(nullptr, exe, MAX_PATH);
    std::string dir(exe); dir.resize(dir.find_last_of("\\/"));
    std::string zip = dir + "\\update.zip", bat = dir + "\\update.bat";
    { std::ofstream f(zip, std::ios::binary); f.write(body.data(), (std::streamsize)body.size()); if (!f) { set_message("Cannot write update.zip"); g_state = State::Failed; return; } }
    // Relaunch exactly what was started, so this works the same from the release batch file, the
    // launcher, or a development shortcut with its own arguments. Percent signs would be eaten by
    // the batch interpreter.
    std::string relaunch = GetCommandLineA();
    for (size_t i = relaunch.find('%'); i != std::string::npos; i = relaunch.find('%', i + 2)) relaunch.insert(i, 1, '%');

    // The script waits for this process to exit, unpacks the zip (Windows 10+ ships tar for zips),
    // copies the release folder over this one (keeping User\, settings, saves, replays) and starts
    // the game again. Written in binary: text mode would turn every \r\n into \r\r\n, and the
    // stray carriage return becomes part of the last argument on each line, which is what stopped
    // the relaunch from working.
    std::ofstream b(bat, std::ios::binary);
    b << "@echo off\r\ncd /d \"" << dir << "\"\r\n"
      << "set LOG=\"" << dir << "\\update.log\"\r\n"
      << "echo update started %DATE% %TIME%> %LOG%\r\n"
      << ":wait\r\ntasklist /FI \"PID eq " << GetCurrentProcessId() << "\" 2>nul | find \"" << GetCurrentProcessId() << "\" >nul && (timeout /t 1 /nobreak >nul & goto wait)\r\n"
      << "rmdir /s /q update_tmp 2>nul\r\nmkdir update_tmp\r\n"
      << "tar -xf update.zip -C update_tmp\r\n"
      << "if errorlevel 1 (echo could not unpack update.zip>> %LOG% & echo Could not unpack the update. & pause & exit /b 1)\r\n"
      << "set SRC=\r\n"
      << "for /d %%d in (update_tmp\\MeleeUnlocked-* update_tmp\\MeleePort-*) do set SRC=%%d\r\n"
      << "if not defined SRC (echo no release folder inside update.zip>> %LOG% & echo The update did not contain a release folder. & pause & exit /b 1)\r\n"
      << "echo copying from %SRC%>> %LOG%\r\n"
      << "xcopy /e /y /q \"%SRC%\\*\" \".\\\" >> %LOG% 2>&1\r\n"
      << "if errorlevel 1 (echo copy failed>> %LOG% & echo Could not copy the update into place. & pause & exit /b 1)\r\n"
      << "rmdir /s /q update_tmp\r\ndel update.zip\r\n"
      << "echo restarting: " << relaunch << ">> %LOG%\r\n"
      << "start \"\" " << relaunch << "\r\n"
      << "echo done>> %LOG%\r\n"
      << "del \"%~f0\"\r\n";
    b.close();
    set_message("Update downloaded; restarting to install");
    g_state = State::ReadyToInstall;
    host::log("updater: %zu bytes downloaded, installing via update.bat", body.size());
    STARTUPINFOA si{}; si.cb = sizeof si; PROCESS_INFORMATION pi{};
    // Doubled quotes: cmd strips one layer, and the path contains spaces.
    std::string cmd = "cmd /c \"\"" + bat + "\"\"";
    if (CreateProcessA(nullptr, cmd.data(), nullptr, nullptr, FALSE, CREATE_NEW_CONSOLE, nullptr, dir.c_str(), &si, &pi)) { CloseHandle(pi.hProcess); CloseHandle(pi.hThread); }
    host::request_exit(0);
  });
}
}  // namespace host::updater

#endif  // _MSC_VER
