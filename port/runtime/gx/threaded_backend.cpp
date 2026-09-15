// Render thread: owns the window and the D3D12 backend, consumes simulation frames from a bounded
// queue, and presents on its own timeline. With a sub-frame mode enabled it renders new frames
// between 60 Hz simulation frames from re-posed geometry (see subframe.h); the simulation is never
// touched. The bounded source queue can back-pressure simulation when rendering is slow.
// SPDX-License-Identifier: GPL-2.0-or-later
#include "threaded_backend.h"
#include "frame_queue.h"
#include "gx_d3d12.h"
#include "host.h"
#include "subframe.h"
#include "authored_pose.h"
#include "window.h"
#include <algorithm>
#include <chrono>
#include <string>
#include <cstdio>
#include <cstring>
#include <future>
#include <cmath>
#include <thread>
namespace gx {
namespace {

constexpr double SIM_PERIOD = 1.0 / 60.0;

class ThreadedBackend final : public Backend {
  FrameQueue queue;
  std::thread worker;
  D3D12Options options_;

  // Runs on the render thread.
  void present_loop(Backend* renderer) {
    SubFrameSolver solver;
    std::vector<DrawMatrices> overrides;
    Frame frames[2];             // ring: previous and current simulation frames
    int cur = -1;                // index of the current frame in `frames`, -1 until the first arrives
    bool have_prev = false;
    uint64_t rendered_sequence = 0, submitted = 0, presented = 0, burst_logged = 0;
    bool subframes = options_.subframe != SubFrameMode::Off;
    bool authored = options_.subframe == SubFrameMode::Authored || options_.subframe == SubFrameMode::AuthoredInterpolate;
    bool interpolate = options_.subframe == SubFrameMode::Interpolate || options_.subframe == SubFrameMode::AuthoredInterpolate;
    double cap_period = options_.fps_cap > 0 ? 1.0 / options_.fps_cap : 0.0;
    double refresh_check = 0;
    double render_budget = 0.004;
    struct Trace {
      FILE* file = nullptr;
      explicit Trace(const std::string& path) {
        if (!path.empty()) {
          file = std::fopen(path.c_str(), "w");
          if (!file) throw std::runtime_error("cannot open frame timing CSV");
          std::setvbuf(file, nullptr, _IOFBF, 1024 * 1024);
          std::fputs("presentation,simulation,phase,source_age_ms,interval_ms,solver_ms,submit_ms,present_wait_ms,authored_draws,paired_draws,sim_ms\n", file);
        }
      }
      ~Trace() { if (file) std::fclose(file); }
    } trace(options_.frame_times);
    double last_submission = 0; uint64_t drained = 0;
    double next_present = host::now_seconds();
    double stats_time = next_present; uint64_t stats_presented = 0, stats_sim = 0, stats_lines = 0;
    uint32_t phase_bins[5] = {};
    double build_seconds = 0, submit_seconds = 0; uint64_t cost_presented = 0;   // presented phases: [0,.25) [.25,.5) [.5,.75) [.75,1) exactly 1
    for (;;) {
      host::window_pump();
      const auto& live_options = d3d12_options(renderer);
      if (live_options.fps_cap >= 0) cap_period = live_options.fps_cap > 0 ? 1.0/live_options.fps_cap : 0;
      if (live_options.fps_cap < 0 && host::now_seconds() >= refresh_check) {
        cap_period = 1.0 / host::window_refresh_rate();
        refresh_check = host::now_seconds() + 1.0;
      }
      if (host::window_closed()) { queue.finish(true); break; }
      {
        // The PC settings panel can switch sub-frame animation at run time.
        bool now_sub = live_options.subframe != SubFrameMode::Off;
        if (now_sub != subframes) {
          subframes = now_sub;
          if (subframes && cur >= 0) solver.set_frames(have_prev ? &frames[cur ^ 1] : nullptr, &frames[cur]);
        }
      }
      authored = live_options.subframe == SubFrameMode::Authored || live_options.subframe == SubFrameMode::AuthoredInterpolate;
      interpolate = live_options.subframe == SubFrameMode::Interpolate || live_options.subframe == SubFrameMode::AuthoredInterpolate;
      // Render every source at least once: EFB resources can depend on earlier commands.
      bool got_new = false;
      Frame incoming;
      if ((cur < 0 || frames[cur].sequence == rendered_sequence) && queue.try_pop(incoming)) {
        int next = cur < 0 ? 0 : cur ^ 1;
        queue.recycle(std::move(frames[next]));   // return the buffers this slot is about to drop
        frames[next] = std::move(incoming);
        have_prev = cur >= 0;
        cur = next;
        got_new = true;
        ++submitted; ++stats_sim;
      }
      if (cur < 0) {
        if (queue.drained()) break;
        queue.wait_available(std::chrono::milliseconds(2));
        continue;
      }
      // Pairing must follow every new frame, including drained ones: the index maps this frame's
      // draws onto the previous frame's, and the ring slots are refilled underneath it.
      if (got_new && subframes) solver.set_frames(have_prev ? &frames[cur ^ 1] : nullptr, &frames[cur]);
      // Backlog (the renderer fell behind, or a compile burst): execute older frames without
      // the solver or a present so their EFB copies exist, then catch up to the newest one. The
      // simulation never waits on this.
      if (got_new && queue.size() > 0) {
        renderer->set_skip_present(true);
        renderer->submit_frame(frames[cur]);
        renderer->set_skip_present(false);
        rendered_sequence = frames[cur].sequence;
        ++drained;
        if (drained == 1 || drained % 300 == 0) host::log("renderer: draining a backlog of %zu queued frames (%llu drained so far)", queue.size() + 1, (unsigned long long)drained);
        continue;
      }
      const Frame& current = frames[cur];
      bool should_render;
      double t = 0.0;
      if (!subframes) {
        should_render = current.sequence != rendered_sequence;   // once per simulation frame
        if (!should_render) {
          if (queue.drained()) break;
          queue.wait_available(std::chrono::milliseconds(2));
          continue;
        }
      } else {
        double now = host::now_seconds();
        t = (now - current.time) / SIM_PERIOD;
        if (interpolate) t = std::min(std::max(t, 0.0), 1.0);
        else t = std::min(std::max(t, 0.0), 1.0);   // never extrapolate more than one frame ahead
        if (cap_period > 0 && now < next_present - render_budget) {
          // Start early enough to finish GPU submission before the presentation deadline.
          double wait = next_present - render_budget - now;
          if (wait > 0.0005) std::this_thread::sleep_for(std::chrono::microseconds((long long)(std::min(wait - 0.0003, 0.001) * 1e6)));
          else std::this_thread::yield();
          continue;
        }
        should_render = true;
        if (queue.drained() && current.sequence == rendered_sequence) break;
      }
      if (options_.capture_burst && options_.capture_sim_frame && current.sequence >= options_.capture_sim_frame && burst_logged < options_.capture_burst) {
        host::log("present %llu: sim frame %llu phase %.3f", presented + 1, (unsigned long long)current.sequence, t);
        ++burst_logged;
      }
      renderer->set_present_deadline(subframes && cap_period > 0 ? next_present : 0);
      const double render_start = host::now_seconds();
      double solver_ms = 0;
      if (subframes && have_prev) {
        double t0 = host::now_seconds();
        solver.build(t, interpolate, overrides, authored);
        double t1 = host::now_seconds();
        solver_ms = (t1-t0)*1000.0;
        renderer->submit_frame(current, overrides.data());
        build_seconds += t1 - t0; submit_seconds += host::now_seconds() - t1; ++cost_presented;
      } else {
        double t1 = host::now_seconds();
        renderer->submit_frame(current);
        submit_seconds += host::now_seconds() - t1; ++cost_presented;
      }
      const double render_end = host::now_seconds();
      const double present_wait = renderer->presentation_wait_seconds();
      render_budget = std::max(render_budget * 0.95, render_end-render_start-present_wait+0.0002);
      if (trace.file) std::fprintf(trace.file, "%llu,%llu,%.6f,%.3f,%.3f,%.3f,%.3f,%.3f,%u,%u,%.3f\n",
          (unsigned long long)(presented+1), (unsigned long long)current.sequence, t,
          (render_start-current.time)*1000.0, last_submission ? (render_end-last_submission)*1000.0 : 0.0,
          solver_ms, (render_end-render_start-present_wait)*1000.0-solver_ms, present_wait*1000.0, solver.stats().authored, solver.stats().paired, host::last_sim_frame_ms());
      last_submission = render_end;
      rendered_sequence = current.sequence;
      ++presented; ++stats_presented;
      ++phase_bins[t >= 1.0 ? 4 : (int)(t * 4.0)];
      if (cap_period > 0) {
        double now = host::now_seconds();
        // Skip missed slots; never emit catch-up bursts after a stall.
        next_present += cap_period;
        if (next_present <= now) next_present += (std::floor((now-next_present)/cap_period)+1.0)*cap_period;
      }
      double now = host::now_seconds();
      if (now - stats_time >= 1.0) {
        const SubFrameStats& s = solver.stats();
        wchar_t title[160];
        _snwprintf_s(title, _TRUNCATE, L"Melee Unlocked  |  DISPLAY %.0f fps%s  |  game logic %.0f Hz (always 60, like Rivals' physics)  |  %s  |  draws %u paired %u",
                     stats_presented / (now - stats_time), cap_period > 0 ? L" (capped)" : L" (uncapped)", stats_sim / (now - stats_time),
                     !subframes ? L"locked" : authored ? L"authored" : interpolate ? L"interpolate" : L"extrapolate", s.draws, s.paired);
        host::window_set_title(title);
        if (++stats_lines % 5 == 0) {
          host::log("display: %.0f fps (sim %.0f Hz, %s, %u draws, %u paired, %u cuts)", stats_presented / (now - stats_time), stats_sim / (now - stats_time),
                    !subframes ? "locked" : authored ? "authored" : interpolate ? "interpolate" : "extrapolate", s.draws, s.paired, s.cuts);
          if (subframes) host::log("pair rejection: missing %u, HUD %u, geometry %u, state %u (last BP %02X), projection %u, authored %u, camera-only %u, vertex-blended %u | phases <.25:%u <.5:%u <.75:%u <1:%u =1:%u",
                                   s.missing, s.hud, s.geometry, s.state, s.state_register, s.projection, s.authored, s.carried, s.vertex_blended, phase_bins[0], phase_bins[1], phase_bins[2], phase_bins[3], phase_bins[4]);
          if (subframes) std::memset(phase_bins, 0, sizeof phase_bins);
          host::log("render cost: solver %.2f ms/frame, submit %.2f ms/frame (%s)", 1000.0 * build_seconds / std::max<uint64_t>(1, cost_presented), 1000.0 * submit_seconds / std::max<uint64_t>(1, cost_presented), d3d12_profile_line().c_str());
          build_seconds = submit_seconds = 0; cost_presented = 0;
          if (authored) {
            const AuthoredStats& a = authored_stats();
            std::string line = "authored: captured " + std::to_string(a.captured) + " sampled " + std::to_string(a.sampled) + " | capture fails:";
            for (int i = 1; i < 24; ++i) if (a.capture[i]) line += " c" + std::to_string(i) + "=" + std::to_string(a.capture[i]);
            line += " | sample fails:";
            for (int i = 1; i < 24; ++i) if (a.sample[i]) line += " s" + std::to_string(i) + "=" + std::to_string(a.sample[i]);
            host::log("%s", line.c_str());
          }
        }
        stats_time = now; stats_presented = 0; stats_sim = 0;
      }
    }
    host::log("renderer: %llu simulation frames, %llu presented frames on its own thread, %llu drained without presenting", submitted, presented, (unsigned long long)drained);
  }

