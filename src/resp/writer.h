#pragma once

#include <cstdint>
#include <string>

#include "net/buffer.h"

namespace kvsd {

// RESP2 reply encoders. Every one appends to the connection's output buffer rather
// than writing to the socket, because a pipelined batch must produce one write() for
// N replies, not N writes -- and because a command handler has no business knowing
// whether the socket is currently writable.
namespace reply {

// +<s>\r\n -- for status replies like OK and PONG.
void simple(Buffer& out, const char* s);
void simple(Buffer& out, const std::string& s);

// -<msg>\r\n. msg must already carry its error code, e.g. "ERR unknown command 'x'".
void error(Buffer& out, const std::string& msg);

// :<n>\r\n
void integer(Buffer& out, int64_t n);

// $<len>\r\n<data>\r\n -- binary safe.
void bulk(Buffer& out, const std::string& s);
void bulk(Buffer& out, const char* data, size_t len);

// $-1\r\n -- the RESP2 null, which is how "no such key" is spelled.
void null_bulk(Buffer& out);

// *<n>\r\n, followed by n element replies written by the caller.
void array_header(Buffer& out, int64_t n);

void ok(Buffer& out);

}  // namespace reply
}  // namespace kvsd
