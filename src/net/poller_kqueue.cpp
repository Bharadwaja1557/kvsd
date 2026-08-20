// macOS/BSD backend. Level-triggered by deliberate omission of EV_CLEAR, which is
// kqueue's spelling of edge-triggered.
#include "net/poller.h"

#include <fcntl.h>
#include <sys/event.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>

#include <cerrno>

namespace kvsd {
namespace {
constexpr int kInitialEventCapacity = 64;
constexpr int kMaxEventCapacity = 4096;
}  // namespace

struct Poller::Impl {
  int kq = -1;
  std::vector<struct kevent> events{kInitialEventCapacity};
  // fd -> index into the caller's out vector for the event already emitted this
  // wait(), or -1. Indexed directly by fd because fds are small dense integers.
  std::vector<int> slot;
};

Poller::Poller() : impl_(new Impl) {
  impl_->kq = ::kqueue();
  if (impl_->kq >= 0) ::fcntl(impl_->kq, F_SETFD, FD_CLOEXEC);
}

Poller::~Poller() {
  if (impl_->kq >= 0) ::close(impl_->kq);
}

bool Poller::valid() const { return impl_->kq >= 0; }

const char* Poller::backend_name() { return "kqueue"; }

bool Poller::add(int fd, bool want_write) {
  // kqueue has no single "interest set" per fd the way epoll does: read and write are
  // separate filters, each registered independently. We register both up front and
  // toggle the write filter with EV_ENABLE/EV_DISABLE, rather than adding and deleting
  // it, because EV_DELETE on an absent filter fails with ENOENT and would force the
  // caller to track registration state that epoll does not require it to track.
  struct kevent ch[2];
  EV_SET(&ch[0], fd, EVFILT_READ, EV_ADD | EV_ENABLE, 0, 0, nullptr);
  EV_SET(&ch[1], fd, EVFILT_WRITE, EV_ADD | (want_write ? EV_ENABLE : EV_DISABLE), 0, 0,
         nullptr);
  return ::kevent(impl_->kq, ch, 2, nullptr, 0, nullptr) == 0;
}

bool Poller::set_write_interest(int fd, bool want_write) {
  struct kevent ch;
  EV_SET(&ch, fd, EVFILT_WRITE, want_write ? EV_ENABLE : EV_DISABLE, 0, 0, nullptr);
  return ::kevent(impl_->kq, &ch, 1, nullptr, 0, nullptr) == 0;
}

bool Poller::del(int fd) {
  struct kevent ch[2];
  EV_SET(&ch[0], fd, EVFILT_READ, EV_DELETE, 0, 0, nullptr);
  EV_SET(&ch[1], fd, EVFILT_WRITE, EV_DELETE, 0, 0, nullptr);
  return ::kevent(impl_->kq, ch, 2, nullptr, 0, nullptr) == 0;
}

int Poller::wait(std::vector<PollEvent>& out, int timeout_ms) {
  out.clear();

  struct timespec ts;
  struct timespec* tsp = nullptr;
  if (timeout_ms >= 0) {
    ts.tv_sec = timeout_ms / 1000;
    ts.tv_nsec = static_cast<long>(timeout_ms % 1000) * 1000000L;
    tsp = &ts;
  }

  const int n = ::kevent(impl_->kq, nullptr, 0, impl_->events.data(),
                         static_cast<int>(impl_->events.size()), tsp);
  if (n < 0) return (errno == EINTR) ? 0 : -1;

  // Coalesce per-filter reports into one per-fd report.
  //
  // WHY this matters: epoll returns a single event per fd with a combined mask, but
  // kqueue returns one event per *filter*, so an fd that is both readable and writable
  // appears twice in the same batch. Handed to the loop unmerged, the first event
  // could close the connection and free the Conn, and the second would then be
  // dispatched against a dead fd -- or worse, against a new connection that already
  // reused the number. Normalizing here means the loop cannot observe the difference.
  for (int i = 0; i < n; ++i) {
    const struct kevent& e = impl_->events[i];
    const int fd = static_cast<int>(e.ident);
    if (fd < 0) continue;
    if (static_cast<size_t>(fd) >= impl_->slot.size()) impl_->slot.resize(fd + 1, -1);

    int idx = impl_->slot[fd];
    if (idx < 0) {
      idx = static_cast<int>(out.size());
      impl_->slot[fd] = idx;
      PollEvent pe;
      pe.fd = fd;
      out.push_back(pe);
    }
    PollEvent& pe = out[static_cast<size_t>(idx)];

    if (e.flags & EV_ERROR) pe.error = true;
    // EV_EOF on the read filter means the peer closed. We report it as readable so the
    // read path sees the 0-byte read and runs the normal teardown.
    if (e.flags & EV_EOF) {
      pe.error = true;
      pe.readable = true;
    }
    if (e.filter == EVFILT_READ) pe.readable = true;
    if (e.filter == EVFILT_WRITE) pe.writable = true;
    if (pe.error) pe.readable = true;
  }

  for (const PollEvent& pe : out) impl_->slot[static_cast<size_t>(pe.fd)] = -1;

  if (n == static_cast<int>(impl_->events.size()) &&
      impl_->events.size() < static_cast<size_t>(kMaxEventCapacity)) {
    impl_->events.resize(impl_->events.size() * 2);
  }
  return static_cast<int>(out.size());
}

}  // namespace kvsd
