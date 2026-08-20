#include "net/event_loop.h"

#include <sys/socket.h>
#include <unistd.h>

#include <string>

#include "test_util.h"

using kvsd::EventLoop;

namespace {
struct Pair {
  int a = -1, b = -1;
  Pair() {
    int fds[2];
    if (::socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0) { a = fds[0]; b = fds[1]; }
  }
  void close_a() { if (a >= 0) { ::close(a); a = -1; } }
  void close_b() { if (b >= 0) { ::close(b); b = -1; } }
  ~Pair() { close_a(); close_b(); }
};
}  // namespace

static void test_dispatches_read_then_ticks() {
  EventLoop loop;
  CHECK(loop.valid());
  Pair s;
  CHECK_EQ(::write(s.b, "hi", 2), ssize_t(2));

  std::string seen;
  int ticks = 0;
  CHECK(loop.add(s.a, false, [&](int fd, bool r, bool) {
    if (r) {
      char buf[8];
      ssize_t n = ::read(fd, buf, sizeof(buf));
      if (n > 0) seen.append(buf, static_cast<size_t>(n));
    }
  }));
  loop.set_tick([&] {
    if (++ticks >= 2) loop.stop();
  });
  loop.set_tick_interval_ms(10);
  loop.run();

  CHECK_EQ(seen, std::string("hi"));
  CHECK(ticks >= 2);
}

static void test_callback_can_remove_itself() {
  // The lifetime case the unique_ptr indirection exists for: the callback ends its own
  // registration and closes its fd while its own frame is still on the stack.
  EventLoop loop;
  Pair s;
  CHECK_EQ(::write(s.b, "x", 1), ssize_t(1));

  int calls = 0;
  CHECK(loop.add(s.a, false, [&](int fd, bool, bool) {
    ++calls;
    loop.remove(fd);
    ::close(fd);
    s.a = -1;
  }));
  int ticks = 0;
  loop.set_tick([&] { if (++ticks >= 3) loop.stop(); });
  loop.set_tick_interval_ms(5);
  loop.run();

  // Without the deferral it would still be called once; what matters is that the data
  // stayed readable and it was NOT called again, and that nothing crashed on the way.
  CHECK_EQ(calls, 1);
  CHECK_EQ(loop.registered_count(), size_t(0));
}

static void test_write_interest_round_trip() {
  EventLoop loop;
  Pair s;
  int writes = 0;
  CHECK(loop.add(s.a, /*want_write=*/false, [&](int fd, bool, bool w) {
    if (w) {
      ++writes;
      // Result checked rather than discarded: glibc marks write() warn_unused_result,
      // and this project builds with -Werror in CI.
      CHECK_EQ(::write(fd, "y", 1), ssize_t(1));
      loop.set_write_interest(fd, false);
    }
  }));

  int ticks = 0;
  loop.set_tick([&] {
    ++ticks;
    if (ticks == 1) loop.set_write_interest(s.a, true);
    if (ticks >= 4) loop.stop();
  });
  loop.set_tick_interval_ms(5);
  loop.run();

  // Armed once, fired once, disarmed again -- an idle connection must not spin.
  CHECK_EQ(writes, 1);
}

static void test_new_registration_during_dispatch() {
  // Mirrors accept(): a callback adds another fd while the loop is mid-batch.
  EventLoop loop;
  Pair first, second;
  CHECK_EQ(::write(first.b, "a", 1), ssize_t(1));
  CHECK_EQ(::write(second.b, "b", 1), ssize_t(1));

  std::string seen;
  bool added = false;
  CHECK(loop.add(first.a, false, [&](int fd, bool, bool) {
    char c;
    if (::read(fd, &c, 1) == 1) seen += c;
    if (!added) {
      added = true;
      loop.add(second.a, false, [&](int fd2, bool, bool) {
        char c2;
        if (::read(fd2, &c2, 1) == 1) seen += c2;
      });
    }
  }));
  int ticks = 0;
  loop.set_tick([&] { if (++ticks >= 3) loop.stop(); });
  loop.set_tick_interval_ms(5);
  loop.run();

  CHECK_EQ(seen, std::string("ab"));
}

int main() {
  test_dispatches_read_then_ticks();
  test_callback_can_remove_itself();
  test_write_interest_round_trip();
  test_new_registration_during_dispatch();
  return kvsd_test::summarize("event_loop");
}
