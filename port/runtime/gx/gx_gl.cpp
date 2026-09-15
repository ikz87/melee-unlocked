// OpenGL 4.5 backend for captured GX frames (POSIX build).
//
// This mirrors the D3D12 backend: the same captured draw stream, the same generated GX shaders
// (translated to GLSL by gx_gl_shader.cpp, so the TEV combiner, vertex lighting and texgen come
// from the one generator), pipeline state, EFB target, texture decode/upload and the letterboxed
// present. EFB-to-texture copies and DLSS are not implemented yet.
// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef _MSC_VER
#include "gx_gl.h"
#include "gx_core.h"
#include "gx_gl_shader.h"
#include "gx_shader.h"
#include "gx_texture.h"
#include "host.h"
#include <SDL.h>
#include <GL/glew.h>
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace gx {
namespace {

uint32_t bits(uint32_t v, int start, int len) { return (v >> start) & ((1u << len) - 1); }

const char* kVS = R"(#version 450 core
layout(std140, binding = 0) uniform VSBlock {
  vec4 projection[4];
  vec4 depthparams;
  vec4 viewparams;
  vec4 materials[4];
  vec4 lights[40];
  vec4 texmatrices[24];
  vec4 transformmatrices[64];
  vec4 normalmatrices[32];
  vec4 posttransformmatrices[64];
  vec4 unjittered_projection[4];
  vec4 prev_projection[4];
  vec4 prev_transformmatrices[64];
};
layout(location=0) in vec3 a_pos;
layout(location=1) in vec3 a_nrm;
layout(location=2) in vec4 a_col0;
layout(location=3) in vec4 a_col1;
layout(location=4) in vec2 a_uv0;
layout(location=5) in vec2 a_uv1;
layout(location=6) in vec2 a_uv2;
layout(location=7) in vec2 a_uv3;
layout(location=8) in vec2 a_uv4;
layout(location=9) in vec2 a_uv5;
layout(location=10) in vec2 a_uv6;
layout(location=11) in vec2 a_uv7;
layout(location=12) in uvec4 a_blend;
uniform uint u_components;
out vec4 v_col0, v_col1;
out vec2 v_uv[8];
void main() {
  int posmtx = int(a_blend.x);
  vec4 rawpos4 = vec4(a_pos, 1.0);
  vec4 pos = vec4(dot(transformmatrices[posmtx], rawpos4),
                  dot(transformmatrices[posmtx+1], rawpos4),
                  dot(transformmatrices[posmtx+2], rawpos4), 1.0);
  // Match the generated shader: colour 0 defaults to white when the vertex format omits it.
  v_col0 = ((u_components & 0x2000u) != 0u) ? a_col0 : vec4(1.0);
  v_col1 = ((u_components & 0x4000u) != 0u) ? a_col1 : v_col0;
  v_uv[0]=a_uv0; v_uv[1]=a_uv1; v_uv[2]=a_uv2; v_uv[3]=a_uv3;
  v_uv[4]=a_uv4; v_uv[5]=a_uv5; v_uv[6]=a_uv6; v_uv[7]=a_uv7;
  gl_Position = vec4(dot(projection[0], pos), dot(projection[1], pos),
                     dot(projection[2], pos), dot(projection[3], pos));
  gl_Position.z = gl_Position.w * depthparams.x - gl_Position.z * depthparams.y;
  gl_Position.xy *= sign(depthparams.zw * vec2(-1.0, 1.0));
  gl_Position.xy = gl_Position.xy + gl_Position.w * depthparams.zw;
  if (gl_Position.w == 1.0)
    gl_Position.xy = round(gl_Position.xy * viewparams.xy) * viewparams.zw;
}
)";

const char* kFS = R"(#version 450 core
in vec4 v_col0, v_col1;
in vec2 v_uv[8];
layout(binding = 0) uniform sampler2D u_tex0;
uniform int u_textured;
uniform vec2 u_texdims;
layout(location=0) out vec4 o_color;
void main() {
  vec4 c = v_col0;
  if (u_textured != 0) c *= texture(u_tex0, v_uv[0]);
  o_color = c;
}
)";

