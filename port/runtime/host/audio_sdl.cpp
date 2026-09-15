// Host audio output for POSIX: 32 kHz 16-bit stereo from the emulated AI DMA through SDL2.
//
// The design is the same as the Windows WASAPI path (audio.cpp): a lock-free single-producer/
// single-consumer ring written by the simulation and drained by the audio callback, resampled with
// a cubic interpolator whose ratio is steered by the ring fill. The game's AI clock and the sound
// card's crystal drift apart, and without the steer the ring would slowly fill or empty until it
// dropped a block or ran dry (audible clicks/gaps).
// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef _MSC_VER
#include <SDL.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include "audio.h"
#include "host.h"
#include "jukebox.h"

namespace host {
namespace {
constexpr int SAMPLE_RATE = 32000;
constexpr int BLOCK_BYTES = 640;      // one 5 ms AI DMA frame: 160 stereo samples
constexpr size_t RING_FRAMES = 8192;  // 256 ms, power of two
constexpr size_t RING_MASK = RING_FRAMES - 1;
constexpr double MAX_RATE_SHIFT = 0.015;

std::atomic<int> g_volume{0};
std::atomic<uint64_t> g_frames{0}, g_dropped{0}, g_underruns{0}, g_underrun_frames{0};
std::atomic<double> g_rate_min{1.0}, g_rate_max{1.0};
bool g_open = false;
SDL_AudioDeviceID g_dev = 0;
FILE* g_wav = nullptr;
uint32_t g_wav_bytes = 0;

int16_t g_ring[RING_FRAMES][2];
std::atomic<uint64_t> g_ring_write{0}, g_ring_read{0};
double g_ring_phase = 0.0;
double g_rate = 1.0;
double g_fill_average = 0.0;
int16_t g_last_output[2] = {0, 0};
size_t g_target_frames = SAMPLE_RATE * 35 / 1000;
bool g_priming = true;

void wav_header(FILE* f, uint32_t data_bytes) {
  auto u32 = [&](uint32_t v) { std::fwrite(&v, 4, 1, f); };
  auto u16 = [&](uint16_t v) { std::fwrite(&v, 2, 1, f); };
  std::fwrite("RIFF", 1, 4, f); u32(36 + data_bytes); std::fwrite("WAVE", 1, 4, f);
  std::fwrite("fmt ", 1, 4, f); u32(16); u16(1); u16(2); u32(SAMPLE_RATE); u32(SAMPLE_RATE * 4); u16(4); u16(16);
  std::fwrite("data", 1, 4, f); u32(data_bytes);
}

inline double cubic(double a, double b, double c, double d, double t) {
  return b + 0.5 * t * (c - a + t * (2.0 * a - 5.0 * b + 4.0 * c - d + t * (3.0 * (b - c) + d - a)));
}

void sdl_callback(void*, Uint8* stream, int len) {
  int16_t* out = reinterpret_cast<int16_t*>(stream);
  uint32_t want = (uint32_t)len / 4;   // stereo frames
  const int volume = g_volume.load();
  uint64_t read = g_ring_read.load(std::memory_order_relaxed);

  const uint64_t buffered = g_ring_write.load(std::memory_order_acquire) - read;
  if (g_priming) {
    if (buffered < g_target_frames) { std::memset(stream, 0, (size_t)want * 4); return; }
    g_priming = false;
  }
  if (g_fill_average == 0.0) g_fill_average = (double)buffered;
  g_fill_average += ((double)buffered - g_fill_average) * 0.02;
  const double error = (g_fill_average - (double)g_target_frames) / (double)g_target_frames;
  const double target_rate = 1.0 + std::clamp(error * 0.25, -MAX_RATE_SHIFT, MAX_RATE_SHIFT);
  g_rate += (target_rate - g_rate) * 0.05;
  if (g_rate < g_rate_min.load()) g_rate_min.store(g_rate);
  if (g_rate > g_rate_max.load()) g_rate_max.store(g_rate);

  uint32_t starved = 0;
  for (uint32_t i = 0; i < want; ++i) {
    const uint64_t write = g_ring_write.load(std::memory_order_acquire);
    if (write - read < 4) {
      out[i * 2] = (int16_t)((int32_t)g_last_output[0] * volume / 100);
      out[i * 2 + 1] = (int16_t)((int32_t)g_last_output[1] * volume / 100);
      ++starved;
      continue;
    }
    for (int channel = 0; channel < 2; ++channel) {
      const double a = g_ring[(read - 1) & RING_MASK][channel];
      const double b = g_ring[read & RING_MASK][channel];
      const double c = g_ring[(read + 1) & RING_MASK][channel];
      const double d = g_ring[(read + 2) & RING_MASK][channel];
      const double v = cubic(a, b, c, d, g_ring_phase);
      g_last_output[channel] = (int16_t)std::clamp(v, -32768.0, 32767.0);
      out[i * 2 + channel] = (int16_t)((int32_t)g_last_output[channel] * volume / 100);
    }
    g_ring_phase += g_rate;
    while (g_ring_phase >= 1.0) { g_ring_phase -= 1.0; ++read; }
  }
  g_ring_read.store(read, std::memory_order_release);
  if (starved) { g_underruns.fetch_add(1); g_underrun_frames.fetch_add(starved); }
  if (volume > 0) slippi::jukebox::mix(out, want);
}
}  // namespace

void audio_set_volume(int volume) { g_volume.store(std::clamp(volume, 0, 100)); }
int audio_volume() { return g_volume.load(); }

bool audio_open(int volume_percent, const char* wav_dump_path, bool open_device) {
  if (wav_dump_path && *wav_dump_path) {
    g_wav = std::fopen(wav_dump_path, "wb");
    if (g_wav) { wav_header(g_wav, 0); g_wav_bytes = 0; g_open = true; }
    else log("audio: cannot open %s", wav_dump_path);
  }
  if (!open_device) return g_open;
  if (SDL_InitSubSystem(SDL_INIT_AUDIO) != 0) { log("audio: SDL audio init failed: %s", SDL_GetError()); return g_open; }
  SDL_AudioSpec want{}, have{};
  want.freq = SAMPLE_RATE; want.format = AUDIO_S16SYS; want.channels = 2; want.samples = 512; want.callback = sdl_callback;
  g_dev = SDL_OpenAudioDevice(nullptr, 0, &want, &have, 0);
  if (!g_dev) { log("audio: SDL_OpenAudioDevice failed: %s", SDL_GetError()); return g_open; }
  // Keep at least two device callbacks (plus the simulation's per-frame burst) buffered, so a
  // callback cannot drain the ring before the next audio burst lands.
  g_target_frames = (size_t)want.samples * 2 + SAMPLE_RATE * 10 / 1000;
  if (g_target_frames > RING_FRAMES / 2) g_target_frames = RING_FRAMES / 2;
  g_priming = true;
  g_fill_average = 0.0;
  g_volume = std::clamp(volume_percent, 0, 100);
  SDL_PauseAudioDevice(g_dev, 0);
  g_open = true;
  log("audio: SDL2 default device, %d Hz stereo (%d-frame buffer), volume %d%%", have.freq, (int)have.samples, (int)g_volume.load());
  return true;
}

void audio_close() {
  if (g_dev) { SDL_CloseAudioDevice(g_dev); g_dev = 0; }
  if (g_wav) {
    std::fseek(g_wav, 0, SEEK_SET);
    wav_header(g_wav, g_wav_bytes);
    std::fclose(g_wav); g_wav = nullptr;
  }
  SDL_QuitSubSystem(SDL_INIT_AUDIO);
  g_open = false;
}

void audio_push(const uint8_t* be_samples, size_t bytes) {
  if (!g_open) return;
  for (size_t off = 0; off + BLOCK_BYTES <= bytes; off += BLOCK_BYTES) {
    int16_t converted[BLOCK_BYTES / 2];
    const uint8_t* src = be_samples + off;
    for (int i = 0; i < BLOCK_BYTES / 4; ++i) {
      int16_t r = (int16_t)((src[i * 4] << 8) | src[i * 4 + 1]);
      int16_t l = (int16_t)((src[i * 4 + 2] << 8) | src[i * 4 + 3]);
      converted[i * 2] = l; converted[i * 2 + 1] = r;
    }
    if (g_wav) { std::fwrite(converted, 1, BLOCK_BYTES, g_wav); g_wav_bytes += BLOCK_BYTES; }
    constexpr size_t frames = BLOCK_BYTES / 4;
    uint64_t write = g_ring_write.load(std::memory_order_relaxed);
    if (write + frames - g_ring_read.load(std::memory_order_acquire) > RING_FRAMES - 4) { ++g_dropped; continue; }
    for (size_t i = 0; i < frames; ++i) {
      g_ring[(write + i) & RING_MASK][0] = converted[i * 2];
      g_ring[(write + i) & RING_MASK][1] = converted[i * 2 + 1];
    }
    g_ring_write.store(write + frames, std::memory_order_release);
    g_frames += frames;
  }
}

uint64_t audio_pushed_frames() { return g_frames.load(); }
uint64_t audio_dropped_blocks() { return g_dropped.load(); }
uint64_t audio_underruns(uint64_t* silent_ms) { if (silent_ms) *silent_ms = g_underrun_frames.load() * 1000 / SAMPLE_RATE; return g_underruns.load(); }
void audio_rate_range(double* low, double* high) { if (low) *low = g_rate_min.load(); if (high) *high = g_rate_max.load(); }
uint32_t audio_buffered_ms() {
  const uint64_t buffered = g_ring_write.load(std::memory_order_acquire) - g_ring_read.load(std::memory_order_acquire);
  return (uint32_t)(buffered * 1000 / SAMPLE_RATE);
}

}  // namespace host

#endif  // !_MSC_VER
