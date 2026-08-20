// These tests run on both backends unmodified. That is the point: they encode the
// semantics poller.h promises, so the epoll build cannot quietly diverge from the
// kqueue build that the benchmarks were taken on.
#include "net/poller.h"

#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>

#include "test_util.h"

using kvsd::PollEvent;
using kvsd::Poller;

namespace {

struct Pair {
  int a = -1;
  int b = -1;
  Pair() {
    int fds[2];
    if (::socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0) {
      a = fds[0];
      b = fds[1];
    }
  }
  ~Pair() {
    if (a >= 0) ::close(a);
    if (b >= 0) ::close(b);
  }
};

const PollEvent* find(const std::vector<PollEvent>& v, int fd) {
  auto it = std::find_if(v.begin(), v.end(), [fd](const PollEvent& e) { return e.fd == fd; });
  return it == v.end() ? nullptr : &*it;
}

size_t count_for(const std::vector<PollEvent>& v, int fd) {
  return static_cast<size_t>(
      std::count_if(v.begin(), v.end(), [fd](const PollEvent& e) { return e.fd == fd; }));
}

}  // namespace

static void test_read_readiness() {
  Poller p;
  CHECK(p.valid());
  Pair s;
  CHECK(s.a >= 0);

  CHECK(p.add(s.a, /*want_write=*/false));

  std::vector<PollEvent> ev;
  // Nothing written yet, so the read filter must not fire.
  CHECK_EQ(p.wait(ev, 0), 0);

  CHECK_EQ(::write(s.b, "x", 1), ssize_t(1));
  CHECK_EQ(p.wait(ev, 200), 1);
  const PollEvent* e = find(ev, s.a);
  CHECK(e != nullptr);
  if (e) {
    CHECK(e->readable);
    CHECK(!e->writable);
  }
}

static void test_level_triggered_reports_until_drained() {
  // The defining property we depend on: leaving data unread means the next wait()
  // reports it again. An edge-triggered registration would report it exactly once.
  Poller p;
  Pair s;
  CHECK(p.add(s.a, false));
  CHECK_EQ(::write(s.b, "hello", 5), ssize_t(5));

  std::vector<PollEvent> ev;
  CHECK_EQ(p.wait(ev, 200), 1);

  char buf[2];
  CHECK_EQ(::read(s.a, buf, 2), ssize_t(2));  // partial read, 3 bytes still pending

  CHECK_EQ(p.wait(ev, 200), 1);
  const PollEvent* e = find(ev, s.a);
  CHECK(e && e->readable);

  char rest[8];
  CHECK_EQ(::read(s.a, rest, 8), ssize_t(3));
  CHECK_EQ(p.wait(ev, 0), 0);
}

static void test_write_interest_toggles() {
  Poller p;
  Pair s;
  std::vector<PollEvent> ev;

  CHECK(p.add(s.a, /*want_write=*/false));
  // A fresh socket is writable, but we did not ask, so nothing fires. This is what
  // keeps an idle connection from spinning the loop at 100% CPU.
  CHECK_EQ(p.wait(ev, 0), 0);

  CHECK(p.set_write_interest(s.a, true));
  CHECK_EQ(p.wait(ev, 200), 1);
  const PollEvent* e = find(ev, s.a);
  CHECK(e && e->writable);

  CHECK(p.set_write_interest(s.a, false));
  CHECK_EQ(p.wait(ev, 0), 0);
}

static void test_one_event_per_fd_when_both_ready() {
  // kqueue reports read and write as separate events for the same fd; epoll reports
  // one event with a combined mask. The loop is written against the epoll shape, so
  // the kqueue backend must coalesce.
  Poller p;
  Pair s;
  CHECK(p.add(s.a, /*want_write=*/true));
  CHECK_EQ(::write(s.b, "x", 1), ssize_t(1));

  std::vector<PollEvent> ev;
  CHECK_EQ(p.wait(ev, 200), 1);
  CHECK_EQ(count_for(ev, s.a), size_t(1));
  const PollEvent* e = find(ev, s.a);
  CHECK(e && e->readable);
  CHECK(e && e->writable);
}

static void test_peer_close_is_reported_as_readable() {
  // Teardown must always arrive through the read path, whichever flag the backend
  // actually sets (EPOLLRDHUP/EPOLLHUP on Linux, EV_EOF on macOS).
  Poller p;
  int fds[2];
  CHECK_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, fds), 0);
  CHECK(p.add(fds[0], false));
  ::close(fds[1]);

  std::vector<PollEvent> ev;
  CHECK_EQ(p.wait(ev, 200), 1);
  const PollEvent* e = find(ev, fds[0]);
  CHECK(e && e->readable);
  ::close(fds[0]);
}

static void test_del_stops_reports() {
  Poller p;
  Pair s;
  CHECK(p.add(s.a, false));
  CHECK(p.del(s.a));
  CHECK_EQ(::write(s.b, "x", 1), ssize_t(1));

  std::vector<PollEvent> ev;
  CHECK_EQ(p.wait(ev, 50), 0);
}

static void test_many_fds_are_all_reported() {
  Poller p;
  std::vector<Pair*> pairs;
  for (int i = 0; i < 40; ++i) {
    Pair* s = new Pair();
    CHECK(p.add(s->a, false));
    CHECK_EQ(::write(s->b, "x", 1), ssize_t(1));
    pairs.push_back(s);
  }

  std::vector<PollEvent> ev;
  CHECK_EQ(p.wait(ev, 500), 40);
  for (Pair* s : pairs) {
    CHECK(find(ev, s->a) != nullptr);
    delete s;
  }
}

int main() {
  test_read_readiness();
  test_level_triggered_reports_until_drained();
  test_write_interest_toggles();
  test_one_event_per_fd_when_both_ready();
  test_peer_close_is_reported_as_readable();
  test_del_stops_reports();
  test_many_fds_are_all_reported();
  return kvsd_test::summarize("poller");
}
