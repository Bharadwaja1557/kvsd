#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "net/buffer.h"

namespace kvsd {

// Strict integer parsing, the same shape Redis uses in string2ll: no leading '+', no
// whitespace, no partial consumption, and overflow is a failure rather than a clamp.
// Shared by the protocol (bulk lengths, argument counts) and by the commands that
// interpret stored values as numbers (INCR, DECR, SET EX).
bool string_to_int64(const char* p, size_t n, int64_t* out);
inline bool string_to_int64(const std::string& s, int64_t* out) {
  return string_to_int64(s.data(), s.size(), out);
}

enum class ParseStatus {
  // Not enough bytes yet. Whatever was consumed stays consumed and the parser holds
  // the partial command; call again after the next read().
  Incomplete,
  // One command is in argv. May legitimately be empty (a "*0" or a blank inline line),
  // which the dispatcher skips.
  Complete,
  // The client violated the protocol. The reply is an error and then a close: once the
  // framing is wrong there is no way to find where the next command starts.
  Error,
};

// Ceilings that make the parser safe to point at the open internet. Without them a
// four-byte header ("*9999999999") can ask the server to commit unbounded memory.
// The defaults match Redis so a client that works against redis-server works here.
struct ProtocolLimits {
  size_t max_multibulk_args = 1024 * 1024;
  size_t max_bulk_len = 512u * 1024 * 1024;
  size_t max_inline_len = 64 * 1024;
};

// An incremental RESP2 parser. One instance lives on each connection for the life of
// that connection.
//
// The invariant that makes it incremental: bytes are consumed from the buffer as soon
// as they form a complete token, and everything needed to resume mid-command lives in
// the members below. A parser that instead re-scanned the buffer from byte zero on
// every read would be O(n^2) in the number of reads -- a 512 MiB bulk string arriving
// in 16 KiB chunks would be rescanned 32768 times.
class RespParser {
 public:
  RespParser() = default;
  explicit RespParser(const ProtocolLimits& limits) : limits_(limits) {}

  // Consumes at most one command from in. See ParseStatus. On Error, err receives the
  // message text as Redis phrases it ("Protocol error: ...") without the "-ERR " prefix.
  ParseStatus parse(Buffer& in, std::vector<std::string>* argv, std::string* err);

  void reset();

  const ProtocolLimits& limits() const { return limits_; }

 private:
  enum class State {
    // Between commands: the next byte decides multibulk ('*') or inline (anything else).
    Start,
    // Inside a multibulk, expecting a "$<len>" header.
    ArgLen,
    // Inside a multibulk, expecting <len> bytes of payload plus CRLF.
    ArgData,
  };

  ParseStatus fail(std::string* err, const std::string& msg);

  State state_ = State::Start;
  int64_t args_remaining_ = 0;  // arguments still expected in the current multibulk
  int64_t arg_len_ = -1;        // declared length of the bulk argument being read
  std::vector<std::string> argv_;  // arguments completed so far for the in-flight command
  ProtocolLimits limits_;
};

}  // namespace kvsd
