// PC settings loading for the POSIX build. The in-game overlay itself is D3D12/ImGui-Win32 only;
// on Linux the same port-settings.ini keys are honored but the ImGui panel is not shown.
// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef _MSC_VER
#include "pc_settings.h"
#include "jukebox.h"
#include <algorithm>
#include <fstream>
#include <string>

namespace gx {
void load_pc_settings(D3D12Options& options, int& volume) {
  std::ifstream file(options.settings_path);
  options.settings_open = false;
  std::string key, value;
  while (file >> key >> value) {
    try {
      if (key == "fps") { double rate = std::stod(value); if (rate == -1 || rate == 0 || (rate >= 30 && rate <= 2000)) options.fps_cap = rate; }
      else if (key == "scale") { int scale = std::stoi(value); if (scale >= 0 && scale <= 8) options.efb_scale = scale; }
      else if (key == "fullscreen") options.fullscreen = value == "1";
      else if (key == "vsync") options.vsync = value == "1";
      else if (key == "widescreen") options.widescreen = value == "1";
      else if (key == "sharpness") options.sharpness = std::clamp(std::stof(value), 0.0f, 1.0f);
      else if (key == "anisotropy") { int a = std::stoi(value); if (a == 1 || a == 2 || a == 4 || a == 8 || a == 16) options.anisotropy = a; }
      else if (key == "ssaa") { int a = std::stoi(value); if (a == 1 || a == 2) options.ssaa = a; }
      else if (key == "subframe") options.subframe = value == "0" ? SubFrameMode::Off : value == "2" ? SubFrameMode::AuthoredInterpolate : SubFrameMode::Authored;
      else if (key == "music") slippi::jukebox::set_user_volume(std::stoi(value));
      else if (key == "performance") options.performance_overlay = value == "1";
      else if (key == "startup") options.settings_open = value != "0";
      else if (key == "dlss") { int m = std::stoi(value); if (m >= 0 && m <= 5) options.dlss_mode = m; }
      else if (key == "volume") volume = std::clamp(std::stoi(value), 0, 100);
    } catch (...) { /* Ignore a malformed preference, retaining the safe default. */ }
  }
}
}  // namespace gx

#endif  // !_MSC_VER
