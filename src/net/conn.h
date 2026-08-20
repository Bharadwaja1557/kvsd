#pragma once

#include <string>

#include "net/buffer.h"
#include "resp/parser.h"

namespace kvsd {

// Everything the server knows about one client socket.
//
// The two buffers are what make the loop non-blocking in both directions: `in` holds
// bytes the kernel handed us that do not yet form a whole command, and `out` holds
// replies the kernel would not take yet. Neither the parser nor a command handler ever
// has to wait for a socket, because neither one ever touches a socket.
struct Conn {
  int fd = -1;
  std::string peer;  // "ip:port", for logs only

  Buffer in;
  Buffer out;

  // Parser state lives per connection because a command may be split across reads and
  // the resumption point belongs to the client that sent the fragment.
  RespParser parser;

  // The reply now queued is the last one this connection will get: QUIT, a protocol
  // error, or a soft shutdown. The socket closes once `out` has drained, never before,
  // or the client loses the reply it is waiting for.
  bool close_after_write = false;

  // Whether write readiness is currently armed in the poller. Tracked so the common
  // case -- a reply the socket accepts immediately -- costs zero poller syscalls; see
  // "Why the write path is optimistic" in docs/DESIGN.md.
  bool write_armed = false;
};

}  // namespace kvsd
