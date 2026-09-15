#include "frame_queue.h"
#include <cstdio>
#include <cstdlib>
#include <future>
static void check(bool ok) { if (!ok) std::abort(); }
static gx::Frame frame(uint64_t sequence) { gx::Frame f; f.sequence = sequence; return f; }
int main() {
  gx::FrameQueue queue;
  constexpr uint64_t kCap = 32;   // FrameQueue::push blocks once this many frames are queued
  for (uint64_t i = 1; i <= kCap; ++i) check(queue.push(frame(i)));
  auto blocked = std::async(std::launch::async, [&] { return queue.push(frame(kCap + 1)); });
  check(blocked.wait_for(std::chrono::milliseconds(20)) == std::future_status::timeout);
  gx::Frame out;
  check(queue.pop(out) && out.sequence == 1);
  check(blocked.get());
  queue.finish();
  check(!queue.push(frame(kCap + 2)));
  for (uint64_t i = 2; i <= kCap + 1; ++i) check(queue.pop(out) && out.sequence == i);
  check(queue.drained() && !queue.pop(out));
  gx::FrameQueue cancelled;
  check(cancelled.push(frame(1))); check(cancelled.push(frame(2)));
  auto waiting = std::async(std::launch::async, [&] { return cancelled.push(frame(3)); });
  cancelled.finish(true);
  check(!waiting.get() && cancelled.drained());
  std::puts("bounded ordering, drain, cancellation and producer wakeup passed");
}
