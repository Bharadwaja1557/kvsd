#include "resp/parser.h"

#include <cctype>
#include <cstring>
#include <utility>

namespace kvsd {
namespace {

// Returns a pointer to the '\r' of the first CRLF in [p, p+n), or nullptr.
const char* find_crlf(const char* p, size_t n) {
  const char* end = p + n;
  while (p + 1 < end) {
    const char* r = static_cast<const char*>(
        std::memchr(p, '\r', static_cast<size_t>(end - p - 1)));
    if (r == nullptr) return nullptr;
    if (r[1] == '\n') return r;
    p = r + 1;
  }
  return nullptr;
}

bool is_hex(char c) {
  return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

int hex_val(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  return c - 'A' + 10;
}

bool is_space(char c) { return std::isspace(static_cast<unsigned char>(c)) != 0; }

// Splits an inline command the way redis-cli and a human with telnet would expect:
// whitespace-separated, with double-quoted strings supporting \xHH and the usual
// escapes, and single-quoted strings taking everything literally except \'.
// Returns false on an unterminated or badly terminated quote.
bool split_inline(const char* p, size_t n, std::vector<std::string>* out) {
  out->clear();
  size_t i = 0;
  while (true) {
    while (i < n && is_space(p[i])) ++i;
    if (i >= n) return true;

    std::string cur;
    bool in_double = false;
    bool in_single = false;
    bool done = false;

    while (!done) {
      if (in_double) {
        if (i >= n) return false;  // unterminated
        if (p[i] == '\\' && i + 3 < n && p[i + 1] == 'x' && is_hex(p[i + 2]) &&
            is_hex(p[i + 3])) {
          cur += static_cast<char>((hex_val(p[i + 2]) << 4) | hex_val(p[i + 3]));
          i += 4;
        } else if (p[i] == '\\' && i + 1 < n) {
          char c = p[i + 1];
          switch (c) {
            case 'n': c = '\n'; break;
            case 'r': c = '\r'; break;
            case 't': c = '\t'; break;
            case 'b': c = '\b'; break;
            case 'a': c = '\a'; break;
            default: break;  // \" \\ and anything else stand for themselves
          }
          cur += c;
          i += 2;
        } else if (p[i] == '"') {
          // A closing quote must end the word. `"a"b` is a client bug, not an argument.
          if (i + 1 < n && !is_space(p[i + 1])) return false;
          ++i;
          done = true;
        } else {
          cur += p[i++];
        }
      } else if (in_single) {
        if (i >= n) return false;
        if (p[i] == '\\' && i + 1 < n && p[i + 1] == '\'') {
          cur += '\'';
          i += 2;
        } else if (p[i] == '\'') {
          if (i + 1 < n && !is_space(p[i + 1])) return false;
          ++i;
          done = true;
        } else {
          cur += p[i++];
        }
      } else {
        if (i >= n) {
          done = true;
        } else if (is_space(p[i])) {
          ++i;
          done = true;
        } else if (p[i] == '"') {
          in_double = true;
          ++i;
        } else if (p[i] == '\'') {
          in_single = true;
          ++i;
        } else {
          cur += p[i++];
        }
      }
    }
    out->push_back(std::move(cur));
  }
}

}  // namespace

bool string_to_int64(const char* p, size_t n, int64_t* out) {
  if (n == 0 || n > 20) return false;

  bool negative = false;
  size_t i = 0;
  if (p[0] == '-') {
    negative = true;
    i = 1;
    if (n == 1) return false;
  }

  uint64_t v = 0;
  for (; i < n; ++i) {
    if (p[i] < '0' || p[i] > '9') return false;
    const uint64_t d = static_cast<uint64_t>(p[i] - '0');
    if (v > (UINT64_MAX - d) / 10) return false;
    v = v * 10 + d;
  }

  const uint64_t limit =
      negative ? static_cast<uint64_t>(INT64_MAX) + 1 : static_cast<uint64_t>(INT64_MAX);
  if (v > limit) return false;

  if (negative) {
    *out = (v == static_cast<uint64_t>(INT64_MAX) + 1) ? INT64_MIN
                                                       : -static_cast<int64_t>(v);
  } else {
    *out = static_cast<int64_t>(v);
  }
  return true;
}

void RespParser::reset() {
  state_ = State::Start;
  args_remaining_ = 0;
  arg_len_ = -1;
  argv_.clear();
}

ParseStatus RespParser::fail(std::string* err, const std::string& msg) {
  if (err) *err = "Protocol error: " + msg;
  reset();
  return ParseStatus::Error;
}

ParseStatus RespParser::parse(Buffer& in, std::vector<std::string>* argv, std::string* err) {
  for (;;) {
    switch (state_) {
      case State::Start: {
        if (in.readable() == 0) return ParseStatus::Incomplete;

        if (in.peek()[0] != '*') {
          // Inline command. Terminated by a bare '\n'; a preceding '\r' is optional so
          // that `telnet` and `nc` both work.
          const char* nl =
              static_cast<const char*>(std::memchr(in.peek(), '\n', in.readable()));
          if (nl == nullptr) {
            if (in.readable() > limits_.max_inline_len) {
              return fail(err, "too big inline request");
            }
            return ParseStatus::Incomplete;
          }
          size_t line_len = static_cast<size_t>(nl - in.peek());
          const size_t total = line_len + 1;
          if (line_len > 0 && in.peek()[line_len - 1] == '\r') --line_len;
          if (line_len > limits_.max_inline_len) {
            return fail(err, "too big inline request");
          }

          std::vector<std::string> parts;
          if (!split_inline(in.peek(), line_len, &parts)) {
            in.consume(total);
            return fail(err, "unbalanced quotes in request");
          }
          in.consume(total);
          *argv = std::move(parts);
          reset();
          return ParseStatus::Complete;
        }

        const char* crlf = find_crlf(in.peek(), in.readable());
        if (crlf == nullptr) {
          // A header line this long is not a slow client, it is a client sending junk.
          if (in.readable() > 64) return fail(err, "too big mbulk count string");
          return ParseStatus::Incomplete;
        }

        const size_t line_len = static_cast<size_t>(crlf - in.peek());
        int64_t count = 0;
        if (!string_to_int64(in.peek() + 1, line_len - 1, &count)) {
          return fail(err, "invalid multibulk length");
        }
        if (count > static_cast<int64_t>(limits_.max_multibulk_args)) {
          return fail(err, "invalid multibulk length");
        }
        in.consume(line_len + 2);

        if (count <= 0) {
          // "*0" and the null array "*-1" are well-formed but carry no command. Redis
          // treats them as a no-op rather than an error, and so do we.
          argv->clear();
          reset();
          return ParseStatus::Complete;
        }

        args_remaining_ = count;
        argv_.clear();
        // Reserve for the common case only: `count` is attacker-controlled up to 1M,
        // and reserving that many std::strings would be a 32 MiB allocation on demand.
        argv_.reserve(count < 16 ? static_cast<size_t>(count) : size_t(16));
        state_ = State::ArgLen;
        break;
      }

      case State::ArgLen: {
        const char* crlf = find_crlf(in.peek(), in.readable());
        if (crlf == nullptr) {
          if (in.readable() > 64) return fail(err, "too big bulk count string");
          return ParseStatus::Incomplete;
        }
        const size_t line_len = static_cast<size_t>(crlf - in.peek());
        if (line_len == 0 || in.peek()[0] != '$') {
          const char got = (line_len == 0) ? ' ' : in.peek()[0];
          return fail(err, std::string("expected '$', got '") + got + "'");
        }
        int64_t len = 0;
        if (!string_to_int64(in.peek() + 1, line_len - 1, &len) || len < 0 ||
            len > static_cast<int64_t>(limits_.max_bulk_len)) {
          return fail(err, "invalid bulk length");
        }
        in.consume(line_len + 2);
        arg_len_ = len;
        state_ = State::ArgData;
        break;
      }

      case State::ArgData: {
        const size_t need = static_cast<size_t>(arg_len_) + 2;  // payload + CRLF
        if (in.readable() < need) return ParseStatus::Incomplete;

        const char* p = in.peek();
        if (p[arg_len_] != '\r' || p[arg_len_ + 1] != '\n') {
          // The declared length and the framing disagree; there is no safe resync point.
          return fail(err, "invalid bulk length");
        }
        // Binary safe by construction: the payload is copied by length, never scanned
        // for a terminator, so embedded NULs and CRLFs are ordinary bytes.
        argv_.emplace_back(p, static_cast<size_t>(arg_len_));
        in.consume(need);
        arg_len_ = -1;

        if (--args_remaining_ == 0) {
          *argv = std::move(argv_);
          reset();
          return ParseStatus::Complete;
        }
        state_ = State::ArgLen;
        break;
      }
    }
  }
}

}  // namespace kvsd
