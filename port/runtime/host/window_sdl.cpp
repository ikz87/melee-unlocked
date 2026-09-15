// SDL2 window, event pump and keyboard/gamepad mapping to GameCube pads (POSIX build).
// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef _MSC_VER
#include <SDL.h>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>
#include "host.h"
#include "window.h"

namespace host {

namespace {
SDL_Window* g_window = nullptr;
const Uint8* g_keys = nullptr;
std::mutex g_keys_mutex;
bool g_closed = false;
int g_client_w = 1280, g_client_h = 960;
ResizeCallback g_on_resize;
std::atomic<bool> g_fullscreen_toggle{false};
MessageCallback g_on_message;
std::atomic<bool> g_ui_capture{false};
std::mutex g_ui_pad_mutex;
PadState g_ui_pad{};
bool g_ui_gamecube = false;
bool g_fullscreen = false;

SDL_GameController* g_pads[4] = {};
}  // namespace

void* window_create(int w, int h, const char* title, bool visible) {
  if (SDL_InitSubSystem(SDL_INIT_VIDEO) != 0) die("cannot init SDL video: %s", SDL_GetError());
  Uint32 flags = SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI;
  if (!visible) flags |= SDL_WINDOW_HIDDEN;
  g_window = SDL_CreateWindow(title ? title : "Melee Unlocked", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, w, h, flags);
  if (!g_window) die("cannot create SDL window: %s", SDL_GetError());
  g_closed = false;
  g_client_w = w; g_client_h = h;
  SDL_GameControllerEventState(SDL_ENABLE);
  for (int i = 0; i < SDL_NumJoysticks() && i < 4; ++i)
    if (SDL_IsGameController(i)) g_pads[i] = SDL_GameControllerOpen(i);
  return g_window;
}

void window_set_fullscreen(bool enabled) {
  if (!g_window || enabled == g_fullscreen) return;
  SDL_SetWindowFullscreen(g_window, enabled ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0);
  g_fullscreen = enabled;
}

double window_refresh_rate() {
  SDL_DisplayMode mode{};
  int display = g_window ? SDL_GetWindowDisplayIndex(g_window) : 0;
  if (SDL_GetCurrentDisplayMode(display, &mode) == 0 && mode.refresh_rate > 1) return mode.refresh_rate;
  return 60.0;
}

void window_set_message_callback(MessageCallback cb) { g_on_message = std::move(cb); }
void window_input_capture(bool capture) { g_ui_capture.store(capture); }
bool window_ui_gamecube_pad(PadState& pad) { std::lock_guard<std::mutex> lock(g_ui_pad_mutex); pad = g_ui_pad; return g_ui_gamecube; }
void window_set_resize_callback(ResizeCallback cb) { g_on_resize = std::move(cb); }
bool window_take_fullscreen_toggle() { return g_fullscreen_toggle.exchange(false); }
void window_destroy() {
  for (auto& p : g_pads) if (p) SDL_GameControllerClose(p);
  if (g_window) { SDL_DestroyWindow(g_window); g_window = nullptr; }
}
bool window_closed() { return g_closed; }
void window_client_size(int* w, int* h) { if (w) *w = g_client_w; if (h) *h = g_client_h; }

void window_pump() {
  SDL_Event e;
  while (SDL_PollEvent(&e)) {
    if (g_on_message && g_on_message(g_window, e.type, 0, reinterpret_cast<intptr_t>(&e))) continue;
    switch (e.type) {
      case SDL_QUIT: g_closed = true; request_exit(0); break;
      case SDL_WINDOWEVENT:
        if (e.window.event == SDL_WINDOWEVENT_SIZE_CHANGED) {
          g_client_w = e.window.data1; g_client_h = e.window.data2;
          if (g_on_resize && g_client_w > 0 && g_client_h > 0) g_on_resize(g_client_w, g_client_h);
        } else if (e.window.event == SDL_WINDOWEVENT_CLOSE) {
          g_closed = true; request_exit(0);
        }
        break;
      case SDL_KEYDOWN:
        if (e.key.keysym.sym == SDLK_RETURN && (e.key.keysym.mod & KMOD_ALT)) g_fullscreen_toggle.store(true);
        break;
      case SDL_CONTROLLERDEVICEADDED:
        if (e.cdevice.which < 4) g_pads[e.cdevice.which] = SDL_GameControllerOpen(e.cdevice.which);
        break;
      case SDL_CONTROLLERDEVICEREMOVED: {
        SDL_GameController* c = SDL_GameControllerFromInstanceID(e.cdevice.which);
        for (auto& p : g_pads) if (p == c) { SDL_GameControllerClose(p); p = nullptr; }
        break;
      }
    }
  }
  g_keys = SDL_GetKeyboardState(nullptr);
}

void window_set_title(const char* title) { if (g_window) SDL_SetWindowTitle(g_window, title ? title : ""); }

// GameCube button bits (PADStatus.button)
enum : uint16_t {
  PAD_LEFT = 0x0001, PAD_RIGHT = 0x0002, PAD_DOWN = 0x0004, PAD_UP = 0x0008, PAD_Z = 0x0010, PAD_R = 0x0020, PAD_L = 0x0040,
  PAD_A = 0x0100, PAD_B = 0x0200, PAD_X = 0x0400, PAD_Y = 0x0800, PAD_START = 0x1000,
};

namespace {
struct ScriptEntry { uint32_t frame; uint16_t buttons; int8_t sx, sy, cx, cy; int port;  bool relative = false; };
std::vector<ScriptEntry> g_script;
uint32_t g_script_ports = 1;
static bool g_script_relative_section = false;
static uint32_t g_script_loop = 0;
static std::atomic<uint32_t> g_match_start_retrace{0};

PadState PollKeyboard() {
  PadState p{};
  p.err = 0;
  std::lock_guard<std::mutex> lock(g_keys_mutex);
  if (!g_keys) return p;
  auto key = [](SDL_Scancode sc) { return g_keys[sc] != 0; };
  int sx = 0, sy = 0, cx = 0, cy = 0;
  if (key(SDL_SCANCODE_LEFT)) sx -= 127; if (key(SDL_SCANCODE_RIGHT)) sx += 127;
  if (key(SDL_SCANCODE_UP)) sy += 127; if (key(SDL_SCANCODE_DOWN)) sy -= 127;
  if (key(SDL_SCANCODE_J)) cx -= 127; if (key(SDL_SCANCODE_L)) cx += 127;
  if (key(SDL_SCANCODE_I)) cy += 127; if (key(SDL_SCANCODE_K)) cy -= 127;
  if (key(SDL_SCANCODE_Z)) p.button |= PAD_A; if (key(SDL_SCANCODE_X)) p.button |= PAD_B;
  if (key(SDL_SCANCODE_C)) p.button |= PAD_X; if (key(SDL_SCANCODE_V)) p.button |= PAD_Y;
  if (key(SDL_SCANCODE_RETURN)) p.button |= PAD_START;
  if (key(SDL_SCANCODE_Q)) { p.button |= PAD_L; p.trig_l = 255; }
  if (key(SDL_SCANCODE_W)) { p.button |= PAD_R; p.trig_r = 255; }
  if (key(SDL_SCANCODE_E)) p.button |= PAD_Z;
  if (key(SDL_SCANCODE_T)) p.button |= PAD_UP; if (key(SDL_SCANCODE_G)) p.button |= PAD_DOWN;
  if (key(SDL_SCANCODE_F)) p.button |= PAD_LEFT; if (key(SDL_SCANCODE_H)) p.button |= PAD_RIGHT;
  // First connected SDL game controller maps onto player 1 alongside the keyboard.
  SDL_GameController* c = nullptr;
  for (auto* p2 : g_pads) if (p2) { c = p2; break; }
  if (c) {
    auto axis = [](Sint16 v) { int a = v / 258; return a > 127 ? 127 : a < -127 ? -127 : a; };
    Sint16 lx = SDL_GameControllerGetAxis(c, SDL_CONTROLLER_AXIS_LEFTX), ly = SDL_GameControllerGetAxis(c, SDL_CONTROLLER_AXIS_LEFTY);
    Sint16 rx = SDL_GameControllerGetAxis(c, SDL_CONTROLLER_AXIS_RIGHTX), ry = SDL_GameControllerGetAxis(c, SDL_CONTROLLER_AXIS_RIGHTY);
    if (abs(lx) > 7849 || abs(ly) > 7849) { sx = axis(lx); sy = -axis(ly); }
    if (abs(rx) > 8689 || abs(ry) > 8689) { cx = axis(rx); cy = -axis(ry); }
    if (SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_A)) p.button |= PAD_A;
    if (SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_B)) p.button |= PAD_B;
    if (SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_X)) p.button |= PAD_X;
    if (SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_Y)) p.button |= PAD_Y;
    if (SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_START)) p.button |= PAD_START;
    if (SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_RIGHTSHOULDER)) p.button |= PAD_Z;
    if (SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_DPAD_UP)) p.button |= PAD_UP;
    if (SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_DPAD_DOWN)) p.button |= PAD_DOWN;
    if (SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_DPAD_LEFT)) p.button |= PAD_LEFT;
    if (SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_DPAD_RIGHT)) p.button |= PAD_RIGHT;
    int lt = SDL_GameControllerGetAxis(c, SDL_CONTROLLER_AXIS_TRIGGERLEFT) >> 7;
    int rt = SDL_GameControllerGetAxis(c, SDL_CONTROLLER_AXIS_TRIGGERRIGHT) >> 7;
    if (lt > 30) { p.trig_l = (uint8_t)(lt > 255 ? 255 : lt); if (lt > 200) p.button |= PAD_L; }
    if (rt > 30) { p.trig_r = (uint8_t)(rt > 255 ? 255 : rt); if (rt > 200) p.button |= PAD_R; }
  }
  p.stick_x = (int8_t)sx; p.stick_y = (int8_t)sy; p.sub_x = (int8_t)cx; p.sub_y = (int8_t)cy;
  return p;
}
}  // namespace

