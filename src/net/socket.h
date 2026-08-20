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

}  // namespace sock
}  // namespace kvsd