 public:
  ThreadedBackend(D3D12Options options, bool visible) : options_(options) {
    std::promise<void> initialized;
    auto ready = initialized.get_future();
    worker = std::thread([this, options, visible, init = std::move(initialized)]() mutable {
      bool started = false;
      try {
        void* window = host::window_create(options.window_w, options.window_h, "Melee Unlocked (development)", visible);
        if (options.fullscreen) host::window_set_fullscreen(true);
        // The swapchain must match the window as it is now (fullscreen covers the monitor, not window_w x window_h).
        int client_w = options.window_w, client_h = options.window_h;
        host::window_client_size(&client_w, &client_h);
        std::unique_ptr<Backend> renderer(create_d3d12_backend(window, std::max(client_w, 1), std::max(client_h, 1), options));
        host::window_set_resize_callback([&renderer](int w, int h) { d3d12_resize(renderer.get(), w, h); });
        init.set_value(); started = true;
        present_loop(renderer.get());
        host::window_set_resize_callback({});
        renderer.reset();
        host::window_destroy();
      } catch (const std::exception& e) {
        host::log("renderer: fatal error on the render thread: %s", e.what());
        if (!started) init.set_exception(std::current_exception());
        else host::request_exit(3);
        queue.finish(true);
      } catch (...) {
        host::log("renderer: fatal error on the render thread");
        if (!started) init.set_exception(std::current_exception());
        else host::request_exit(3);
        queue.finish(true);
      }
    });
    try { ready.get(); }
    catch (...) { queue.finish(true); worker.join(); throw; }
  }
  ~ThreadedBackend() override { queue.finish(); worker.join(); }
  void submit_frame(const Frame& frame) override {
    host::SimCostScope cost(host::SIM_QUEUE);
    if (!queue.push(frame)) throw ExitRequested{host::exit_code()};
  }
  void submit_and_recycle(Frame& frame) override {
    host::SimCostScope cost(host::SIM_QUEUE);
    if (!queue.push_and_recycle(frame)) throw ExitRequested{host::exit_code()};
  }
};
}
std::unique_ptr<Backend> create_threaded_backend(const D3D12Options& options, bool visible) {
  return std::make_unique<ThreadedBackend>(options, visible);
}
}
