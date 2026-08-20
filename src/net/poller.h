#pragma once

#include <memory>
#include <vector>

namespace kvsd {

// One readiness report for one fd. Both backends are normalized to at most one of
// these per fd per wait() -- see the coalescing note in poller_kqueue.cpp.
struct PollEvent {
  int fd = -1;
  bool readable = false;
  bool writable = false;
  // Hangup or socket error. The loop still attempts the read, because read() is what
  // distinguishes an orderly close (returns 0) from a reset (returns ECONNRESET), and
  // the reply to both is the same anyway: close the connection.
  bool error = false;
};

// A readiness poller over epoll (Linux) or kqueue (macOS/BSD). Exactly one backend is
// compiled in; the choice is made by CMake at configure time, so there is no virtual
// dispatch and no runtime branch.
//
// The interface is deliberately narrower than either syscall:
//   - Read interest is always on. Nothing in kvsd needs to stop reading from a live
//     connection, and an unused knob is a knob that is never tested.
//   - Registration changes are applied immediately, one fd at a time. kqueue can batch
//     a whole changelist into the same call that collects events; we do not exploit
//     that, because matching epoll_ctl's one-call-per-change shape keeps the two
//     backends behaviourally identical, and the change rate is near zero in steady
//     state (see the write_armed_ flag on Conn).
// Both are documented in docs/DESIGN.md, "What poller.h does and does not hide".
class Poller {
 public:
  Poller();
  ~Poller();
  Poller(const Poller&) = delete;
  Poller& operator=(const Poller&) = delete;

  bool valid() const;
  static const char* backend_name();

  // Registers fd for read readiness, and for write readiness if want_write.
  bool add(int fd, bool want_write);

  // Arms or disarms write readiness for an already-registered fd.
  bool set_write_interest(int fd, bool want_write);

  // Best effort: closing an fd unregisters it from both backends automatically, so a
  // failure here is only interesting while the fd is still open.
  bool del(int fd);

  // Blocks for up to timeout_ms (negative = forever). Returns the number of events
  // written to out, or -1 on a real error. A signal interrupting the wait is reported
  // as zero events, not an error, so the caller's shutdown check runs normally.
  int wait(std::vector<PollEvent>& out, int timeout_ms);

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace kvsd