void input_mark_match_start() { g_match_start_retrace.store(retrace_count()); }

bool input_load_script(const char* path) {
  FILE* f = fopen(path, "r");
  if (!f) return false;
  char line[256];
  while (fgets(line, sizeof line, f)) {
    ScriptEntry e{};
    char* p = line;
    if (*p == '#' || *p == '\n' || *p == '\r') continue;
    if (!strncmp(p, "@match", 6)) { g_script_relative_section = true; continue; }
    if (!strncmp(p, "@loop", 5)) { g_script_loop = (uint32_t)strtoul(p + 5, nullptr, 10); continue; }
    e.relative = g_script_relative_section;
    e.frame = (uint32_t)strtoul(p, &p, 10);
    while (*p) {
      while (*p == ' ' || *p == '\t') ++p;
      if (!*p || *p == '\n' || *p == '\r' || *p == '#') break;
      char tok[32]; int n = 0;
      while (*p && *p != ' ' && *p != '+' && *p != '\n' && *p != '\r' && n < 31) tok[n++] = *p++;
      tok[n] = 0;
      if (*p == '+') ++p;
      if (!strcmp(tok, "A")) e.buttons |= PAD_A; else if (!strcmp(tok, "B")) e.buttons |= PAD_B;
      else if (!strcmp(tok, "X")) e.buttons |= PAD_X; else if (!strcmp(tok, "Y")) e.buttons |= PAD_Y;
      else if (!strcmp(tok, "Z")) e.buttons |= PAD_Z; else if (!strcmp(tok, "L")) e.buttons |= PAD_L;
      else if (!strcmp(tok, "R")) e.buttons |= PAD_R; else if (!strcmp(tok, "START")) e.buttons |= PAD_START;
      else if (!strcmp(tok, "DU")) e.buttons |= PAD_UP; else if (!strcmp(tok, "DD")) e.buttons |= PAD_DOWN;
      else if (!strcmp(tok, "DL")) e.buttons |= PAD_LEFT; else if (!strcmp(tok, "DR")) e.buttons |= PAD_RIGHT;
      else if (!strncmp(tok, "sx=", 3)) e.sx = (int8_t)atoi(tok + 3); else if (!strncmp(tok, "sy=", 3)) e.sy = (int8_t)atoi(tok + 3);
      else if (!strncmp(tok, "cx=", 3)) e.cx = (int8_t)atoi(tok + 3); else if (!strncmp(tok, "cy=", 3)) e.cy = (int8_t)atoi(tok + 3);
      else if (!strncmp(tok, "p=", 2)) { e.port = atoi(tok + 2) - 1; if (e.port < 0 || e.port > 3) e.port = 0; g_script_ports |= 1u << e.port; }
    }
    g_script.push_back(e);
  }
  fclose(f);
  return !g_script.empty();
}

