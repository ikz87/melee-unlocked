// Host services: memory, disc, boot, event delivery, time, MMIO, logging.
// SPDX-License-Identifier: GPL-2.0-or-later
#include "host.h"
#include "memory_range.h"
#include "compat.h"
#ifdef _MSC_VER
#include <windows.h>
#include <bcrypt.h>
#endif
#include "functions.h"
#include "guest_symbols.h"
#include "gx_core.h"
#include "window.h"
#include "ax_ucode.h"
#include "exi_slippi.h"
#include "gecko_data.h"
#include <chrono>
#include <mutex>
#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <deque>
#include <thread>

namespace guest {
struct NameEntry { uint32_t addr; const char* name; };
extern const NameEntry name_table[];
extern const size_t name_table_count;
}
namespace hle { void audio_tick(bool force); }

namespace hle { void dvd_poll(); }
namespace host {

Options options;
uint8_t* ram = nullptr;
uint8_t* aram = nullptr;
ppc::Context* cpu = nullptr;

static FILE* g_disc = nullptr;
static FILE* g_state_trace = nullptr;
static uint32_t g_fst_offset, g_fst_size, g_fst_max;
static std::deque<Completion> g_completions;
static bool g_pe_finish_pending = false;
static bool g_pe_token_pending = false;
static uint16_t g_pe_token = 0;
static uint32_t g_retraces = 0;
static std::atomic<bool> g_exit{false};
static std::atomic<int> g_exit_code{0};
static std::chrono::steady_clock::time_point g_next_frame;
static uint8_t g_mmio[0x10000];      // 0xCC000000 - 0xCC00FFFF register file (big-endian bytes)
static bool g_in_interrupt = false;

// ---------------- logging ----------------
// Every line also goes to melee_port.log in the working directory (truncated at start), so a play
// session can be inspected afterwards without the console window.
static FILE* g_log_file = nullptr;
static void open_log_file() {
  static bool tried = false;
  if (tried) return;
  tried = true;
  g_log_file = std::fopen(options.log_file.empty() ? "melee_port.log" : options.log_file.c_str(), "w");
}
void log(const char* fmt, ...) {
  if (options.quiet) return;
  open_log_file();
  va_list ap; va_start(ap, fmt);
  std::vfprintf(stdout, fmt, ap);
  va_end(ap);
  std::fputc('\n', stdout);
  std::fflush(stdout);
  if (g_log_file) {
    va_list ap2; va_start(ap2, fmt);
    std::vfprintf(g_log_file, fmt, ap2);
    va_end(ap2);
    std::fputc('\n', g_log_file);
    std::fflush(g_log_file);
  }
}

void log_guest_text(const char* data, size_t len) {
  std::fwrite(data, 1, len, stdout);
  std::fflush(stdout);
  if (g_log_file) { std::fwrite(data, 1, len, g_log_file); std::fflush(g_log_file); }
}

[[noreturn]] void die(const char* fmt, ...) {
  va_list ap; va_start(ap, fmt);
  std::fprintf(stderr, "\nFATAL: ");
  std::vfprintf(stderr, fmt, ap);
  std::fprintf(stderr, "\n");
  va_end(ap);
  if (g_log_file) {
    va_list ap2; va_start(ap2, fmt);
    std::fprintf(g_log_file, "\nFATAL: ");
    std::vfprintf(g_log_file, fmt, ap2);
    std::fprintf(g_log_file, "\n");
    va_end(ap2);
    std::fflush(g_log_file);
  }
  std::fflush(stderr);
  std::fflush(stdout);
  std::exit(3);
}

const char* symbol_name(uint32_t addr) {
  // Binary search the sorted function name table for the containing function.
  size_t lo = 0, hi = guest::name_table_count;
  while (lo < hi) {
    size_t mid = (lo + hi) / 2;
    if (guest::name_table[mid].addr <= addr) lo = mid + 1; else hi = mid;
  }
  if (lo == 0) return "?";
  return guest::name_table[lo - 1].name;
}

// ---------------- memory ----------------
uint8_t* ptr(uint32_t addr, uint32_t bytes) {
  uint32_t off = addr & 0x3FFFFFFFu;
  if (!valid_range(off, bytes, ppc::RAM_SIZE)) die("host access outside RAM: %08X+%X", addr, bytes);
  return ram + off;
}
uint32_t rd32(uint32_t a) { uint32_t v; std::memcpy(&v, ptr(a, 4), 4); return _byteswap_ulong(v); }
uint16_t rd16(uint32_t a) { uint16_t v; std::memcpy(&v, ptr(a, 2), 2); return _byteswap_ushort(v); }
uint8_t rd8(uint32_t a) { return *ptr(a); }
void wr32(uint32_t a, uint32_t v) { v = _byteswap_ulong(v); std::memcpy(ptr(a, 4), &v, 4); }
void wr16(uint32_t a, uint16_t v) { v = _byteswap_ushort(v); std::memcpy(ptr(a, 2), &v, 2); }
void wr8(uint32_t a, uint8_t v) { *ptr(a) = v; }
std::string cstr(uint32_t addr, size_t max) {
  std::string s;
  for (size_t i = 0; i < max; ++i) { char ch = (char)rd8(addr + (uint32_t)i); if (!ch) break; s += ch; }
  return s;
}

// ---------------- disc ----------------
bool disc_open(const std::string& path) {
  g_disc = std::fopen(path.c_str(), "rb");
  if (!g_disc) return false;
  uint8_t hdr[0x440];
  if (!disc_read(0, hdr, sizeof hdr)) return false;
  auto be = [&](int o) { return ((uint32_t)hdr[o] << 24) | ((uint32_t)hdr[o + 1] << 16) | ((uint32_t)hdr[o + 2] << 8) | hdr[o + 3]; };
  g_fst_offset = be(0x424);
  g_fst_size = be(0x428);
  g_fst_max = be(0x42C);
  if (std::memcmp(hdr, "GALE01", 6) != 0) log("warning: disc id is not GALE01");
  return true;
}
uint64_t g_disc_reads = 0, g_disc_bytes = 0;
static std::mutex g_disc_mutex;   // the DVD worker and the simulation thread share the file
bool disc_read(uint32_t offset, void* dst, uint32_t size) {
  std::lock_guard<std::mutex> lk(g_disc_mutex);
  if (!g_disc) return false;
  if (_fseeki64(g_disc, offset, SEEK_SET) != 0) return false;
  ++g_disc_reads;
  g_disc_bytes += size;
  return std::fread(dst, 1, size, g_disc) == size;
}
uint32_t disc_fst_offset() { return g_fst_offset; }
uint32_t disc_fst_size() { return g_fst_size; }

// Looks a file up by name in the disc's FST (root and nested directories; exact match first,
// then case-insensitive). Used to serve ISO files to host-side loaders (Slippi game files).
bool disc_find_file(const std::string& name, uint32_t* offset, uint32_t* size) {
  static std::vector<uint8_t> fst;
  if (fst.empty()) {
    if (!g_fst_size) return false;
    fst.resize(g_fst_size);
    if (!disc_read(g_fst_offset, fst.data(), g_fst_size)) { fst.clear(); return false; }
  }
  auto be32 = [&](size_t o) { return o + 4 <= fst.size() ? ((uint32_t)fst[o] << 24) | ((uint32_t)fst[o + 1] << 16) | ((uint32_t)fst[o + 2] << 8) | fst[o + 3] : 0u; };
  uint32_t entries = be32(8);
  size_t strings = (size_t)entries * 12;
  if (strings > fst.size()) return false;
  for (int pass = 0; pass < 2; ++pass) {
    for (uint32_t i = 1; i < entries; ++i) {
      uint32_t a = be32(i * 12);
      if (a >> 24) continue;   // directory
      size_t so = strings + (a & 0xFFFFFF);
      if (so >= fst.size()) continue;
      const char* n = (const char*)&fst[so];
      size_t maxlen = fst.size() - so;
      bool match = pass == 0 ? (std::strncmp(n, name.c_str(), maxlen) == 0) : (_strnicmp(n, name.c_str(), maxlen) == 0);
      if (match && std::strlen(n) == name.size()) {
        if (offset) *offset = be32(i * 12 + 4);
        if (size) *size = be32(i * 12 + 8);
        return true;
      }
    }
  }
  return false;
}
uint32_t disc_fst_max_size() { return g_fst_max; }

// ---------------- boot ----------------
// Portable SHA-1 used to verify the DOL on every platform (Windows previously used BCrypt).
static void sha1(const uint8_t* msg, size_t len, uint8_t out[20]) {
  uint32_t h0 = 0x67452301, h1 = 0xEFCDAB89, h2 = 0x98BADCFE, h3 = 0x10325476, h4 = 0xC3D2E1F0;
  auto rol = [](uint32_t x, int n) { return (x << n) | (x >> (32 - n)); };
  std::vector<uint8_t> data(msg, msg + len);
  uint64_t bitlen = (uint64_t)len * 8;
  data.push_back(0x80);
  while (data.size() % 64 != 56) data.push_back(0);
  for (int i = 7; i >= 0; --i) data.push_back((uint8_t)(bitlen >> (i * 8)));
  for (size_t off = 0; off < data.size(); off += 64) {
    uint32_t w[80];
    for (int i = 0; i < 16; ++i)
      w[i] = ((uint32_t)data[off + i * 4] << 24) | ((uint32_t)data[off + i * 4 + 1] << 16) |
             ((uint32_t)data[off + i * 4 + 2] << 8) | data[off + i * 4 + 3];
    for (int i = 16; i < 80; ++i) w[i] = rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
    uint32_t a = h0, b = h1, c = h2, d = h3, e = h4;
    for (int i = 0; i < 80; ++i) {
      uint32_t f, k;
      if (i < 20) { f = (b & c) | ((~b) & d); k = 0x5A827999; }
      else if (i < 40) { f = b ^ c ^ d; k = 0x6ED9EBA1; }
      else if (i < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8F1BBCDC; }
      else { f = b ^ c ^ d; k = 0xCA62C1D6; }
      uint32_t t = rol(a, 5) + f + e + k + w[i];
      e = d; d = c; c = rol(b, 30); b = a; a = t;
    }
    h0 += a; h1 += b; h2 += c; h3 += d; h4 += e;
  }
  uint32_t hs[5] = {h0, h1, h2, h3, h4};
  for (int i = 0; i < 5; ++i) {
    out[i * 4] = (uint8_t)(hs[i] >> 24); out[i * 4 + 1] = (uint8_t)(hs[i] >> 16);
    out[i * 4 + 2] = (uint8_t)(hs[i] >> 8); out[i * 4 + 3] = (uint8_t)hs[i];
  }
}

static void load_dol_from_disc() {
  uint8_t hdr[0x20];
  if (!disc_read(0x420, hdr, 4)) die("cannot read disc DOL offset");
  uint32_t dol_offset = ((uint32_t)hdr[0] << 24) | ((uint32_t)hdr[1] << 16) | ((uint32_t)hdr[2] << 8) | hdr[3];
  constexpr uint32_t dol_size = 0x4385E0u;
  std::vector<uint8_t> image(dol_size);
  if (!disc_read(dol_offset, image.data(), dol_size)) die("cannot read full Melee DOL");
  uint8_t digest[20];
  const uint8_t expected[20] = {0x08,0xe0,0xbf,0x20,0x13,0x4d,0xfc,0xb2,0x60,0x69,0x96,0x71,0x00,0x45,0x27,0xb2,0xd6,0xbb,0x1a,0x45};
  sha1(image.data(), dol_size, digest);
  if (std::memcmp(digest, expected, sizeof digest))
    die("ISO DOL does not match vanilla Melee NTSC 1.02; recompiled code cannot run this image");
  uint8_t dh[0x100];
  if (!disc_read(dol_offset, dh, sizeof dh)) die("cannot read DOL header");
  auto be = [&](int o) { return ((uint32_t)dh[o] << 24) | ((uint32_t)dh[o + 1] << 16) | ((uint32_t)dh[o + 2] << 8) | dh[o + 3]; };
  for (int i = 0; i < 18; ++i) {
    uint32_t off = be(i * 4), addr = be(0x48 + i * 4), size = be(0x90 + i * 4);
    if (!size) continue;
    if (!disc_read(dol_offset + off, ptr(addr, size), size)) die("cannot read DOL section %d", i);
  }
  // The DOL header's bss range overlaps the loaded .sdata section; the guest's own
  // __init_data zeroes .bss/.sbss precisely and RAM starts zeroed, so do not memset here.
  log("boot: DOL loaded from disc offset %08X, bss %08X+%X, entry %08X", dol_offset, be(0xD8), be(0xDC), be(0xE0));
}

// Reproduces Slippi Dolphin's boot-time Gecko installation in guest RAM: codehandler.bin at
// 0x80001800, bootloader.gct at 0x800028B8, then the effect of running the handler once (its
// 32-bit writes and the C2 hook branches into the caves inside the table). The recompiled code
// already contains these patches; this keeps RAM identical to what the game expects to read.
static void install_gecko_boot() {
  if (!gecko::codehandler_bin_size) { log("boot: translated without Slippi code tables"); return; }
  std::memcpy(ptr(0x80001800u, (uint32_t)gecko::codehandler_bin_size), gecko::codehandler_bin, gecko::codehandler_bin_size);
  wr32(0x80001D6Cu, 0x4E800020u);   // USB Gecko I/O replaced by blr, as Slippi does
  wr32(0x80001800u, 0xD01F1BADu);   // handler magic
  std::memcpy(ptr(0x800028B8u, (uint32_t)gecko::bootloader_gct_size), gecko::bootloader_gct, gecko::bootloader_gct_size);
  wr8(0x80001807u, 1);              // codes on
  for (size_t i = 0; i < gecko::boot_writes_count; ++i) {
    const gecko::Write& w = gecko::boot_writes[i];
    std::memcpy(ptr(w.addr, w.size), w.data, w.size);
  }
  for (size_t i = 0; i < gecko::boot_hooks_count; ++i) {
    const gecko::HookInstall& h = gecko::boot_hooks[i];
    wr32(h.hook, 0x48000000u | ((h.cave_addr - h.hook) & 0x03FFFFFCu));
    uint32_t last = h.cave_addr + (h.words - 1) * 4;
    wr32(last, 0x48000000u | (((h.hook + 4) - last) & 0x03FFFFFCu));
  }
  slippi::init();
  log("boot: Slippi code tables installed (%zu boot writes, %zu boot hooks, main GCT %zu bytes served over EXI)",
      gecko::boot_writes_count, gecko::boot_hooks_count, gecko::slippi_gct_size);
}

void boot_setup() {
  if (!options.state_trace.empty()) {
    g_state_trace = std::fopen(options.state_trace.c_str(), "w");
    if (!g_state_trace) die("cannot open state trace");
    std::fprintf(g_state_trace, "retrace,cpu,ram,aram,events\n");
  }
  ram = (uint8_t*)std::calloc(ppc::RAM_SIZE + 64, 1);
  aram = (uint8_t*)std::calloc(0x01000000, 1);
  ax::set_memory({rd16, rd32, wr16, wr32, aram, 0x01000000});
  ax::reset();
  cpu = new ppc::Context();
  std::memset(cpu, 0, sizeof *cpu);
  if (!ram || !aram) die("out of memory");
  std::memset(g_mmio, 0, sizeof g_mmio);

  load_dol_from_disc();

  // Low memory, mirroring Dolphin's Boot_BS2Emu.cpp (GC path) plus what the apploader leaves.
  disc_read(0, ptr(0x80000000, 0x20), 0x20);              // disc id
  wr32(0x80000020, 0x0D15EA5E);                      // booted from bootrom
  wr32(0x80000028, ppc::RAM_SIZE);                   // physical memory size
  wr32(0x8000002C, 0x10000006);                      // console type (Dolphin reports devkit)
  wr32(0x80000030, 0);                               // arena lo (0 = use linker default)
  wr32(0x800000CC, 0);                               // NTSC
  wr32(0x800000D0, 0x01000000);                      // ARAM size
  wr32(0x800000F0, ppc::RAM_SIZE);                   // simulated memory size
  wr32(0x800000F8, 0x09A7EC80);                      // bus clock
  wr32(0x800000FC, 0x1CF7C580);                      // cpu clock
  wr32(0x80000300, 0x4C000064);                      // rfi stubs
  wr32(0x80000800, 0x4C000064);
  wr32(0x80000C00, 0x4C000064);
  uint64_t tb = options.time_base;
  if (!tb) {
    // Dolphin presets the timebase from the RTC (seconds since GC epoch 2000-01-01) * 40.5 MHz.
    auto now = std::chrono::system_clock::now().time_since_epoch();
    uint64_t secs = (uint64_t)std::chrono::duration_cast<std::chrono::seconds>(now).count();
    const uint64_t GC_EPOCH = 946684800ull;
    tb = (secs - GC_EPOCH) * TB_HZ;
  }
  // Like Dolphin: the timebase register starts near zero; 0x800030D8 holds the epoch adjust
  // that __OSGetSystemTime adds to mftb.
  wr32(0x800030D8, (uint32_t)(tb >> 32));
  wr32(0x800030DC, (uint32_t)tb);
  cpu->tb = 0;

  // Apploader: FST at the top of RAM, arena hi below it.
  if (!valid_range(0, g_fst_max, ppc::RAM_SIZE) || g_fst_size > g_fst_max) die("invalid FST size");
  uint32_t fst_addr = (0x81800000u - g_fst_max) & ~31u;
  if (!disc_read(g_fst_offset, ptr(fst_addr, g_fst_size), g_fst_size)) die("cannot read FST");
  wr32(0x80000038, fst_addr);
  wr32(0x8000003C, g_fst_max);
  wr32(0x80000034, fst_addr);                        // arena hi
  log("boot: FST %u bytes at %08X (max %X), arena hi %08X", g_fst_size, fst_addr, g_fst_max, fst_addr);
  install_gecko_boot();

  cpu->msr = 0x00002030u | 0x8000u;                  // FP | DR | IR | EE
  cpu->fpscr = 0;
  ppc::update_mxcsr(*cpu);
  g_next_frame = std::chrono::steady_clock::now();
}

// ---------------- guest calls from host ----------------
void call_guest(uint32_t addr, uint32_t r3, uint32_t r4, uint32_t r5, uint32_t r6) {
  ppc::Context& c = *cpu;
  uint32_t saved_lr = c.lr;
  c.r[3] = r3; c.r[4] = r4; c.r[5] = r5; c.r[6] = r6;
  c.lr = 0;
  ppc::call(c, ram, addr);
  c.lr = saved_lr;
}

// ---------------- events ----------------
void post_completion(Completion fn) { g_completions.push_back(std::move(fn)); }
void set_pe_finish_pending() { g_pe_finish_pending = true; }
void set_pe_token_pending(uint16_t token) { g_pe_token = token; g_pe_token_pending = true; }
bool exit_requested() { return g_exit; }
void request_exit(int code) { g_exit_code.store(code); g_exit.store(true); }
int exit_code() { return g_exit_code.load(); }
uint32_t retrace_count() { return g_retraces; }
// The VI retrace is periodic in virtual time, like the hardware interrupt: `g_next_retrace_tb`
// is the timebase value of the next retrace. A sleeping guest (wait_event) jumps time straight
// to that boundary; a guest that busy-waits with interrupts enabled advances time in small steps
// at HLE entry points and loop polls and takes the retrace when it crosses the boundary (Slippi's
// lag-reduction code waits for pad data that the retrace path produces).
static uint64_t g_next_retrace_tb = TB_PER_FRAME;
static bool g_in_retrace = false;
void advance_time(uint64_t ticks) { cpu->tb += ticks; }
static void advance_frame() {
  if (cpu->tb < g_next_retrace_tb) cpu->tb = g_next_retrace_tb;   // idle: jump to the boundary
  g_next_retrace_tb += TB_PER_FRAME;
}

void deliver_interrupt(uint32_t number) {
  // __OSInterruptHandlerTable lives at 0x80003040 (OS_INTERRUPTTABLE_ADDR).
  uint32_t handler = rd32(0x80003040u + number * 4);
  if (!handler) return;
  uint32_t context = rd32(0x800000D4u);  // OS current context (virtual address)
  ppc::Context& c = *cpu;
  ppc::Context saved = c;                // handlers clobber registers; restore like an rfi would
  bool was = g_in_interrupt;
  g_in_interrupt = true;
  try {
    call_guest(handler, number, context);
  } catch (const LoadContextUnwind&) {
  }
  g_in_interrupt = was;
  uint64_t tb = c.tb;
  c = saved;
  c.tb = tb;
  ppc::update_mxcsr(c);
}

static void fire_due_alarms(bool force);
static bool deliver_completions(bool force);

static void validate_alarm_queue(const char* where);
void pump_completions() {
  // Called from HLE entry points the guest polls. Virtual time flows a little so periodic
  // alarms (pad sampling) fire even in loops that never sleep. Nothing is delivered while the
  // guest has interrupts disabled; ppc::mtmsr flushes when they come back on.
  advance_time(2048);
  hle::dvd_poll();
  validate_alarm_queue("hle entry");
  if (!ppc::interrupts_on(*cpu)) return;
  fire_due_alarms(false);
  hle::audio_tick(false);
  deliver_completions(false);
  if (cpu->tb >= g_next_retrace_tb && !g_in_retrace) retrace();   // periodic VI interrupt during busy waits
}

// Diagnostic: the OSAlarm queue must only ever link alarms whose handlers are code. A corrupt
// link is reported at the first HLE entry after it appears so the call trace points at the writer.
static void validate_alarm_queue(const char* where) {
  static bool reported = false;
  if (reported) return;
  uint32_t a = rd32(gs::AlarmQueue), prev = 0;
  for (int guard = 0; a && guard < 64; ++guard) {
    bool bad = a < 0x80003000u || a >= 0x81800000u;
    uint32_t handler = bad ? 0 : rd32(a);
    // Handlers live in the DOL's text or in the Slippi code table caves; nothing else is code.
    bool code = (handler >= 0x80003100u && handler < 0x803B7240u) || (handler >= 0x8065C000u && handler < 0x8071B000u);
    if (!bad) bad = !code || (handler & 3) || rd32(a + 16) != prev;
    if (bad) {
      reported = true;
      log("ALARM QUEUE CORRUPT (%s): entry %08X handler %08X prev %08X (expected %08X) next %08X head %08X tail %08X retrace %u",
          where, a, handler, bad && a >= 0x80003000u && a < 0x81800000u ? rd32(a + 16) : 0, prev, a >= 0x80003000u && a < 0x81800000u ? rd32(a + 20) : 0,
          rd32(gs::AlarmQueue), rd32(gs::AlarmQueue + 4), g_retraces);
      ppc::fatal(*cpu, "alarm queue corrupt", a);
      return;
    }
    prev = a; a = rd32(a + 20);
  }
}

static void fire_due_alarms(bool force) {
  // OSAlarm queue head lives in the SDK's static AlarmQueue; fire through the installed
  // decrementer exception handler (OSExceptionTable[8] at 0x80003000 + 8*4) so the guest's
  // own callback logic runs. The handler expects an exception frame; the asm wrapper just
  // saves GPRs into the context and tail-calls DecrementerExceptionCallback, which processes
  // one alarm and re-arms periodic ones, so loop while the head is due.
  static bool firing = false;
  if (firing) return;
  if (!force && !ppc::interrupts_on(*cpu)) return;
  firing = true;
  for (int guard = 0; guard < 16; ++guard) {
    validate_alarm_queue(guard ? "after previous alarm handler" : "entry");
    uint32_t head = rd32(gs::AlarmQueue);
    if (!head) break;
    uint64_t fire = ((uint64_t)rd32(head + 8) << 32) | rd32(head + 12);
    // Alarm times are OS system time: timebase + the adjust at 0x800030D8.
    uint64_t adjust = ((uint64_t)rd32(0x800030D8u) << 32) | rd32(0x800030DCu);
    if ((int64_t)fire > (int64_t)(cpu->tb + adjust)) break;
    uint32_t handler = rd32(0x80003000u + 8 * 4);
    if (!handler) break;
    uint32_t context = rd32(0x800000D4u);
    static int reported = 0;
    if (options.trace_calls && reported++ < 40)
      log("[alarm] head=%08X fire=%llu tb=%llu handler=%08X cb=%08X period=%llu", head, fire, cpu->tb,
          handler, rd32(head), ((uint64_t)rd32(head + 24) << 32) | rd32(head + 28));
    ppc::Context& c = *cpu;
    ppc::Context saved = c;
    try {
      call_guest(handler, 8, context);
    } catch (const LoadContextUnwind&) {
    }
    uint64_t tb = c.tb;
    c = saved;
    c.tb = tb;
    ppc::update_mxcsr(c);
    static char where[64];
    std::snprintf(where, sizeof where, "after alarm %08X handler %08X", head, rd32(head));
    validate_alarm_queue(where);
  }
  firing = false;
}

bool g_has_window = false;

// Field-wise CPU hash excludes C++ padding and diagnostic counters/trace history.
static void trace_state() {
  if (!g_state_trace) return;
  uint64_t h = 0;
  auto add = [&](const auto& v) { h = (h ^ gx::hash_bytes(&v, sizeof v)) * 0x100000001b3ull; };
  const auto& c = *cpu;
  add(c.r); add(c.f); add(c.cr); add(c.lr); add(c.ctr);
  add(c.ca); add(c.so); add(c.ov); add(c.fpscr); add(c.gqr);
  add(c.msr); add(c.hid0); add(c.hid2); add(c.dec); add(c.tb); add(c.spr);
  uint64_t events = (uint64_t)g_completions.size() << 32 |
      (uint64_t)g_pe_token << 8 | (g_pe_finish_pending ? 1 : 0) | (g_pe_token_pending ? 2 : 0);
  std::fprintf(g_state_trace, "%u,%016llX,%016llX,%016llX,%016llX\n", g_retraces,
      h, gx::hash_bytes(ram, ppc::RAM_SIZE), gx::hash_bytes(aram, 0x01000000), events);
  std::fflush(g_state_trace);
}

static double g_frame_time = 0.0;
static double g_emulation_speed = 1.0;
void set_emulation_speed(double speed) { g_emulation_speed = speed < 0.5 ? 0.5 : speed > 2.0 ? 2.0 : speed; }
double emulation_speed() { return g_emulation_speed; }
double now_seconds() { return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count(); }
double frame_time() { return g_frame_time; }

// Simulation-thread cost accounting: HLE entry points add their time to a slot; at the next
// retrace the frame's work time (sleep excluded) is logged when it exceeds 20 ms, with the
// slots that explain it, so a hitch is attributed instead of guessed.
static double g_sim_costs[SIM_COST_COUNT];
static double g_sim_costs_window[SIM_COST_COUNT];   // accumulated over the 60-frame log interval
static double g_sim_ms_window = 0, g_sim_ms_worst = 0;
static const char* const g_sim_cost_names[SIM_COST_COUNT] = {"disc", "ax", "jukebox", "exi", "texsnap", "queue", "observe"};
static double g_sim_frame_start = 0.0, g_last_sim_ms = 0.0;
void sim_cost_add(int slot, double seconds) { if (slot >= 0 && slot < SIM_COST_COUNT) { g_sim_costs[slot] += seconds; g_sim_costs_window[slot] += seconds; } }
// "sim: 3.1 ms/frame (worst 12.4) | observe 0.9 texsnap 0.4" for the periodic frame log.
static std::string sim_cost_line(uint32_t frames) {
  char buf[320];
  size_t n = (size_t)std::snprintf(buf, sizeof buf, "sim: %.1f ms/frame (worst %.1f)", g_sim_ms_window / std::max(1u, frames), g_sim_ms_worst);
  bool first = true;
  for (int i = 0; i < SIM_COST_COUNT; ++i) {
    double ms = g_sim_costs_window[i] * 1000.0 / std::max(1u, frames);
    if (ms < 0.05) continue;
    n += (size_t)std::snprintf(buf + n, sizeof buf - n, "%s %s %.2f", first ? " |" : "", g_sim_cost_names[i], ms);
    first = false;
  }
  std::memset(g_sim_costs_window, 0, sizeof g_sim_costs_window);
  g_sim_ms_window = 0; g_sim_ms_worst = 0;
  return buf;
}
double last_sim_frame_ms() { return g_last_sim_ms; }

void retrace() {
  struct Guard { Guard() { g_in_retrace = true; } ~Guard() { g_in_retrace = false; } } guard;
  ++g_retraces;
  {
    double now = now_seconds();
    if (g_sim_frame_start > 0.0) {
      g_last_sim_ms = (now - g_sim_frame_start) * 1000.0;
      g_sim_ms_window += g_last_sim_ms;
      if (g_last_sim_ms > g_sim_ms_worst) g_sim_ms_worst = g_last_sim_ms;
      if (g_last_sim_ms > 20.0) {
        char detail[256] = ""; size_t n = 0;
        for (int i = 0; i < SIM_COST_COUNT; ++i) if (g_sim_costs[i] * 1000.0 >= 0.5) n += (size_t)std::snprintf(detail + n, sizeof detail - n, " %s %.1f", g_sim_cost_names[i], g_sim_costs[i] * 1000.0);
        log("sim frame %u took %.1f ms (ms:%s%s)", g_retraces, g_last_sim_ms, detail, n ? "" : " guest code");
      }
    }
    std::memset(g_sim_costs, 0, sizeof g_sim_costs);
  }
  slippi::poll_options();
  advance_frame();
  if (g_has_window) window_pump();
  if (!options.fast) {
    g_next_frame += std::chrono::microseconds((long long)(16667.0 / g_emulation_speed));
    auto now = std::chrono::steady_clock::now();
    if (g_next_frame > now) std::this_thread::sleep_until(g_next_frame);
    else if (now - g_next_frame > std::chrono::milliseconds(34)) g_next_frame = now;   // after a stall, resume at 60 Hz instead of sprinting to catch up (audio would crackle)
    g_frame_time = std::chrono::duration<double>(g_next_frame.time_since_epoch()).count();
  } else {
    g_frame_time = now_seconds();
  }
  g_sim_frame_start = now_seconds();
  fire_due_alarms(true);
  hle::audio_tick(true);
  // VI: mark display-interrupt 0 as pending (bit 15 of DI0 status, VI reg index 0x18).
  uint16_t di0 = ((uint16_t)g_mmio[0x2030] << 8) | g_mmio[0x2031];
  di0 |= 0x8000;
  g_mmio[0x2030] = (uint8_t)(di0 >> 8); g_mmio[0x2031] = (uint8_t)di0;
  deliver_interrupt(24);  // __OS_INTERRUPT_PI_VI
  trace_state();
  if (g_retraces % 60 == 0 || (options.frames && g_retraces >= options.frames)) {
    uint64_t commands, draws, vertices; uint32_t copies;
    gx_stats(&commands, &draws, &vertices, &copies);
    log("[frame %u] gx: %llu cmds %llu draws %llu verts %u efb-copies | disc: %llu reads %.1f MB | %s",
        g_retraces, commands, draws, vertices, copies, g_disc_reads, g_disc_bytes / 1048576.0, sim_cost_line(60).c_str());
  }
  if (options.frames && g_retraces >= options.frames) request_exit(0);
  if (g_exit) {
    log("exit requested after %u retraces", g_retraces);
    std::fflush(stdout);
    throw ExitRequested{g_exit_code.load()};
  }
}

// Nesting: a callback that sleeps (OSSleepThread inside a DVD/ARQ chain) is a wait point where
// hardware would run further completions, so forced delivery may nest; polled entry points
// (force = false) never nest so callback order stays as posted.
static int g_pump_depth = 0;
static bool deliver_completions(bool force) {
  if (g_completions.empty()) return false;
  if (!force && (g_pump_depth > 0 || !ppc::interrupts_on(*cpu))) return false;
  if (g_pump_depth >= 16) return false;
  ++g_pump_depth;
  size_t n = g_completions.size();
  ppc::Context saved = *cpu;
  for (size_t i = 0; i < n && !g_completions.empty(); ++i) {
    Completion fn = std::move(g_completions.front());
    g_completions.pop_front();
    fn();
  }
  uint64_t tb = cpu->tb;
  *cpu = saved;
  cpu->tb = tb;
  ppc::update_mxcsr(*cpu);
  --g_pump_depth;
  return true;
}

void wait_event() {
  if (g_pe_finish_pending) {
    g_pe_finish_pending = false;
    // PE_ISR (0xCC00100A): finish interrupt status bit 3.
    g_mmio[0x100B] |= 0x08;
    deliver_interrupt(19);  // __OS_INTERRUPT_PI_PE_FINISH
    return;
  }
  if (g_pe_token_pending) {
    g_pe_token_pending = false;
    g_mmio[0x100B] |= 0x04;
    g_mmio[0x100E] = (uint8_t)(g_pe_token >> 8); g_mmio[0x100F] = (uint8_t)g_pe_token;
    deliver_interrupt(18);  // __OS_INTERRUPT_PI_PE_TOKEN
    return;
  }
  // The sleeping thread yields: interrupts are effectively enabled during the switch, so pending
  // completions run now (nested if this sleep happens inside another callback). Otherwise time moves on.
  hle::dvd_poll();
  if (deliver_completions(true)) return;
  retrace();
}

}  // namespace host

