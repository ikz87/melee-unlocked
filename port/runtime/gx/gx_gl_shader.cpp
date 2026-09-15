// HLSL -> GLSL translation for the GX shader generators.
//
// The D3D12 backend feeds `generate_vertex_shader`/`generate_pixel_shader` to D3DCompile. On the
// POSIX build we run the same generators but translate their output to GLSL, so the TEV combiner,
// vertex lighting and texgen stay derived from the one generator. The generated HLSL is a small,
// regular subset (vec/int types, cbuffers, Texture2D/SamplerState, SV_ semantics), so the
// translation is a token/type rewrite plus entry-point and I/O remapping rather than a compiler.
// SPDX-License-Identifier: GPL-2.0-or-later
#include "gx_gl_shader.h"
#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <string>
#include <vector>

namespace gx {
namespace {

bool ident(char c) { return std::isalnum((unsigned char)c) || c == '_'; }

std::string glsl_type(const std::string& t) {
  if (t == "float4") return "vec4";
  if (t == "float3") return "vec3";
  if (t == "float2") return "vec2";
  if (t == "int4") return "ivec4";
  if (t == "int3") return "ivec3";
  if (t == "int2") return "ivec2";
  if (t == "uint4") return "uvec4";
  if (t == "uint3") return "uvec3";
  if (t == "uint2") return "uvec2";
  return t;
}

std::string trim(const std::string& s) {
  size_t a = s.find_first_not_of(" \t\r\n");
  size_t b = s.find_last_not_of(" \t\r\n");
  return a == std::string::npos ? "" : s.substr(a, b - a + 1);
}

std::vector<std::string> split_top(const std::string& s, char sep) {
  std::vector<std::string> out;
  int depth = 0;
  size_t start = 0;
  for (size_t i = 0; i < s.size(); ++i) {
    char c = s[i];
    if (c == '(' || c == '[' || c == '{') ++depth;
    else if (c == ')' || c == ']' || c == '}') --depth;
    else if (c == sep && depth == 0) { out.push_back(s.substr(start, i - start)); start = i + 1; }
  }
  out.push_back(s.substr(start));
  return out;
}

struct Field { std::string qual, type, name, sem; };

// "float4 colors_0 : COLOR0" or "in float3 uv0 : TEXCOORD0".
bool parse_field(const std::string& raw, Field* f) {
  std::string s = trim(raw);
  size_t colon = s.find(':');
  if (colon == std::string::npos) return false;
  f->sem = trim(s.substr(colon + 1));
  while (!f->sem.empty() && (f->sem.back() == ';' || f->sem.back() == '\r')) f->sem.pop_back();
  f->sem = trim(f->sem);
  std::vector<std::string> toks = split_top(trim(s.substr(0, colon)), ' ');
  toks.erase(std::remove_if(toks.begin(), toks.end(), [](const std::string& t) { return t.empty(); }), toks.end());
  if (toks.size() == 3 && (toks[0] == "in" || toks[0] == "out" || toks[0] == "inout")) {
    f->qual = toks[0]; f->type = toks[1]; f->name = toks[2];
  } else if (toks.size() == 2) {
    f->type = toks[0]; f->name = toks[1];
  } else {
    return false;
  }
  return true;
}

int varying_location(const std::string& sem) {
  if (sem == "COLOR0") return 0;
  if (sem == "COLOR1") return 1;
  if (sem.rfind("TEXCOORD", 0) == 0) return 2 + std::atoi(sem.c_str() + 8);
  return -1;
}

int attr_location(const std::string& sem) {
  if (sem == "POSITION") return 0;
  if (sem == "NORMAL0") return 1;
  if (sem == "COLOR0") return 2;
  if (sem == "COLOR1") return 3;
  if (sem.rfind("TEXCOORD", 0) == 0) return 4 + std::atoi(sem.c_str() + 8);
  if (sem == "BLENDINDICES") return 12;
  if (sem == "BLENDINDICES1") return 13;
  return -1;
}

void replace_token(std::string& s, const std::string& from, const std::string& to) {
  size_t pos = 0;
  while ((pos = s.find(from, pos)) != std::string::npos) {
    bool lb = pos == 0 || !ident(s[pos - 1]);
    bool rb = pos + from.size() >= s.size() || !ident(s[pos + from.size()]);
    if (lb && rb) { s.replace(pos, from.size(), to); pos += to.size(); }
    else pos += from.size();
  }
}

void replace_first(std::string& s, const std::string& from, const std::string& to) {
  size_t p = s.find(from);
  if (p != std::string::npos) s.replace(p, from.size(), to);
}

void erase_line(std::string& s, const std::string& trimmed_line) {
  size_t pos = 0;
  while (pos < s.size()) {
    size_t eol = s.find('\n', pos);
    size_t end = eol == std::string::npos ? s.size() : eol + 1;
    if (trim(s.substr(pos, end - pos)) == trimmed_line) { s.erase(pos, end - pos); return; }
    pos = end;
  }
}

void erase_lines_prefix(std::string& s, const std::string& prefix) {
  size_t pos = 0;
  while (pos < s.size()) {
    size_t eol = s.find('\n', pos);
    size_t end = eol == std::string::npos ? s.size() : eol + 1;
    if (trim(s.substr(pos, end - pos)).rfind(prefix, 0) == 0) { s.erase(pos, end - pos); continue; }
    pos = end;
  }
}

std::string replace_samples(const std::string& in) {
  std::string out;
  size_t i = 0;
  while (i < in.size()) {
    size_t p = in.find("Tex[", i);
    if (p == std::string::npos) { out.append(in, i, std::string::npos); break; }
    out.append(in, i, p - i);
    size_t rb = in.find(']', p);
    // Only a real texture fetch: `Tex[N].Sample(`, never the `Tex[8]` declaration.
    if (rb == std::string::npos || in.compare(rb + 1, 8, ".Sample(") != 0) {
      out.append(in, p, 4); i = p + 4; continue;
    }
    std::string idx = trim(in.substr(p + 4, rb - (p + 4)));
    size_t open = rb + 1 + 7;   // '(' of Sample
    int depth = 0; size_t comma = std::string::npos, close = std::string::npos;
    for (size_t k = open + 1; k < in.size(); ++k) {
      char c = in[k];
      if (c == '(' || c == '[') ++depth;
      else if (c == ')' || c == ']') { if (depth == 0) { close = k; break; } --depth; }
      else if (c == ',' && depth == 0 && comma == std::string::npos) comma = k;
    }
    if (comma == std::string::npos || close == std::string::npos) { out.append(in, p, 4); i = p + 4; continue; }
    out += "texture(Tex[" + idx + "], " + trim(in.substr(comma + 1, close - comma - 1)) + ")";
    i = close + 1;
  }
  return out;
}

void apply_types(std::string& s) {
  replace_token(s, "wu4", "ivec4");
  replace_token(s, "wu3", "ivec3");
  replace_token(s, "wu2", "ivec2");
  replace_token(s, "wu", "int");
  replace_token(s, "uint4", "uvec4");
  replace_token(s, "uint3", "uvec3");
  replace_token(s, "uint2", "uvec2");
  replace_token(s, "float4", "vec4");
  replace_token(s, "float3", "vec3");
  replace_token(s, "float2", "vec2");
  replace_token(s, "int4", "ivec4");
  replace_token(s, "int3", "ivec3");
  replace_token(s, "int2", "ivec2");
}

// Text between the matching parentheses starting at `open`.
std::string paren_body(const std::string& s, size_t open, size_t* close) {
  int depth = 0;
  for (size_t k = open; k < s.size(); ++k) {
    if (s[k] == '(') ++depth;
    else if (s[k] == ')') { if (--depth == 0) { *close = k; return s.substr(open + 1, k - open - 1); } }
  }
  *close = std::string::npos;
  return {};
}

}  // namespace

std::string hlsl_vertex_to_glsl(const std::string& hlsl) {
  std::string s = hlsl;
  replace_first(s, "cbuffer VSBlock : register(b0) {", "layout(std140, binding = 0) uniform VSBlock {");

  std::string out_decls;
  size_t sb = s.find("struct VS_OUTPUT {");
  if (sb != std::string::npos) {
    size_t lb = s.find('{', sb);
    size_t rb = s.find("};", lb);
    if (lb != std::string::npos && rb != std::string::npos) {
      std::string body = s.substr(lb + 1, rb - (lb + 1));
      for (const std::string& line : split_top(body, '\n')) {
        Field f;
        if (!parse_field(line, &f)) continue;
        if (f.sem == "SV_Position") continue;
        out_decls += "layout(location = " + std::to_string(varying_location(f.sem)) + ") out " + glsl_type(f.type) + " " + f.name + ";\n";
      }
      s.erase(sb, (rb + 2) - sb);
    }
  }

  size_t mp = s.find("VS_OUTPUT main(");
  if (mp != std::string::npos) {
    size_t open = s.find('(', mp), close = 0;
    std::string params = paren_body(s, open, &close);
    std::string in_decls;
    for (const std::string& piece : split_top(params, ',')) {
      Field f;
      if (!parse_field(piece, &f)) continue;
      in_decls += "layout(location = " + std::to_string(attr_location(f.sem)) + ") in " + glsl_type(f.type) + " " + f.name + ";\n";
    }
    s.replace(mp, close + 1 - mp, in_decls + out_decls + "void main()");
  }

  replace_token(s, "o.pos", "gl_Position");
  erase_line(s, "VS_OUTPUT o;");
  erase_line(s, "return o;");
  size_t dot;
  while ((dot = s.find("o.")) != std::string::npos) s.erase(dot, 2);

  apply_types(s);
  return std::string("#version 450 core\n") + s;
}

std::string hlsl_pixel_to_glsl(const std::string& hlsl) {
  std::string s = hlsl;
  erase_lines_prefix(s, "#define wu");
  erase_lines_prefix(s, "SamplerState");
  replace_first(s, "Texture2D Tex[8] : register(t0);", "uniform sampler2D Tex[8];");
  replace_first(s, "cbuffer PSBlock : register(b1) {", "layout(std140, binding = 1) uniform PSBlock {");
  erase_lines_prefix(s, "[earlydepthstencil]");

  size_t mp = s.find("void main(");
  if (mp != std::string::npos) {
    size_t open = s.find('(', mp), close = 0;
    std::string params = paren_body(s, open, &close);
    std::string decls = "layout(location = 0) out vec4 ocol0;\n";
    std::string local_decls;
    bool uses_rawpos = false;
    for (const std::string& piece : split_top(params, ',')) {
      Field f;
      if (!parse_field(piece, &f)) continue;
      if (f.qual == "out") continue;   // ocol0 declared above
      if (f.sem == "SV_Position") { uses_rawpos = true; continue; }
      // GLSL shader inputs are read-only but the TEV code writes uv0/clipPos, so alias them.
      decls += "layout(location = " + std::to_string(varying_location(f.sem)) + ") in " + glsl_type(f.type) + " in_" + f.name + ";\n";
      local_decls += glsl_type(f.type) + " " + f.name + " = in_" + f.name + ";\n";
    }
    if (uses_rawpos) local_decls += "vec4 rawpos = gl_FragCoord;\n";
    s.replace(mp, close + 1 - mp, "void main()");
    s.insert(mp, decls + "\n");
    size_t brace = s.find('{', mp);
    if (brace != std::string::npos && !local_decls.empty()) s.insert(brace + 1, "\n" + local_decls + "\n");
  }

  s = replace_samples(s);
  apply_types(s);
  return std::string("#version 450 core\n") + s;
}

}  // namespace gx
