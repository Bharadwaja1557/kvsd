#include "net/server.h"

#include <fcntl.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <utility>

#include "cmd/commands.h"
#include "resp/writer.h"
#include "net/socket.h"
#include "util/log.h"

namespace kvsd {
namespace {

// One read() per readability report. WHY not loop until EAGAIN: with level triggering
// the leftover bytes are reported again on the next wait(), so looping buys nothing
// except the ability of one busy client to monopolise the thread. 16 KiB is Redis's
// figure -- large enough that a pipelined batch usually arrives in one call, small
// enough that the per-connection read buffer is not a memory problem at scale.
constexpr size_t kReadChunk = 16 * 1024;

// Accepts per readability report, for the same fairness reason. A listener with more
// pending connections than this stays readable and is served on the next iteration,
// after the already-connected clients have had their turn.
constexpr int kMaxAcceptsPerEvent = 1000;

#ifdef MSG_NOSIGNAL
// Linux: suppress SIGPIPE per call, since there is no socket option for it.
constexpr int kSendFlags = MSG_NOSIGNAL;
#else
// macOS/BSD: SO_NOSIGPIPE was set on the socket when it was accepted.
constexpr int kSendFlags = 0;
#endif

}  // namespace

Server::Server(ServerConfig config) : cfg_(std::move(config)) {}

Server::~Server() {
  for (auto& entry : conns_) {
    ::close(entry.second->fd);
  }
  conns_.clear();
  if (listen_fd_ >= 0) ::close(listen_fd_);
  if (spare_fd_ >= 0) ::close(spare_fd_);
}

bool Server::start(std::string* err) {
  if (!loop_.valid()) {
    if (err) *err = std::string("failed to create ") + Poller::backend_name();
    return false;
  }

  listen_fd_ = sock::listen_on(cfg_.bind_addr, cfg_.port, cfg_.backlog, err);
  if (listen_fd_ < 0) return false;

  if (!reopen_spare_fd()) {
    // Not fatal: the server works, it just degrades differently under fd exhaustion
    // (it pauses the listener instead of shedding). Worth saying out loud.
    log::warn("could not reserve a spare file descriptor: %s", std::strerror(errno));
  }

  loop_.set_tick_interval_ms(cfg_.tick_interval_ms);
  loop_.set_tick([this] { on_tick(); });

  if (!loop_.add(listen_fd_, false, [this](int, bool readable, bool) {
        if (readable) on_listener_readable();
      })) {
    if (err) *err = "failed to register the listener with the poller";
    return false;
  }

  log::info("kvsd listening on %s:%u (%s, pid %d)", cfg_.bind_addr.c_str(),
            static_cast<unsigned>(bound_port()), Poller::backend_name(),
            static_cast<int>(::getpid()));
  return true;
}

void Server::run() { loop_.run(); }

uint16_t Server::bound_port() const {
  return listen_fd_ < 0 ? 0 : sock::local_port(listen_fd_);
}

bool Server::reopen_spare_fd() {
  if (spare_fd_ >= 0) return true;
  // Any descriptor will do; /dev/null is the cheapest one that always exists.
  spare_fd_ = ::open("/dev/null", O_RDONLY | O_CLOEXEC);
  return spare_fd_ >= 0;
}

void Server::on_listener_readable() {
  for (int i = 0; i < kMaxAcceptsPerEvent; ++i) {
    const int fd = ::accept(listen_fd_, nullptr, nullptr);
    if (fd >= 0) {
      add_connection(fd);
      continue;
    }

    if (errno == EAGAIN || errno == EWOULDBLOCK) return;  // queue drained
    if (errno == EINTR) continue;

    // The client hung up between the readiness report and the accept. Ordinary on a
    // busy server, and specifically not a reason to stop accepting.
    if (errno == ECONNABORTED) continue;

    if (errno == EMFILE || errno == ENFILE) {
      shed_connection();
      return;
    }

    log::error("accept failed: %s", std::strerror(errno));
    return;
  }
}

void Server::shed_connection() {
  // The trap this avoids: the listener is level-triggered and stays readable for as
  // long as a connection sits in the accept queue. If we cannot accept it, wait()
  // returns immediately, forever, and the server spins at 100% CPU serving nobody.
  // The fix is to make sure we can always accept -- even when out of descriptors --
  // by keeping one in reserve to spend on closing the connection politely.
  if (spare_fd_ >= 0) {
    ::close(spare_fd_);
    spare_fd_ = -1;

    const int fd = ::accept(listen_fd_, nullptr, nullptr);
    if (fd >= 0) ::close(fd);

    log::warn("out of file descriptors: rejected a connection (%zu open)", conns_.size());

    if (reopen_spare_fd()) return;
  }

  // The spare could not be reclaimed, so the next EMFILE would spin. Stop watching the
  // listener entirely; a closing connection frees the descriptor that resumes it.
  pause_listener();
}

void Server::pause_listener() {
  if (listener_paused_) return;
  loop_.remove(listen_fd_);
  listener_paused_ = true;
  log::warn("listener paused: no file descriptors available to accept with");
}

void Server::resume_listener() {
  if (!listener_paused_) return;
  if (!reopen_spare_fd()) return;  // still nothing to spare; stay paused

  if (!loop_.add(listen_fd_, false, [this](int, bool readable, bool) {
        if (readable) on_listener_readable();
      })) {
    log::error("failed to re-register the listener after pausing");
    return;
  }
  listener_paused_ = false;
  log::info("listener resumed");
}

void Server::add_connection(int fd) {
  std::string err;
  if (!sock::set_nonblocking(fd, &err)) {
    log::error("rejecting connection: %s", err.c_str());
    ::close(fd);
    return;
  }
  sock::set_tcp_nodelay(fd);
  sock::suppress_sigpipe(fd);

  std::unique_ptr<Conn> conn(new Conn);
  conn->fd = fd;
  conn->peer = sock::peer_name(fd);

  if (!loop_.add(fd, false, [this](int event_fd, bool readable, bool writable) {
        on_conn_event(event_fd, readable, writable);
      })) {
    log::error("failed to register %s with the poller", conn->peer.c_str());
    ::close(fd);
    return;
  }

  log::debug("accepted %s (fd %d, %zu open)", conn->peer.c_str(), fd, conns_.size() + 1);
  conns_[fd] = std::move(conn);
}

void Server::on_conn_event(int fd, bool readable, bool writable) {
  auto it = conns_.find(fd);
  if (it == conns_.end()) return;
  Conn& c = *it->second;

  // Read first: the reply produced here often drains in the same iteration, which is
  // what keeps a request/response round trip down to one read and one write.
  if (readable && !handle_read(c)) return;

  // A writable report means an earlier partial write can continue. handle_read has
  // already tried once, so this only matters for output left over from before.
  if (writable && !c.out.empty() && !flush_output(c)) return;

  if (c.close_after_write && c.out.empty()) close_conn(c, "reply delivered");
}

bool Server::handle_read(Conn& c) {
  c.in.ensure_writable(kReadChunk);
  const ssize_t n = ::read(c.fd, c.in.write_ptr(), kReadChunk);

  if (n == 0) {
    // Orderly close from the peer. Anything half-parsed is discarded: a command the
    // client did not finish sending is a command it did not send.
    close_conn(c, "client closed the connection");
    return false;
  }
  if (n < 0) {
    if (errno == EAGAIN || errno == EWOULDBLOCK) return true;  // spurious readability
    if (errno == EINTR) return true;                           // retried on the next tick
    close_conn(c, std::strerror(errno));
    return false;
  }

  c.in.commit(static_cast<size_t>(n));
  return process_input(c);
}

bool Server::process_input(Conn& c) {
  for (;;) {
    std::string perr;
    const ParseStatus status = c.parser.parse(c.in, &argv_, &perr);

    if (status == ParseStatus::Incomplete) break;

    if (status == ParseStatus::Error) {
      // Framing is lost, so there is no way to find where the next command begins.
      // Send the diagnosis, then close -- guessing would corrupt the client's view.
      log::warn("protocol error from %s: %s", c.peer.c_str(), perr.c_str());
      reply::error(c.out, "ERR " + perr);
      c.close_after_write = true;
      break;
    }

    if (argv_.empty()) continue;  // "*0" and blank inline lines carry no command

    CommandContext ctx{argv_, c.out, db_};
    CommandTable::instance().dispatch(ctx);

    if (ctx.close_after_reply) {
      c.close_after_write = true;
      break;
    }

    // Checked inside the loop, not after it: a pipeline is exactly how a client
    // accumulates gigabytes of replies, and the point of the limit is to stop before
    // the memory is committed rather than to notice afterwards.
    if (c.out.readable() > cfg_.max_output_buffer) {
      log::warn("closing %s: output buffer %zu bytes exceeds the %zu byte limit",
                c.peer.c_str(), c.out.readable(), cfg_.max_output_buffer);
      // Closed outright rather than flushed first: a client that let its replies pile
      // up this far is one that is not reading, so there is nobody to flush to.
      close_conn(c, "output buffer limit exceeded");
      return false;
    }
  }

  return flush_output(c);
}

bool Server::flush_output(Conn& c) {
  while (!c.out.empty()) {
    const ssize_t n = ::send(c.fd, c.out.peek(), c.out.readable(), kSendFlags);
    if (n > 0) {
      c.out.consume(static_cast<size_t>(n));
      continue;
    }
    if (n < 0 && errno == EINTR) continue;
    if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) break;  // socket buffer full

    close_conn(c, std::strerror(errno));
    return false;
  }

