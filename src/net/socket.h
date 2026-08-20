#pragma once

#include <cstdint>
#include <string>

namespace kvsd {

// Thin wrappers over the POSIX calls the server needs. They exist so the event loop
// and the accept path read as protocol logic rather than as a wall of setsockopt.
namespace sock {

// Creates a non-blocking, SO_REUSEADDR listening socket bound to bind_addr:port.
// Returns the fd, or -1 with err set to a human-readable reason.
int listen_on(const std::string& bind_addr, uint16_t port, int backlog, std::string* err);

bool set_nonblocking(int fd, std::string* err);

// Disables Nagle. WHY: RESP is a small-request/small-response protocol, so Nagle
// would hold a finished reply waiting for more bytes that only arrive once the
// client has seen that reply -- a self-inflicted 40 ms round trip.
bool set_tcp_nodelay(int fd);

// Returns "ip:port" for logging, or "?" if it cannot be determined.
std::string peer_name(int fd);

// The port the socket is actually bound to, which is only interesting when the
// requested port was 0 and the kernel chose one -- as the tests do, so that a stray
// server from an earlier run cannot make a later run fail on EADDRINUSE.
uint16_t local_port(int fd);

// Makes a write to a peer that has gone away fail with EPIPE instead of killing the
// process with SIGPIPE.
//
// WHY per socket rather than only ignoring the signal process-wide: SIGPIPE's default
// disposition is inherited and global, so a library that relies on the main program
// having disabled it is a library that dies in a program that did not. macOS/BSD have
// SO_NOSIGPIPE for exactly this; on Linux there is no socket option and the equivalent
// is MSG_NOSIGNAL on each send(), which is what the write path passes there. main.cpp
// also sets SIG_IGN, so the two mechanisms cover each other.
bool suppress_sigpipe(int fd);

}  // namespace sock
}  // namespace kvsd
