// HLSL -> GLSL translation for the GX shader generators.
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <string>

namespace gx {
std::string hlsl_vertex_to_glsl(const std::string& hlsl);
std::string hlsl_pixel_to_glsl(const std::string& hlsl);
}  // namespace gx
