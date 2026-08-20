#pragma once

#include <cstddef>
#include <functional>
#include <memory>
#include <unordered_map>
#include <vector>

#include "net/poller.h"

namespace kvsd {

// The single-threaded event loop: wait for readiness, dispatch, run the periodic
// tick, repeat. Everything in kvsd runs on this thread, which is what lets the
// keyspace be a plain unordered_map with no locking anywhere.
class EventLoop {
 public:
  using IoCallback = std::function<void(int fd, bool readable, bool writable)>;
  using TickCallback = std::function<void()>;

  EventLoop();
  ~EventLoop();
  EventLoop(const EventLoop&) = delete;
  EventLoop& operator=(const EventLoop&) = delete;

  bool valid() const { return poller_.valid(); }

  // Takes ownership of cb. The fd must not already be registered.
  bool add(int fd, bool want_write, IoCallback cb);

  bool set_write_interest(int fd, bool want_write);

  // Unregisters fd. Safe to call from inside that fd's own callback. Does NOT close
  // the fd -- the owner does that, after this returns.
  void remove(int fd);

  // Runs after every dispatch batch, and after every idle wakeup. This is where
  // active expiry and the shutdown check live.
  void set_tick(TickCallback cb) { tick_ = std::move(cb); }

  // Upper bound on how long a wait() may block, which is therefore also the worst-case
  // latency of the tick callback and of noticing a shutdown signal.
  void set_tick_interval_ms(int ms) { tick_interval_ms_ = ms; }

  void run();
  void stop() { running_ = false; }

  size_t registered_count() const { return handlers_.size(); }

 private:
  Poller poller_;

  // WHY unique_ptr rather than storing the std::function by value: a callback routinely
  // ends its own registration (a connection closes itself on QUIT, on EOF, or on a
  // protocol error). Erasing the map entry from inside the call would destroy the
  // std::function whose operator() is on the stack. The indirection means the callable
  // stays put -- rehashing on insert moves pointers, not callables -- and retiring a
  // registration just moves the owning pointer into retired_, which is cleared at the
  // end of the tick, once no dispatch frame can still be inside it.
  std::unordered_map<int, std::unique_ptr<IoCallback>> handlers_;
  std::vector<std::unique_ptr<IoCallback>> retired_;

  std::vector<PollEvent> events_;
  TickCallback tick_;
  int tick_interval_ms_ = 100;
  bool running_ = false;
};

}  // namespace kvsd