const char* kPresentVS = R"(#version 450 core
out vec2 v_uv;
void main() {
  vec2 p = vec2((gl_VertexID << 1) & 2, gl_VertexID & 2);
  v_uv = p;
  gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
}
)";

const char* kPresentFS = R"(#version 450 core
in vec2 v_uv;
layout(binding = 0) uniform sampler2D u_efb;
layout(location=0) out vec4 o_color;
void main() { o_color = vec4(texture(u_efb, v_uv).rgb, 1.0); }
)";

// EFB -> texture copy. The destination is stored with row 0 at the top of the copied region so it
// samples like a decoded RAM texture (the GL EFB itself is bottom-up), hence the flipped T.
const char* kCopyVS = R"(#version 450 core
out vec2 v_uv;
void main() {
  vec2 p = vec2((gl_VertexID << 1) & 2, gl_VertexID & 2);
  v_uv = p;
  gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
}
)";

const char* kCopyFS = R"(#version 450 core
in vec2 v_uv;
layout(binding = 0) uniform sampler2D u_efb;
uniform vec4 u_rect;   // src_x/W, src_y/H, src_w/W, src_h/H (game top-left origin)
layout(location=0) out vec4 o_color;
void main() {
  vec2 uv = vec2(u_rect.x + v_uv.x * u_rect.z, 1.0 - (u_rect.y + v_uv.y * u_rect.w));
  o_color = texture(u_efb, uv);
}
)";

GLuint compile(GLenum type, const char* src) {
  GLuint s = glCreateShader(type);
  glShaderSource(s, 1, &src, nullptr);
  glCompileShader(s);
  GLint ok = 0; glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
  if (!ok) {
    char log[2048]; GLsizei n = 0; glGetShaderInfoLog(s, sizeof log, &n, log);
    host::log("gl: shader compile failed: %s", log);
  }
  return s;
}

GLuint link_program(const char* vs, const char* fs) {
  GLuint v = compile(GL_VERTEX_SHADER, vs), f = compile(GL_FRAGMENT_SHADER, fs);
  GLuint p = glCreateProgram();
  glAttachShader(p, v); glAttachShader(p, f); glLinkProgram(p);
  GLint ok = 0; glGetProgramiv(p, GL_LINK_STATUS, &ok);
  if (!ok) { char log[2048]; GLsizei n = 0; glGetProgramInfoLog(p, sizeof log, &n, log); host::log("gl: link failed: %s", log); }
  glDeleteShader(v); glDeleteShader(f);
  return p;
}

class GLBackend final : public Backend {
 public:
  GLBackend(SDL_Window* window, int cw, int ch, const D3D12Options& o) : window_(window), opts_(o), client_w_(cw), client_h_(ch) { init(); }
  ~GLBackend() override { shutdown(); }

  void set_skip_present(bool skip) override { skip_present_ = skip; }
  void submit_frame(const Frame& frame) override { render(frame, nullptr); }
  void submit_frame(const Frame& frame, const DrawMatrices* overrides) override { render(frame, overrides); }
  void submit_and_recycle(Frame& frame) override { render(frame, nullptr); frame.clear(); }
  void resize(int w, int h) { client_w_ = w; client_h_ = h; }

