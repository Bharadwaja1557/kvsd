// kvsd entry point: parse the command line, install signal handlers, run the loop.
#include <signal.h>

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "net/server.h"
#include "util/log.h"

namespace {

// Written by the signal handler, read by the event loop's tick.
//
// WHY nothing else happens in the handler: it runs between two arbitrary machine
// instructions, possibly in the middle of an allocation or a log write, and almost
// nothing in the standard library is safe there. Writing one sig_atomic_t is, and it
// is enough -- the loop wakes at most one tick interval later (or immediately, since
// the signal interrupts the wait) and shuts down on its own thread, with the
// invariants of every data structure intact.
volatile sig_atomic_t g_shutdown = 0;

void handle_signal(int) { g_shutdown = 1; }

void usage(const char* argv0) {
  std::printf(
      "kvsd -- a RESP2-compatible key-value server\n"
      "\n"
      "usage: %s [options]\n"
      "\n"
      "  --port <n>               port to listen on (default 6380; 0 lets the OS pick)\n"
      "  --bind <addr>            IPv4 address to bind (default 127.0.0.1)\n"
      "  --max-output-buffer <n>  bytes of pending replies per client before it is\n"
      "                           disconnected (default 67108864)\n"
      "  --expire-samples <n>     keys sampled per active expiry round (default 20)\n"
      "  --tick-interval <ms>     event loop tick period (default 100)\n"
      "  --verbose                log per-connection detail\n"
      "  --help                   this text\n",
      argv0);
}

// Strict on purpose: a mistyped limit that silently becomes 0 is worse than a refusal.
bool parse_u64(const char* s, uint64_t max, uint64_t* out) {
  if (s == nullptr || *s == '\0') return false;
  errno = 0;
  char* end = nullptr;
  const unsigned long long v = std::strtoull(s, &end, 10);
  if (errno != 0 || end == s || *end != '\0' || v > max) return false;
  *out = static_cast<uint64_t>(v);
  return true;
}

bool parse_args(int argc, char** argv, kvsd::ServerConfig* cfg, bool* verbose, int* exit_code) {
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    const char* value = (i + 1 < argc) ? argv[i + 1] : nullptr;
    uint64_t n = 0;

    // Reads the argument that follows a numeric option, consuming it on success and
    // reporting which option was wrong -- not just that "something" was -- on failure.
    auto take_number = [&](uint64_t lo, uint64_t hi) {
      if (!parse_u64(value, hi, &n) || n < lo) {
        std::fprintf(stderr, "kvsd: %s expects an integer in [%llu, %llu], got '%s'\n",
                     arg.c_str(), static_cast<unsigned long long>(lo),
                     static_cast<unsigned long long>(hi), value == nullptr ? "" : value);
        return false;
      }
      ++i;
      return true;
    };

    if (arg == "--help" || arg == "-h") {
      usage(argv[0]);
      *exit_code = 0;
      return false;
    }

    if (arg == "--verbose") {
      *verbose = true;
    } else if (arg == "--bind") {
      if (value == nullptr) {
        std::fprintf(stderr, "kvsd: --bind expects an IPv4 address\n");
        *exit_code = 2;
        return false;
      }
      cfg->bind_addr = value;
      ++i;
    } else if (arg == "--port") {
      if (!take_number(0, 65535)) { *exit_code = 2; return false; }
      cfg->port = static_cast<uint16_t>(n);
    } else if (arg == "--max-output-buffer") {
      if (!take_number(1, 1ull << 40)) { *exit_code = 2; return false; }
      cfg->max_output_buffer = static_cast<size_t>(n);
    } else if (arg == "--expire-samples") {
      if (!take_number(0, 100000)) { *exit_code = 2; return false; }
      cfg->active_expire_samples = static_cast<size_t>(n);
    } else if (arg == "--tick-interval") {
      if (!take_number(1, 60000)) { *exit_code = 2; return false; }
      cfg->tick_interval_ms = static_cast<int>(n);
    } else {
      std::fprintf(stderr, "kvsd: unknown argument '%s'\n\n", arg.c_str());
      usage(argv[0]);
      *exit_code = 2;
      return false;
    }
  }
  return true;
}

void install_signal_handlers() {
  struct sigaction sa;
  std::memset(&sa, 0, sizeof(sa));
  sa.sa_handler = handle_signal;
  sigemptyset(&sa.sa_mask);
  // No SA_RESTART: the point is for the signal to interrupt the poller's wait so that
  // shutdown is immediate rather than up to one tick late. The poller reports an
  // interrupted wait as zero events, so the loop simply runs its tick and exits.
  sa.sa_flags = 0;
  ::sigaction(SIGINT, &sa, nullptr);
  ::sigaction(SIGTERM, &sa, nullptr);

  // Belt and braces with sock::suppress_sigpipe: writing to a peer that has gone away
  // must never take the process down.
  ::signal(SIGPIPE, SIG_IGN);
}

}  // namespace

int main(int argc, char** argv) {
  kvsd::ServerConfig cfg;
  bool verbose = false;
  int exit_code = 0;
  if (!parse_args(argc, argv, &cfg, &verbose, &exit_code)) return exit_code;

  if (verbose) kvsd::log::set_min_level(kvsd::log::Level::Debug);
  install_signal_handlers();

  kvsd::Server server(cfg);
  std::string err;
  if (!server.start(&err)) {
    kvsd::log::error("startup failed: %s", err.c_str());
    return 1;
  }
  server.set_shutdown_check([] { return g_shutdown != 0; });

  server.run();
  kvsd::log::info("kvsd stopped");
  return 0;
}
