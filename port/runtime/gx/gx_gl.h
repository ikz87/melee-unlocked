// OpenGL backend for captured GX frames (POSIX build).
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "gx_d3d12.h"   // D3D12Options / SubFrameMode are the shared renderer options

namespace gx {

// Create an OpenGL 4.5 core backend drawing into `sdl_window` (an SDL_Window*).
Backend* create_gl_backend(void* sdl_window, int client_w, int client_h, const D3D12Options& options);
void gl_resize(Backend* backend, int w, int h);
void gl_stats(Backend* backend, uint32_t* frames_presented, uint32_t* pipelines, uint32_t* textures);

}  // namespace gx