void input_poll(PadState out[4]) {
  struct UiSnapshot {
    PadState* pads; bool gamecube = false;
    ~UiSnapshot() {
      std::lock_guard<std::mutex> lock(g_ui_pad_mutex); g_ui_pad = pads[0]; g_ui_gamecube = gamecube;
      if (g_ui_capture.load()) { pads[0] = {}; pads[0].err = 0; }
    }
  } ui{out};
  for (int i = 0; i < 4; ++i) { std::memset(&out[i], 0, sizeof out[i]); out[i].err = -1; }
  PadState& p = out[0];
  p.err = 0;
  if (!g_script.empty()) {
    uint32_t frame = retrace_count();
    uint32_t start = g_match_start_retrace.load();
    bool in_match = start && frame >= start;
    uint32_t rel = in_match ? frame - start : 0;
    if (in_match && g_script_loop) rel %= g_script_loop;
    for (int port = 0; port < 4; ++port) {
      if (port && !(g_script_ports & (1u << port))) continue;
      out[port].err = 0;
      const ScriptEntry* cur = nullptr;
      for (const ScriptEntry& e : g_script) {
        if (e.port != port) continue;
        if (e.relative) { if (in_match && e.frame <= rel) cur = &e; }
        else if (!in_match && e.frame <= frame) cur = &e;
      }
      if (cur) { PadState& q = out[port]; q.button = cur->buttons; q.stick_x = cur->sx; q.stick_y = cur->sy; q.sub_x = cur->cx; q.sub_y = cur->cy; }
    }
    return;
  }
  uint32_t adapter_mask = gcadapter_poll(out);
  ui.gamecube = (adapter_mask & 1u) != 0;
  if (adapter_mask & 1u) return;
  out[0] = PollKeyboard();
}

}  // namespace host

#endif  // !_MSC_VER
