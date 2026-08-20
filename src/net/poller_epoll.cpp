// Linux backend. Level-triggered by deliberate omission of EPOLLET; see
// docs/DESIGN.md for why level-triggered is the right default for this server.
#include "net/poller.h"

#include <sys/epoll.h>
#include <unistd.h>

#include <cerrno>

namespace kvsd {
namespace {
constexpr int kInitialEventCapacity = 64;
constexpr int kMaxEventCapacity = 4096;
}  // namespace

struct Poller::Impl {
  int epfd = -1;
  std::vector<epoll_event> events{kInitialEventCapacity};
};

Poller::Poller() : impl_(new Impl) {
  // EPOLL_CLOEXEC so a future fork/exec does not leak the loop's own descriptor.
  impl_->epfd = ::epoll_create1(EPOLL_CLOEXEC);
}

Poller::~Poller() {
  if (impl_->epfd >= 0) ::close(impl_->epfd);
}

bool Poller::valid() const { return impl_->epfd >= 0; }

const char* Poller::backend_name() { return "epoll"; }

static uint32_t interest_mask(bool want_write) {
  // No EPOLLET: level-triggered. EPOLLRDHUP tells us the peer closed its write side
  // even when there is no pending data, which is otherwise indistinguishable from idle.
  uint32_t m = EPOLLIN | EPOLLRDHUP;
  if (want_write) m |= EPOLLOUT;
  return m;
}

bool Poller::add(int fd, bool want_write) {
  epoll_event ev{};
  ev.events = interest_mask(want_write);
  ev.data.fd = fd;
  return ::epoll_ctl(impl_->epfd, EPOLL_CTL_ADD, fd, &ev) == 0;
}

bool Poller::set_write_interest(int fd, bool want_write) {
  epoll_event ev{};
  ev.events = interest_mask(want_write);
  ev.data.fd = fd;
  return ::epoll_ctl(impl_->epfd, EPOLL_CTL_MOD, fd, &ev) == 0;
}

bool Poller::del(int fd) {
  return ::epoll_ctl(impl_->epfd, EPOLL_CTL_DEL, fd, nullptr) == 0;
}

int Poller::wait(std::vector<PollEvent>& out, int timeout_ms) {
  out.clear();
  const int n = ::epoll_wait(impl_->epfd, impl_->events.data(),
                             static_cast<int>(impl_->events.size()), timeout_ms);
  if (n < 0) return (errno == EINTR) ? 0 : -1;

  out.reserve(static_cast<size_t>(n));
  for (int i = 0; i < n; ++i) {
    const epoll_event& e = impl_->events[i];
    PollEvent pe;
    pe.fd = e.data.fd;
    pe.readable = (e.events & (EPOLLIN | EPOLLRDHUP)) != 0;
    pe.writable = (e.events & EPOLLOUT) != 0;
    pe.error = (e.events & (EPOLLERR | EPOLLHUP)) != 0;
    // An error-only report still has to reach the read path, which is where the
    // connection teardown lives.
    if (pe.error) pe.readable = true;
    out.push_back(pe);
  }

  // A full buffer means events were likely left behind; grow so the next wait can
  // drain more per syscall. Level-triggered guarantees the leftovers are re-reported.
  if (n == static_cast<int>(impl_->events.size()) &&
      impl_->events.size() < static_cast<size_t>(kMaxEventCapacity)) {
    impl_->events.resize(impl_->events.size() * 2);
  }
  return n;
}

}  // namespace kvsd
