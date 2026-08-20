#include "resp/writer.h"

#include <cstring>

namespace kvsd {
namespace reply {
namespace {

// Formats n into buf and returns the number of bytes written. Hand-rolled rather than
// std::to_string because every INCR and every array header goes through here, and
// to_string allocates a std::string to throw away.
size_t format_int(int64_t n, char* buf) {
  if (n == 0) {
    buf[0] = '0';
    return 1;
  }
  // Negate in unsigned space: -INT64_MIN is undefined in signed arithmetic.
  const bool negative = n < 0;
  uint64_t v = negative ? (~static_cast<uint64_t>(n) + 1) : static_cast<uint64_t>(n);

  char tmp[20];
  size_t len = 0;
  while (v > 0) {
    tmp[len++] = static_cast<char>('0' + (v % 10));
    v /= 10;
  }

  size_t out = 0;
  if (negative) buf[out++] = '-';
  while (len > 0) buf[out++] = tmp[--len];
  return out;
}

void append_int_crlf(Buffer& out, int64_t n) {
  char buf[24];
  const size_t len = format_int(n, buf);
  buf[len] = '\r';
  buf[len + 1] = '\n';
  out.append(buf, len + 2);
}

}  // namespace

void simple(Buffer& out, const char* s) {
  const size_t n = std::strlen(s);
  out.ensure_writable(n + 3);
  out.append('+');
  out.append(s, n);
  out.append("\r\n", 2);
}

void simple(Buffer& out, const std::string& s) {
  out.ensure_writable(s.size() + 3);
  out.append('+');
  out.append(s);
  out.append("\r\n", 2);
}

void error(Buffer& out, const std::string& msg) {
  out.ensure_writable(msg.size() + 3);
  out.append('-');
  out.append(msg);
  out.append("\r\n", 2);
}

void integer(Buffer& out, int64_t n) {
  out.append(':');
  append_int_crlf(out, n);
}

void bulk(Buffer& out, const char* data, size_t len) {
  // One reservation for the whole reply so a large value costs at most one growth.
  out.ensure_writable(len + 32);
  out.append('$');
  append_int_crlf(out, static_cast<int64_t>(len));
  out.append(data, len);
  out.append("\r\n", 2);
}

void bulk(Buffer& out, const std::string& s) { bulk(out, s.data(), s.size()); }

void null_bulk(Buffer& out) { out.append("$-1\r\n", 5); }

void array_header(Buffer& out, int64_t n) {
  out.append('*');
  append_int_crlf(out, n);
}

void ok(Buffer& out) { out.append("+OK\r\n", 5); }

}  // namespace reply
}  // namespace kvsd
