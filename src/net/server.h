#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "db/db.h"
#include "net/conn.h"
#include "net/event_loop.h"

namespace kvsd {

struct ServerConfig {
  std::string bind_addr = "127.0.0.1";
  uint16_t port = 6380;

  // Kernel accept queue depth. 511 rather than 512 because Redis uses 511 and the
  // kernel adds one internally on some systems; the number matters only for bursts.
  int backlog = 511;

  // Hard ceiling on one connection's pending replies. See "Why an output buffer limit
  // is not optional" in docs/DESIGN.md.
  size_t max_output_buffer = 64u * 1024 * 1024;

  // How long the loop may sleep, and therefore the worst-case delay before an expiry
  // cycle runs or a shutdown signal is noticed.
  int tick_interval_ms = 100;

  // Keys examined per expiry round. Larger reclaims memory sooner and costs more time
  // per tick; this is the knob that trades one against the other.
  size_t active_expire_samples = 20;
};

// Owns the listener, the connection table, and the keyspace, and wires them to the
// event loop. Single-threaded by construction: every member below is touched only from
// the loop thread, which is why none of them is synchronized.
//
// The owner is responsible for process-wide signal policy (main.cpp ignores SIGPIPE);
// this class deliberately does not reach out and change it.
class Server {
 public:
  explicit Server(ServerConfig config);
  ~Server();
  Server(const Server&) = delete;
  Server& operator=(const Server&) = delete;

  // Binds, listens, and registers with the loop. On failure err explains why and the
  // server is left in a state where the caller can only destroy it.
  bool start(std::string* err);

  // Runs until stop() or until the shutdown check returns true. Returns when the loop
  // has exited; connections are closed by the destructor.
  void run();

  void stop() { loop_.stop(); }

  // Polled once per tick. A signal handler can do nothing but set a flag, so this is
  // how that flag becomes an orderly shutdown on the loop thread rather than a
  // longjmp out of whatever the loop was doing.
  void set_shutdown_check(std::function<bool()> check) { shutdown_check_ = std::move(check); }

  // The port actually bound, which differs from config.port only when that was 0.
  uint16_t bound_port() const;

  Db& db() { return db_; }
  size_t connection_count() const { return conns_.size(); }

 private:
  void on_listener_readable();
  void on_conn_event(int fd, bool readable, bool writable);

  // Each returns false if it closed (and destroyed) the connection, in which case the
  // caller must not touch it again.
  bool handle_read(Conn& c);
  bool process_input(Conn& c);
  bool flush_output(Conn& c);

  void add_connection(int fd);
  void close_conn(Conn& c, const char* reason);

  // The EMFILE path: give up the reserved descriptor so there is one to accept with,
  // accept the pending connection, and close it immediately.
  void shed_connection();
  bool reopen_spare_fd();
  void pause_listener();
  void resume_listener();

  void on_tick();

  ServerConfig cfg_;
  EventLoop loop_;
  Db db_;

  int listen_fd_ = -1;

  // A descriptor held open for no reason other than to have one to spend when accept()
  // fails with EMFILE. See docs/DESIGN.md, "Running out of file descriptors".
  int spare_fd_ = -1;

  bool listener_paused_ = false;

  std::unordered_map<int, std::unique_ptr<Conn>> conns_;

  // Reused across commands so that dispatching does not allocate a vector per command.
  std::vector<std::string> argv_;

  std::function<bool()> shutdown_check_;
};

}  // namespace kvsd
