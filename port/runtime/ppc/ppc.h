// Guest CPU state and memory/float helpers for statically recompiled Gekko code.
// Float semantics mirror Slippi Dolphin's Jit64 (FMA path, g_want_determinism=false):
// see melee-unlocked\slippi\Source\Core\Core\PowerPC\Jit64\Jit_FloatingPoint.cpp.
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <cmath>
#include <cstdint>
#include <cstring>
#include <immintrin.h>
#ifdef _MSC_VER
#include <intrin.h>
#else
#include <byteswap.h>
#define _byteswap_ushort(x) bswap_16(x)
#define _byteswap_ulong(x) bswap_32(x)
#define _byteswap_uint64(x) bswap_64(x)
#endif

namespace ppc {

constexpr uint32_t RAM_BASE = 0x80000000u;
constexpr uint32_t RAM_SIZE = 0x01800000u;
constexpr uint32_t LC_BASE = 0xE0000000u;
constexpr uint32_t LC_SIZE = 0x4000u;

union FPR {
  struct { double ps0, ps1; };
  struct { uint64_t u0, u1; };
};

struct Context {
  uint32_t r[32];
  FPR f[32];
  uint8_t cr[8];       // per field: LT=8 GT=4 EQ=2 SO=1
  uint32_t lr, ctr;
  uint32_t ca, so, ov; // XER pieces
  uint32_t fpscr;
  uint32_t gqr[8];
  uint32_t msr;
  uint32_t hid0, hid2, dec;
  uint64_t tb;
  uint32_t spr[1024];  // anything else, by number
  uint32_t call_depth;
  uint32_t backedges;  // loop back-edge counter for event polling (see backedge)
  uint32_t last_pc;    // diagnostics only (function entry address)
  uint32_t entry;      // mid-function entry address requested by a dispatch thunk (0 = normal entry)
  uint32_t trace_pos;
  uint32_t trace[64];  // ring of recently entered functions (diagnostics)
};
// Loop back-edge poll: every 1024 iterations of any guest loop, let virtual time advance and
// deliver due host events (alarms, AI DMA frames, completions) if interrupts are enabled.
void loop_poll(Context& c);
inline void backedge(Context& c) { if ((++c.backedges & 0x3FFu) == 0) loop_poll(c); }
// Hang watchdog: every 2^20 function entries the host checks whether simulation time still
// advances (diagnostics for guest spin loops; see host::hang_check).
extern uint64_t g_enter_count;
extern bool g_trace_funcs;          // --trace-func: log entries of selected guest functions
void hang_check(Context& c);
void trace_enter(Context& c, uint32_t pc);

void add_trace_func(uint32_t addr, uint32_t limit);
inline void enter(Context& c, uint32_t pc) {
  c.last_pc = pc; c.trace[c.trace_pos++ & 63] = pc;
  if ((++g_enter_count & 0xFFFFFu) == 0) hang_check(c);
  if (g_trace_funcs) trace_enter(c, pc);
}
// MSR writes: when the guest re-enables external interrupts (EE), deliver pending host events,
// exactly where a real interrupt would have been taken.
void interrupts_enabled(Context& c);
inline void mtmsr(Context& c, uint32_t v) {
  bool enable = !(c.msr & 0x8000u) && (v & 0x8000u);
  c.msr = v;
  if (enable) interrupts_enabled(c);
}
inline bool interrupts_on(const Context& c) { return (c.msr & 0x8000u) != 0; }

using Fn = void (*)(Context&, uint8_t*);

// ---- runtime services implemented in ppc_runtime.cpp ----
void call(Context& c, uint8_t* m, uint32_t addr);         // indirect call by guest address
void interpret(Context& c, uint8_t* m, uint32_t addr);    // run RAM-resident code until it returns (interp.cpp)
void interpreter_stats(uint64_t* calls, uint64_t* insns);
void fatal(Context& c, const char* what, uint32_t a);
// __longjmp: thrown by the HLE, caught by the translated function that called __setjmp on
// `buf` (its body is wrapped in a retry loop; see Emitter). The catch restores the registers
// the MSL longjmp would and resumes at the setjmp return address saved in the buffer.
struct GuestLongJmp { uint32_t buf; uint32_t val; };
void longjmp_restore(Context& c, uint8_t* m, uint32_t buf, uint32_t val);
uint32_t mmio_read(Context& c, uint32_t ea, int bytes);
void mmio_write(Context& c, uint32_t ea, uint32_t value, int bytes);
uint64_t mmio_read64(Context& c, uint32_t ea);
void mmio_write64(Context& c, uint32_t ea, uint64_t value);
uint32_t spr_read(Context& c, uint32_t n);
void spr_write(Context& c, uint32_t n, uint32_t v);
void syscall(Context& c, uint8_t* m);
void update_mxcsr(Context& c);
uint8_t* locked_cache();

// Timebase reads advance time slightly so guest delay loops (OSGetTime polling) terminate.
inline uint64_t read_tb(Context& c) { c.tb += 32; return c.tb; }

// ---- memory ----
// Host pointer for a guest address in RAM, or nullptr when it needs the slow path.
inline uint8_t* fast(uint8_t* m, uint32_t ea) {
  uint32_t off = ea & 0x3FFFFFFFu;
  return off < RAM_SIZE ? m + off : nullptr;
}
inline uint8_t* slowptr(uint32_t ea) {
  if ((ea & 0xFFFFC000u) == LC_BASE) return locked_cache() + (ea & (LC_SIZE - 1));
  return nullptr;
}
inline uint32_t ld8(Context& c, uint8_t* m, uint32_t ea) {
  if (uint8_t* p = fast(m, ea)) return *p;
  if (uint8_t* p = slowptr(ea)) return *p;
  return mmio_read(c, ea, 1);
}
inline uint32_t ld16(Context& c, uint8_t* m, uint32_t ea) {
  if (uint8_t* p = fast(m, ea)) { uint16_t v; std::memcpy(&v, p, 2); return _byteswap_ushort(v); }
  if (uint8_t* p = slowptr(ea)) { uint16_t v; std::memcpy(&v, p, 2); return _byteswap_ushort(v); }
  return mmio_read(c, ea, 2);
}
inline uint32_t ld32(Context& c, uint8_t* m, uint32_t ea) {
  if (uint8_t* p = fast(m, ea)) { uint32_t v; std::memcpy(&v, p, 4); return _byteswap_ulong(v); }
  if (uint8_t* p = slowptr(ea)) { uint32_t v; std::memcpy(&v, p, 4); return _byteswap_ulong(v); }
  return mmio_read(c, ea, 4);
}
inline uint64_t ld64(Context& c, uint8_t* m, uint32_t ea) {
  if (uint8_t* p = fast(m, ea)) { uint64_t v; std::memcpy(&v, p, 8); return _byteswap_uint64(v); }
  if (uint8_t* p = slowptr(ea)) { uint64_t v; std::memcpy(&v, p, 8); return _byteswap_uint64(v); }
  return mmio_read64(c, ea);
}
inline void st8(Context& c, uint8_t* m, uint32_t ea, uint32_t v) {
  if (uint8_t* p = fast(m, ea)) { *p = (uint8_t)v; return; }
  if (uint8_t* p = slowptr(ea)) { *p = (uint8_t)v; return; }
  mmio_write(c, ea, v & 0xFF, 1);
}
inline void st16(Context& c, uint8_t* m, uint32_t ea, uint32_t v) {
  uint16_t s = _byteswap_ushort((uint16_t)v);
  if (uint8_t* p = fast(m, ea)) { std::memcpy(p, &s, 2); return; }
  if (uint8_t* p = slowptr(ea)) { std::memcpy(p, &s, 2); return; }
  mmio_write(c, ea, v & 0xFFFF, 2);
}
inline void st32(Context& c, uint8_t* m, uint32_t ea, uint32_t v) {
  uint32_t s = _byteswap_ulong(v);
  if (uint8_t* p = fast(m, ea)) { std::memcpy(p, &s, 4); return; }
  if (uint8_t* p = slowptr(ea)) { std::memcpy(p, &s, 4); return; }
  mmio_write(c, ea, v, 4);
}
inline void st64(Context& c, uint8_t* m, uint32_t ea, uint64_t v) {
  uint64_t s = _byteswap_uint64(v);
  if (uint8_t* p = fast(m, ea)) { std::memcpy(p, &s, 8); return; }
  if (uint8_t* p = slowptr(ea)) { std::memcpy(p, &s, 8); return; }
  mmio_write64(c, ea, v);
}
// Byte-reversed forms read the guest bytes in host (little-endian) order.
inline uint32_t ld32r(Context& c, uint8_t* m, uint32_t ea) { return _byteswap_ulong(ld32(c, m, ea)); }
inline uint32_t ld16r(Context& c, uint8_t* m, uint32_t ea) { return _byteswap_ushort((uint16_t)ld16(c, m, ea)); }
inline void st32r(Context& c, uint8_t* m, uint32_t ea, uint32_t v) { st32(c, m, ea, _byteswap_ulong(v)); }
inline void st16r(Context& c, uint8_t* m, uint32_t ea, uint32_t v) { st16(c, m, ea, _byteswap_ushort((uint16_t)v)); }
void dcbz(Context& c, uint8_t* m, uint32_t ea);
void lswi(Context& c, uint8_t* m, uint32_t ea, uint32_t rd, uint32_t nb);
void stswi(Context& c, uint8_t* m, uint32_t ea, uint32_t rs, uint32_t nb);
void psq_load(Context& c, uint8_t* m, uint32_t ea, uint32_t rd, uint32_t w, uint32_t i);
void psq_store(Context& c, uint8_t* m, uint32_t ea, uint32_t rs, uint32_t w, uint32_t i);

// ---- condition register ----
inline void cr_set_s(Context& c, int field, int32_t a, int32_t b) {
  c.cr[field] = (uint8_t)((a < b ? 8 : a > b ? 4 : 2) | c.so);
}
inline void cr_set_u(Context& c, int field, uint32_t a, uint32_t b) {
  c.cr[field] = (uint8_t)((a < b ? 8 : a > b ? 4 : 2) | c.so);
}
inline void cr0(Context& c, uint32_t v) { cr_set_s(c, 0, (int32_t)v, 0); }
inline uint32_t mfcr(const Context& c) {
  uint32_t v = 0;
  for (int i = 0; i < 8; ++i) v |= (uint32_t)(c.cr[i] & 15) << (28 - 4 * i);
  return v;
}
inline void mtcrf(Context& c, uint32_t crm, uint32_t v) {
  for (int i = 0; i < 8; ++i)
    if (crm & (0x80 >> i)) c.cr[i] = (uint8_t)((v >> (28 - 4 * i)) & 15);
}
inline uint32_t crbit(const Context& c, int bit) { return (c.cr[bit >> 2] >> (3 - (bit & 3))) & 1; }
inline void crbit_set(Context& c, int bit, uint32_t v) {
  uint8_t mask = (uint8_t)(8 >> (bit & 3));
  c.cr[bit >> 2] = (uint8_t)(v ? (c.cr[bit >> 2] | mask) : (c.cr[bit >> 2] & ~mask));
}
inline uint32_t mask(int mb, int me) {
  uint32_t begin = 0xFFFFFFFFu >> mb, end = 0x7FFFFFFFu >> me, m = begin ^ end;
  return me < mb ? ~m : m;
}
inline uint32_t carry(uint32_t a, uint32_t b) { return b > ~a; }
inline uint32_t cntlzw(uint32_t v) {
#ifdef _MSC_VER
  unsigned long i; return _BitScanReverse(&i, v) ? 31 - i : 32;
#else
  return v ? (uint32_t)__builtin_clz(v) : 32u;
#endif
}
inline uint32_t divw(int32_t a, int32_t b) {
  if (b == 0 || ((uint32_t)a == 0x80000000u && b == -1)) return (a < 0 && b == 0) ? 0xFFFFFFFFu : 0;
  return (uint32_t)(a / b);
}
inline uint32_t divwu(uint32_t a, uint32_t b) { return b ? a / b : 0; }
inline uint32_t sraw(Context& c, uint32_t rs, uint32_t rb) {
  if (rb & 0x20) { c.ca = (rs & 0x80000000u) ? 1 : 0; return c.ca ? 0xFFFFFFFFu : 0; }
  int amount = rb & 31;
  if (!amount) { c.ca = 0; return rs; }
  int32_t s = (int32_t)rs;
  c.ca = (s < 0 && (uint32_t)(s << (32 - amount))) ? 1 : 0;
  return (uint32_t)(s >> amount);
}
inline uint32_t srawi(Context& c, uint32_t rs, int amount) {
  if (!amount) { c.ca = 0; return rs; }
  int32_t s = (int32_t)rs;
  c.ca = (s < 0 && (uint32_t)(s << (32 - amount))) ? 1 : 0;
  return (uint32_t)(s >> amount);
}

// ---- floating point (Jit64 semantics) ----
inline double fs(double x) { return (double)(float)x; }
inline double f25(double d) {
  uint64_t i; std::memcpy(&i, &d, 8);
  i = (i & 0xFFFFFFFFF8000000ull) + (i & 0x8000000ull);
  std::memcpy(&d, &i, 8); return d;
}
// Hardware FMA, exactly as Jit64 emits VFMADD/VFMSUB/VFNMADD/VFNMSUB (scalar double).
inline double fmadd(double a, double c, double b) {
  return _mm_cvtsd_f64(_mm_fmadd_sd(_mm_set_sd(a), _mm_set_sd(c), _mm_set_sd(b)));
}
inline double fmsub(double a, double c, double b) {
  return _mm_cvtsd_f64(_mm_fmsub_sd(_mm_set_sd(a), _mm_set_sd(c), _mm_set_sd(b)));
}
inline double fnmadd(double a, double c, double b) {  // PPC fnmadd = -(a*c + b) = VFNMSUB
  return _mm_cvtsd_f64(_mm_fnmsub_sd(_mm_set_sd(a), _mm_set_sd(c), _mm_set_sd(b)));
}
inline double fnmsub(double a, double c, double b) {  // PPC fnmsub = -(a*c - b) = VFNMADD
  return _mm_cvtsd_f64(_mm_fnmadd_sd(_mm_set_sd(a), _mm_set_sd(c), _mm_set_sd(b)));
}
double fres(double v);
double frsqrte(double v);
inline uint64_t fctiw(double b, bool truncate) {
  uint32_t v;
  if (std::isnan(b)) v = 0x80000000u;
  else {
    if (b > 2147483647.0) b = 2147483647.0;
    if (b < -2147483648.0) v = 0x80000000u;
    else v = (uint32_t)(int32_t)(truncate ? std::trunc(b) : std::nearbyint(b));
  }
  return 0xFFF8000000000000ull | v;
}
inline void fcmp(Context& c, int field, double a, double b) {
  c.cr[field] = (uint8_t)((a != a || b != b) ? 1 : a < b ? 8 : a > b ? 4 : 2);
}
inline double bits_to_double(uint64_t u) { double d; std::memcpy(&d, &u, 8); return d; }
inline uint64_t double_to_bits(double d) { uint64_t u; std::memcpy(&u, &d, 8); return u; }
inline double float_bits_to_double(uint32_t u) { float f; std::memcpy(&f, &u, 4); return (double)f; }
inline uint32_t double_to_float_bits(double d) { float f = (float)d; uint32_t u; std::memcpy(&u, &f, 4); return u; }

}  // namespace ppc