 private:
  void init() {
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 4);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 5);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    context_ = SDL_GL_CreateContext(window_);
    if (!context_) { host::log("gl: cannot create GL context: %s", SDL_GetError()); return; }
    SDL_GL_MakeCurrent(window_, context_);
    glewExperimental = GL_TRUE;
    if (glewInit() != GLEW_OK) { host::log("gl: glewInit failed"); return; }
    glClipControl(GL_LOWER_LEFT, GL_ZERO_TO_ONE);
    glGenVertexArrays(1, &vao_); glBindVertexArray(vao_);
    glGenBuffers(1, &vbo_); glGenBuffers(1, &ibo_); glGenBuffers(1, &ubo_); glGenBuffers(1, &ubo_ps_);
    glBindBuffer(GL_UNIFORM_BUFFER, ubo_);
    glBufferData(GL_UNIFORM_BUFFER, sizeof(VSConstants), nullptr, GL_STREAM_DRAW);
    glBindBufferBase(GL_UNIFORM_BUFFER, 0, ubo_);
    glBindBuffer(GL_UNIFORM_BUFFER, ubo_ps_);
    glBufferData(GL_UNIFORM_BUFFER, sizeof(PSConstants), nullptr, GL_STREAM_DRAW);
    glBindBufferBase(GL_UNIFORM_BUFFER, 1, ubo_ps_);
    // 1x1 white for texture units a shader does not use.
    glGenTextures(1, &white_tex_); glBindTexture(GL_TEXTURE_2D, white_tex_);
    const uint8_t white[4] = {255, 255, 255, 255};
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, white);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    program_ = link_program(kVS, kFS);
    present_program_ = link_program(kPresentVS, kPresentFS);
    copy_program_ = link_program(kCopyVS, kCopyFS);
    u_copy_rect_ = glGetUniformLocation(copy_program_, "u_rect");
    u_textured_ = glGetUniformLocation(program_, "u_textured");
    u_texdims_ = glGetUniformLocation(program_, "u_texdims");
    u_components_ = glGetUniformLocation(program_, "u_components");
    const GLsizei stride = (GLsizei)sizeof(Vertex);
    glBindBuffer(GL_ARRAY_BUFFER, vbo_);        // attribute pointers below capture this buffer
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ibo_);  // and the element buffer is VAO state
    auto attr = [&](GLuint loc, GLint size, GLenum type, bool normalized, size_t offset) {
      glEnableVertexAttribArray(loc);
      glVertexAttribPointer(loc, size, type, normalized ? GL_TRUE : GL_FALSE, stride, (const void*)offset);
    };
    attr(0, 3, GL_FLOAT, false, offsetof(Vertex, pos));
    attr(1, 3, GL_FLOAT, false, offsetof(Vertex, nrm));
    attr(2, 4, GL_UNSIGNED_BYTE, true, offsetof(Vertex, col0));
    attr(3, 4, GL_UNSIGNED_BYTE, true, offsetof(Vertex, col1));
    for (int i = 0; i < 8; ++i) attr(4 + i, 2, GL_FLOAT, false, offsetof(Vertex, uv) + i * 2 * sizeof(float));
    // Integer blend indices: posmtx + texmtx[0..2], then texmtx[3..6] (matches the D3D input layout).
    glEnableVertexAttribArray(12);
    glVertexAttribIPointer(12, 4, GL_UNSIGNED_BYTE, stride, (const void*)offsetof(Vertex, posmtx));
    glEnableVertexAttribArray(13);
    glVertexAttribIPointer(13, 4, GL_UNSIGNED_BYTE, stride, (const void*)(offsetof(Vertex, posmtx) + 4));
    create_efb();
  }

  void shutdown() {
    if (context_) { SDL_GL_DeleteContext(context_); context_ = nullptr; }
  }

  void create_efb() {
    scale_ = opts_.efb_scale > 0 ? opts_.efb_scale : 2;
    widescreen_ = opts_.widescreen;
    efb_w_ = EFB_WIDTH * scale_;
    efb_h_ = EFB_HEIGHT * scale_;
    glGenFramebuffers(1, &efb_fbo_); glBindFramebuffer(GL_FRAMEBUFFER, efb_fbo_);
    glGenTextures(1, &efb_color_); glBindTexture(GL_TEXTURE_2D, efb_color_);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, efb_w_, efb_h_, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, efb_color_, 0);
    glGenRenderbuffers(1, &efb_depth_); glBindRenderbuffer(GL_RENDERBUFFER, efb_depth_);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, efb_w_, efb_h_);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, efb_depth_);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) host::log("gl: EFB framebuffer incomplete");
    glGenFramebuffers(1, &copy_fbo_);
    glViewport(0, 0, efb_w_, efb_h_);
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
  }

  // EFB copies feed later draws (mirrors, shadows, screen effects). Keyed by guest dest address,
  // like the D3D12 backend's efb_copies_ map.
  struct EfbEntry { GLuint tex = 0; uint32_t width = 0, height = 0; uint64_t last_used = 0; };

  GLuint get_texture(const DrawCall& dc, const TextureRef& t, uint32_t* w, uint32_t* h) {
    auto ec = efb_copies_.find(t.addr);
    if (ec != efb_copies_.end() && ec->second.tex) {
      ec->second.last_used = frames_;
      *w = ec->second.width; *h = ec->second.height;
      return ec->second.tex;
    }
    if (!t.used || !t.data) return 0;
    const uint32_t meta[] = {t.width, t.height, t.format, t.mip_levels, t.tlut_format};
    uint64_t key = t.data->hash ^ hash_bytes(meta, sizeof meta);
    auto it = textures_.find(key);
    if (it != textures_.end()) { *w = t.width; *h = t.height; return it->second; }
    std::vector<uint8_t> rgba;
    decode_texture(t.data->image.data(), t.width, t.height, t.format, t.data->palette.data(), t.tlut_format, rgba);
    GLuint tex = 0;
    glGenTextures(1, &tex); glBindTexture(GL_TEXTURE_2D, tex);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, t.width, t.height, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba.data());
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
    textures_[key] = tex;
    *w = t.width; *h = t.height;
    return tex;
  }

  void execute_copy(const EfbCopy& c) {
    uint32_t region_w = c.src_w, region_h = c.src_h;
    if (c.half_scale) { region_w = std::max(1u, region_w / 2); region_h = std::max(1u, region_h / 2); }
    uint32_t sw = region_w * (uint32_t)scale_, sh = region_h * (uint32_t)scale_;
    EfbEntry& e = efb_copies_[c.dest_addr];
    if (!e.tex || e.width != sw || e.height != sh) {
      if (e.tex) glDeleteTextures(1, &e.tex);
      glGenTextures(1, &e.tex); glBindTexture(GL_TEXTURE_2D, e.tex);
      glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, sw, sh, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
      e.width = sw; e.height = sh;
    }
    e.last_used = frames_;
    glBindFramebuffer(GL_FRAMEBUFFER, copy_fbo_);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, e.tex, 0);
    glViewport(0, 0, (GLsizei)sw, (GLsizei)sh);
    glDisable(GL_SCISSOR_TEST); glDisable(GL_DEPTH_TEST); glDisable(GL_BLEND); glDisable(GL_CULL_FACE);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glUseProgram(copy_program_);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, efb_color_);
    glUniform4f(u_copy_rect_, (float)c.src_x / EFB_WIDTH, (float)c.src_y / EFB_HEIGHT,
                (float)c.src_w / EFB_WIDTH, (float)c.src_h / EFB_HEIGHT);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    if (efb_copies_.size() > 64) evict_copies();
  }

  void evict_copies() {
    std::vector<std::pair<uint64_t, uint32_t>> ages;
    ages.reserve(efb_copies_.size());
    for (auto& kv : efb_copies_) ages.push_back({kv.second.last_used, kv.first});
    std::sort(ages.begin(), ages.end());
    for (size_t i = 0; i < 16 && i < ages.size(); ++i) {
      auto it = efb_copies_.find(ages[i].second);
      if (it != efb_copies_.end()) { if (it->second.tex) glDeleteTextures(1, &it->second.tex); efb_copies_.erase(it); }
    }
  }

  void execute_draw(const Frame& frame, const DrawCall& dc, const DrawMatrices* overrides) {
    uint32_t n = dc.vertex_count;
    if (!n) return;
    std::vector<uint32_t> idx;
    GLenum mode = GL_TRIANGLES;
    switch (dc.primitive) {
      case 0x80: case 0x88: for (uint32_t i = 0; i + 3 < n; i += 4) { idx.insert(idx.end(), {i, i + 1, i + 2, i, i + 2, i + 3}); } break;
      case 0x90: for (uint32_t i = 0; i + 2 < n; i += 3) idx.insert(idx.end(), {i, i + 1, i + 2}); break;
      case 0x98: for (uint32_t i = 2; i < n; ++i) { if (i & 1) idx.insert(idx.end(), {i - 1, i - 2, i}); else idx.insert(idx.end(), {i - 2, i - 1, i}); } break;
      case 0xA0: for (uint32_t i = 2; i < n; ++i) idx.insert(idx.end(), {0, i - 1, i}); break;
      case 0xA8: mode = GL_LINES; for (uint32_t i = 0; i + 1 < n; i += 2) idx.insert(idx.end(), {i, i + 1}); break;
      case 0xB0: mode = GL_LINES; for (uint32_t i = 1; i < n; ++i) idx.insert(idx.end(), {i - 1, i}); break;
      default: return;
    }
    if (idx.empty()) return;

    const Vertex* vsrc = (overrides && overrides->vertices) ? overrides->vertices : &frame.vertices[dc.first_vertex];
    glBindBuffer(GL_ARRAY_BUFFER, vbo_);
    glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)((size_t)n * sizeof(Vertex)), vsrc, GL_STREAM_DRAW);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ibo_);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, (GLsizeiptr)(idx.size() * 4), idx.data(), GL_STREAM_DRAW);

    VSUid vsu = make_vs_uid(dc);
    PSUid psu = make_ps_uid(dc);
    GLuint program = get_program(vsu, psu);
    if (!program) program = program_;   // simple fallback when a generated shader fails to compile

    VSConstants vs_constants;
    fill_vs_constants(dc, vs_constants, scale_, overrides);
    glBindBuffer(GL_UNIFORM_BUFFER, ubo_);
    glBufferSubData(GL_UNIFORM_BUFFER, 0, sizeof vs_constants, &vs_constants);
    PSConstants ps_constants;
    fill_ps_constants(dc, ps_constants, scale_);
    glBindBuffer(GL_UNIFORM_BUFFER, ubo_ps_);
    glBufferSubData(GL_UNIFORM_BUFFER, 0, sizeof ps_constants, &ps_constants);

    // Viewport / scissor (D3D top-left origin -> GL bottom-left).
    const float* vp = (const float*)&dc.xf_regs[0x1A];
    float s = (float)scale_;
    float X = (vp[3] - vp[0] - 342.0f) * s, Y = (vp[4] + vp[1] - 342.0f) * s, W = 2.0f * vp[0] * s, H = -2.0f * vp[1] * s;
    if (W < 0) { X += W; W = -W; }
    if (H < 0) { Y += H; H = -H; }
    float min_depth = std::clamp(1.0f - vp[5] / 16777216.0f, 0.0f, 1.0f);
    float max_depth = std::clamp(1.0f - (vp[5] - vp[2]) / 16777216.0f, 0.0f, 1.0f);
    if (max_depth < min_depth) std::swap(min_depth, max_depth);
    glViewport((GLint)X, (GLint)(efb_h_ - (Y + H)), (GLsizei)std::max(W, 1.0f), (GLsizei)std::max(H, 1.0f));
    glDepthRange(min_depth, max_depth);

    uint32_t tl = dc.bp.reg[BP_SCISSORTL], br = dc.bp.reg[BP_SCISSORBR], so = dc.bp.reg[BP_SCISSOROFFSET];
    int xoff = (int)bits(so, 0, 10) * 2 - 342, yoff = (int)bits(so, 10, 10) * 2 - 342;
    int sl = (int)bits(tl, 12, 12) - xoff - 342, st = (int)bits(tl, 0, 12) - yoff - 342;
    int sr = (int)bits(br, 12, 12) - xoff - 341, sb = (int)bits(br, 0, 12) - yoff - 341;
    sl = std::clamp(sl, 0, EFB_WIDTH); sr = std::clamp(sr, 0, EFB_WIDTH);
    st = std::clamp(st, 0, EFB_HEIGHT); sb = std::clamp(sb, 0, EFB_HEIGHT);
    if (sr <= sl || sb <= st) return;
    glScissor(sl * scale_, efb_h_ - sb * scale_, (sr - sl) * scale_, (sb - st) * scale_);

    apply_blend(dc.bp.blendmode() & 0xFFFF, (dc.bp.zcontrol() & 7) == 1);
    apply_depth(dc.bp.zmode() & 0x1F);
    apply_cull(dc.bp.cullmode());

    glUseProgram(program);
    glUniform1ui(u_components_, dc.components);
    // The generated shader samples Tex[i]; bind each stage's decoded texture to unit i. Unused
    // stages get a 1x1 white so an unreferenced sampler is still complete. GX wrap/filter modes
    // come from the texture's BP registers (mode0).
    for (int i = 0; i < 8; ++i) {
      uint32_t tw = 0, th = 0;
      GLuint tex = get_texture(dc, dc.textures[i], &tw, &th);
      glActiveTexture(GL_TEXTURE0 + i);
      glBindTexture(GL_TEXTURE_2D, tex ? tex : white_tex_);
      if (tex) apply_sampler(dc.textures[i], efb_copies_.count(dc.textures[i].addr) != 0);
    }
    glActiveTexture(GL_TEXTURE0);
    glDrawElements(mode, (GLsizei)idx.size(), GL_UNSIGNED_INT, nullptr);
    ++draws_;
  }

  void apply_sampler(const TextureRef& t, bool is_copy) {
    // GX wrap: 0 clamp, 1 repeat, 2 mirror, 3 repeat (matches the D3D12 backend's table).
    static const GLint wrap[] = {GL_CLAMP_TO_EDGE, GL_REPEAT, GL_MIRRORED_REPEAT, GL_REPEAT};
    uint32_t m0 = t.mode0;
    GLint ws = is_copy ? GL_CLAMP_TO_EDGE : wrap[bits(m0, 0, 2)];
    GLint wt = is_copy ? GL_CLAMP_TO_EDGE : wrap[bits(m0, 2, 2)];
    bool mag_linear = is_copy || bits(m0, 4, 1);
    bool min_linear = is_copy || (bits(m0, 5, 3) & 4);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, ws);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, wt);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, mag_linear ? GL_LINEAR : GL_NEAREST);
    // Only level 0 is uploaded, so never select a mipmapped min filter (the texture would be
    // incomplete and sample black).
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, min_linear ? GL_LINEAR : GL_NEAREST);
    if (opts_.anisotropy > 1 && mag_linear && min_linear)
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_ANISOTROPY_EXT, std::clamp(opts_.anisotropy, 1, 16));
  }

  GLuint get_program(const VSUid& vsu, const PSUid& psu) {
    auto key = std::make_pair(vsu.hash(), psu.hash());
    auto it = programs_.find(key);
    if (it != programs_.end()) return it->second;
    std::string vs_glsl = hlsl_vertex_to_glsl(generate_vertex_shader(vsu));
    std::string ps_glsl = hlsl_pixel_to_glsl(generate_pixel_shader(psu));
    GLuint prog = link_program(vs_glsl.c_str(), ps_glsl.c_str());
    GLint ok = 0; glGetProgramiv(prog, GL_LINK_STATUS, &ok);
    if (!ok) { glDeleteProgram(prog); prog = 0; ++shader_failures_; }
    else {
      GLint loc = glGetUniformLocation(prog, "Tex");
      if (loc >= 0) { GLint units[8] = {0, 1, 2, 3, 4, 5, 6, 7}; glUseProgram(prog); glUniform1iv(loc, 8, units); }
    }
    programs_[key] = prog;
    return prog;
  }

  void apply_blend(uint32_t bm, bool alpha_in_efb) {
    bool enable = bits(bm, 0, 1);
    static const GLenum src_factors[] = {GL_ZERO, GL_ONE, GL_DST_COLOR, GL_ONE_MINUS_DST_COLOR, GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_DST_ALPHA, GL_ONE_MINUS_DST_ALPHA};
    static const GLenum dst_factors[] = {GL_ZERO, GL_ONE, GL_SRC_COLOR, GL_ONE_MINUS_SRC_COLOR, GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_DST_ALPHA, GL_ONE_MINUS_DST_ALPHA};
    GLenum sf = src_factors[bits(bm, 8, 3)], df = dst_factors[bits(bm, 5, 3)];
    if (!alpha_in_efb) {
      if (sf == GL_DST_ALPHA) sf = GL_ONE;
      if (sf == GL_ONE_MINUS_DST_ALPHA) sf = GL_ZERO;
      if (df == GL_DST_ALPHA) df = GL_ONE;
      if (df == GL_ONE_MINUS_DST_ALPHA) df = GL_ZERO;
    }
    if (enable) {
      glEnable(GL_BLEND);
      glBlendFuncSeparate(sf, df, GL_ONE, GL_ZERO);
      glBlendEquationSeparate(bits(bm, 11, 1) ? GL_FUNC_REVERSE_SUBTRACT : GL_FUNC_ADD, GL_FUNC_ADD);
    } else {
      glDisable(GL_BLEND);
    }
    bool color_mask = bits(bm, 3, 1), alpha_mask = bits(bm, 4, 1);
    glColorMask(color_mask, color_mask, color_mask, alpha_mask);
  }

  void apply_depth(uint32_t zm) {
    static const GLenum cmp[] = {GL_NEVER, GL_GREATER, GL_EQUAL, GL_GEQUAL, GL_LESS, GL_NOTEQUAL, GL_LEQUAL, GL_ALWAYS};
    bool enable = bits(zm, 0, 1);
    if (enable) {
      glEnable(GL_DEPTH_TEST);
      glDepthFunc(cmp[bits(zm, 1, 3)]);
      glDepthMask(bits(zm, 4, 1) ? GL_TRUE : GL_FALSE);
    } else {
      glDisable(GL_DEPTH_TEST); glDepthMask(GL_FALSE);
    }
  }

  void apply_cull(uint32_t cull) {
    static const GLenum modes[] = {0, GL_BACK, GL_FRONT, GL_BACK};
    GLenum m = modes[cull & 3];
    if (m) { glEnable(GL_CULL_FACE); glFrontFace(GL_CW); glCullFace(m); }
    else glDisable(GL_CULL_FACE);
  }

  void clear_efb(const EfbCopy& c) {
    if (!c.clear) return;
    glEnable(GL_SCISSOR_TEST);
    glScissor(c.src_x * scale_, efb_h_ - (c.src_y + c.src_h) * scale_, c.src_w * scale_, c.src_h * scale_);
    float color[4] = {((c.clear_color >> 16) & 0xFF) / 255.0f, ((c.clear_color >> 8) & 0xFF) / 255.0f,
                      (c.clear_color & 0xFF) / 255.0f, ((c.clear_color >> 24) & 0xFF) / 255.0f};
    glClearColor(color[0], color[1], color[2], color[3]);
    glDepthMask(GL_TRUE);
    glClearDepth(1.0 - (double)c.clear_z / 16777215.0);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glDisable(GL_SCISSOR_TEST);
  }

  void present_efb(const EfbCopy& c) {
    if (skip_present_ || !context_) return;
    SDL_GL_GetDrawableSize(window_, &client_w_, &client_h_);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glViewport(0, 0, client_w_, client_h_);
    glDisable(GL_SCISSOR_TEST); glDisable(GL_DEPTH_TEST); glDisable(GL_BLEND); glDisable(GL_CULL_FACE);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glClearColor(0.05f, 0.05f, 0.15f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    float aspect = widescreen_ ? 16.0f / 9.0f : 4.0f / 3.0f;
    float ww = (float)client_w_, wh = (float)client_h_;
    float vw = ww, vh = ww / aspect;
    if (vh > wh) { vh = wh; vw = wh * aspect; }
    glViewport((GLint)((ww - vw) * 0.5f), (GLint)((wh - vh) * 0.5f), (GLsizei)vw, (GLsizei)vh);
    glUseProgram(present_program_);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, efb_color_);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    capture_if_requested();
    SDL_GL_SwapWindow(window_);
    ++frames_;
  }

  void capture_if_requested() {
    if (opts_.capture_path.empty()) return;
    if (opts_.capture_every) {
      if (frames_ % opts_.capture_every) return;
    } else {
      if (opts_.capture_frame != 0 && frames_ != opts_.capture_frame) return;
      if (opts_.capture_frame == 0 && frames_ != 0) return;
    }
    std::vector<uint8_t> pixels((size_t)client_w_ * client_h_ * 3);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(0, 0, client_w_, client_h_, GL_RGB, GL_UNSIGNED_BYTE, pixels.data());
    std::string path = opts_.capture_path;
    if (opts_.capture_every) {
      std::string stem = path;
      const std::string ext = ".ppm";
      if (stem.size() > ext.size() && stem.compare(stem.size() - ext.size(), ext.size(), ext) == 0)
        stem.resize(stem.size() - ext.size());
      path = stem + "_" + std::to_string(frames_) + ".ppm";
    }
    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) { host::log("gl: cannot write capture %s", path.c_str()); return; }
    std::fprintf(f, "P6\n%d %d\n255\n", client_w_, client_h_);
    for (int y = client_h_ - 1; y >= 0; --y) std::fwrite(&pixels[(size_t)y * client_w_ * 3], 1, (size_t)client_w_ * 3, f);
    std::fclose(f);
    host::log("gl: captured frame %u to %s (%dx%d)", frames_, path.c_str(), client_w_, client_h_);
  }

  void render(const Frame& frame, const DrawMatrices* overrides) {
    if (!context_) return;
    if (frame.commands.empty()) return;
    glBindFramebuffer(GL_FRAMEBUFFER, efb_fbo_);
    glEnable(GL_SCISSOR_TEST);
    for (const FrameCommand& cmd : frame.commands) {
      if (cmd.kind == FrameCommand::Draw) {
        if (cmd.index < frame.draws.size()) execute_draw(frame, frame.draws[cmd.index], overrides);
      } else if (cmd.index < frame.copies.size()) {
        const EfbCopy& c = frame.copies[cmd.index];
        // Order matters and matches the D3D12 backend: present first, then apply this copy's clear.
        // (Melee issues the clear and the XFB copy together at the end of the frame.)
        if (c.to_xfb && !skip_present_) present_efb(c);
        else if (!c.to_xfb) execute_copy(c);
        glBindFramebuffer(GL_FRAMEBUFFER, efb_fbo_);
        if (c.clear) clear_efb(c);
      }
    }
  }

  SDL_Window* window_ = nullptr;
  SDL_GLContext context_ = nullptr;
  D3D12Options opts_;
  int client_w_, client_h_;
  int scale_ = 2;
  bool widescreen_ = false;
  bool skip_present_ = false;
  uint32_t efb_w_ = 0, efb_h_ = 0;
  GLuint vao_ = 0, vbo_ = 0, ibo_ = 0, ubo_ = 0, ubo_ps_ = 0, white_tex_ = 0;
  GLuint efb_fbo_ = 0, efb_color_ = 0, efb_depth_ = 0, copy_fbo_ = 0;
  GLuint program_ = 0, present_program_ = 0, copy_program_ = 0;
  GLint u_textured_ = -1;
  GLint u_texdims_ = -1;
  GLint u_components_ = -1;
  GLint u_copy_rect_ = -1;
  uint32_t shader_failures_ = 0;
  std::unordered_map<uint64_t, GLuint> textures_;
  std::unordered_map<uint32_t, EfbEntry> efb_copies_;
  std::map<std::pair<uint64_t, uint64_t>, GLuint> programs_;
  uint32_t frames_ = 0, draws_ = 0;
};

}  // namespace

Backend* create_gl_backend(void* sdl_window, int client_w, int client_h, const D3D12Options& options) {
  return new GLBackend(static_cast<SDL_Window*>(sdl_window), client_w, client_h, options);
}
void gl_resize(Backend* backend, int w, int h) { static_cast<GLBackend*>(backend)->resize(w, h); }
void gl_stats(Backend* backend, uint32_t* frames_presented, uint32_t* pipelines, uint32_t* textures) {
  (void)backend; if (frames_presented) *frames_presented = 0; if (pipelines) *pipelines = 0; if (textures) *textures = 0;
}

}  // namespace gx

#endif  // !_MSC_VER
