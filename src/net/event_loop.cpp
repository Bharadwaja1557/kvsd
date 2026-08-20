#include "net/event_loop.h"

#include <utility>

namespace kvsd {

EventLoop::EventLoop() = default;
EventLoop::~EventLoop() = default;

bool EventLoop::add(int fd, bool want_write, IoCallback cb) {
  if (!poller_.add(fd, want_write)) return false;
  handlers_[fd] = std::unique_ptr<IoCallback>(new IoCallback(std::move(cb)));
  return true;
}

bool EventLoop::set_write_interest(int fd, bool want_write) {
  return poller_.set_write_interest(fd, want_write);
}

void EventLoop::remove(int fd) {
  auto it = handlers_.find(fd);
  if (it == handlers_.end()) return;

  // Unregister before the caller closes the fd: after close(2) the number can be
  // handed straight back out by accept(), and a stale registration would then steer
  // the new connection's events into the old connection's callback.
  poller_.del(fd);
  retired_.push_back(std::move(it->second));
  handlers_.erase(it);
}

void EventLoop::run() {
  running_ = true;
  while (running_) {
    const int n = poller_.wait(events_, tick_interval_ms_);
    if (n < 0) break;  // the poller fd itself failed; there is no recovering from that

    for (const PollEvent& ev : events_) {
      // Look the handler up per event rather than caching it: an earlier event in this
      // same batch may have closed this connection.
      auto it = handlers_.find(ev.fd);
      if (it == handlers_.end()) continue;
      IoCallback* cb = it->second.get();
      (*cb)(ev.fd, ev.readable, ev.writable);
    }

    if (tick_) tick_();

    // Now that no dispatch frame is live, the callables retired during this iteration
    // can be destroyed.
    retired_.clear();
  }
  running_ = false;
}

}  // namespace kvsd