namespace ppc {
void loop_poll(Context& c) {
  (void)c;
  host::pump_completions();   // advances time; fires alarms / audio frames / completions when EE is set
}
void interrupts_enabled(Context& c) {
  // Called from mtmsr when EE goes 0 -> 1: flush events that arrived while masked.
  if (host::g_pump_depth == 0) host::deliver_completions(true);   // never nest from inside a callback here
}
}  // namespace ppc

namespace host {

// ---------------- MMIO ----------------
static uint32_t mmio_get(uint32_t off, int bytes) {
  uint32_t v = 0;
  for (int i = 0; i < bytes; ++i) v = (v << 8) | g_mmio[(off + i) & 0xFFFF];
  return v;
}
static void mmio_put(uint32_t off, uint32_t value, int bytes) {
  for (int i = bytes - 1; i >= 0; --i) { g_mmio[(off + i) & 0xFFFF] = (uint8_t)value; value >>= 8; }
}

uint32_t mmio_read(uint32_t addr, int bytes) {
  if ((addr & 0xFFFF0000u) == 0xCC000000u) {
    uint32_t off = addr & 0xFFFF;
    switch (off & 0xFFFE) {
      case 0x2002: return 0;                 // VI: vertical position (VIGetCurrentLine)
      case 0x2000: return 0;
      case 0x0000: return 0;                 // CP status: fifo idle, not overflowed
      case 0x0004: return 0;                 // CP control
      case 0x0034: case 0x0036: return mmio_get(off, bytes);   // CP fifo rw distance (we keep 0)
      case 0x3000: return 0;                 // PI INTSR
      case 0x5004: return 0;                 // DSP mailbox from DSP: nothing pending
      case 0x5000: return 0;                 // DSP mailbox to DSP: not busy
      case 0x500A: return mmio_get(off, bytes) & ~0x0001u;  // DSP CSR: DSP not "reset in progress"
      default: return mmio_get(off, bytes);
    }
  }
  if ((addr & 0xF8000000u) == 0xC8000000u) return 0;  // EFB peek
  static int reported = 0;
  if (reported++ < 20) {
    log("mmio read %08X (%d) from %s lr=%08X", addr, bytes, symbol_name(cpu->last_pc), cpu->lr);
    if (reported <= 2) {
      log("  recent entries:");
      for (uint32_t i = 48; i < 64; ++i) { uint32_t pc = cpu->trace[(cpu->trace_pos + i) & 63]; if (pc) log("    %08X %s", pc, symbol_name(pc)); }
    }
  }
  return 0;
}

void mmio_write(uint32_t addr, uint32_t value, int bytes) {
  if ((addr & 0xFFFFC000u) == 0xCC008000u) { gx_write(value, bytes); return; }
  if ((addr & 0xFFFF0000u) == 0xCC000000u) {
    uint32_t off = addr & 0xFFFF;
    mmio_put(off, value, bytes);
    if (off == 0x3000 || off == 0x3004) return;
    return;
  }
  if ((addr & 0xF8000000u) == 0xC8000000u) return;  // EFB poke
  static int reported = 0;
  if (reported++ < 20) log("mmio write %08X = %08X (%d) from %s", addr, value, bytes, symbol_name(cpu->last_pc));
}

// ---------------- GX glue ----------------
void gx_write(uint32_t value, int bytes) { gx::write_fifo(value, bytes); }
void gx_frame_present(uint32_t) {}
void gx_stats(uint64_t* commands, uint64_t* draws, uint64_t* vertices, uint32_t* efb_copies) {
  gx::stats(commands, draws, vertices, efb_copies);
}

}  // namespace host