  if (c.out.empty()) {
    if (c.close_after_write) {
      close_conn(c, "reply delivered");
      return false;
    }
    // Nothing left to send, so stop asking to be told about writability -- otherwise
    // an idle connection reports writable on every single wait() and the loop spins.
    if (c.write_armed) {
      loop_.set_write_interest(c.fd, false);
      c.write_armed = false;
    }
  } else if (!c.write_armed) {
    // A partial write is the only reason to care about writability at all.
    loop_.set_write_interest(c.fd, true);
    c.write_armed = true;
  }
  return true;
}

void Server::close_conn(Conn& c, const char* reason) {
  const int fd = c.fd;
  log::debug("closing %s (fd %d): %s", c.peer.c_str(), fd, reason);

  // Unregister before close(2): the number can be reissued by the very next accept(),
  // and a stale registration would route the new client's events to a dead callback.
  loop_.remove(fd);
  ::close(fd);
  conns_.erase(fd);  // destroys c

  // The descriptor just freed may be the one that lets accepting resume.
  resume_listener();
}

void Server::on_tick() {
  const size_t reclaimed = db_.active_expire_cycle(cfg_.active_expire_samples);
  if (reclaimed > 0) log::debug("active expiry reclaimed %zu keys", reclaimed);

  if (shutdown_check_ && shutdown_check_()) {
    log::info("shutdown requested; closing %zu connection(s)", conns_.size());
    loop_.stop();
  }
}

}  // namespace kvsd
