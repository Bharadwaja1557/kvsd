#include "resp/parser.h"

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

#include "net/buffer.h"
#include "test_util.h"

using kvsd::Buffer;
using kvsd::ParseStatus;
using kvsd::RespParser;

namespace {

using Argv = std::vector<std::string>;

std::string encode(const Argv& args) {
  std::string s = "*" + std::to_string(args.size()) + "\r\n";
  for (const auto& a : args) {
    s += "$" + std::to_string(a.size()) + "\r\n" + a + "\r\n";
  }
  return s;
}

// Feeds the whole input at once and returns the first parse result.
ParseStatus parse_one(const std::string& input, Argv* argv, std::string* err) {
  Buffer b;
  b.append(input);
  RespParser p;
  return p.parse(b, argv, err);
}

std::string join(const Argv& v) {
  std::string s;
  for (size_t i = 0; i < v.size(); ++i) {
    if (i) s += "|";
    s += v[i];
  }
  return s;
}

}  // namespace

static void test_single_command() {
  Argv argv;
  std::string err;
  CHECK(parse_one(encode({"SET", "key", "value"}), &argv, &err) == ParseStatus::Complete);
  CHECK_EQ(join(argv), std::string("SET|key|value"));
  CHECK_EQ(err, std::string(""));
}

static void test_split_one_byte_at_a_time() {
  // The core incremental property: the parser must survive the command arriving in
  // the worst possible chunking, one byte per read().
  const std::string wire = encode({"SET", "counter", "41"});
  Buffer b;
  RespParser p;
  Argv argv;
  std::string err;

  for (size_t i = 0; i + 1 < wire.size(); ++i) {
    b.append(&wire[i], 1);
    CHECK(p.parse(b, &argv, &err) == ParseStatus::Incomplete);
  }
  b.append(&wire[wire.size() - 1], 1);
  CHECK(p.parse(b, &argv, &err) == ParseStatus::Complete);
  CHECK_EQ(join(argv), std::string("SET|counter|41"));
  CHECK(b.empty());
}

static void test_split_at_every_boundary() {
  // Every possible two-chunk split of the same command must produce the same result.
  const std::string wire = encode({"GET", "some-key"});
  for (size_t cut = 0; cut <= wire.size(); ++cut) {
    Buffer b;
    RespParser p;
    Argv argv;
    std::string err;

    b.append(wire.substr(0, cut));
    ParseStatus st = p.parse(b, &argv, &err);
    if (cut < wire.size()) {
      CHECK(st == ParseStatus::Incomplete);
      b.append(wire.substr(cut));
      st = p.parse(b, &argv, &err);
    }
    CHECK(st == ParseStatus::Complete);
    CHECK_EQ(join(argv), std::string("GET|some-key"));
    CHECK(b.empty());
  }
}

static void test_pipelined_commands_in_one_read() {
  Buffer b;
  b.append(encode({"PING"}) + encode({"ECHO", "hi"}) + encode({"GET", "k"}));
  RespParser p;
  Argv argv;
  std::string err;

  CHECK(p.parse(b, &argv, &err) == ParseStatus::Complete);
  CHECK_EQ(join(argv), std::string("PING"));
  CHECK(p.parse(b, &argv, &err) == ParseStatus::Complete);
  CHECK_EQ(join(argv), std::string("ECHO|hi"));
  CHECK(p.parse(b, &argv, &err) == ParseStatus::Complete);
  CHECK_EQ(join(argv), std::string("GET|k"));
  CHECK(p.parse(b, &argv, &err) == ParseStatus::Incomplete);
  CHECK(b.empty());
}

static void test_pipelined_with_a_trailing_partial() {
  // The realistic pipelining case: N whole commands and the front of an N+1th.
  Buffer b;
  const std::string tail = encode({"SET", "a", "b"});
  b.append(encode({"PING"}) + tail.substr(0, 7));
  RespParser p;
  Argv argv;
  std::string err;

  CHECK(p.parse(b, &argv, &err) == ParseStatus::Complete);
  CHECK_EQ(join(argv), std::string("PING"));
  CHECK(p.parse(b, &argv, &err) == ParseStatus::Incomplete);

  b.append(tail.substr(7));
  CHECK(p.parse(b, &argv, &err) == ParseStatus::Complete);
  CHECK_EQ(join(argv), std::string("SET|a|b"));
}

static void test_binary_safe_payload() {
  const std::string payload = std::string("a\0b\r\nc", 6);
  Argv argv;
  std::string err;
  CHECK(parse_one(encode({"SET", "k", payload}), &argv, &err) == ParseStatus::Complete);
  CHECK_EQ(argv.size(), size_t(3));
  CHECK_EQ(argv[2], payload);
}

static void test_empty_bulk_argument() {
  Argv argv;
  std::string err;
  CHECK(parse_one(encode({"SET", "k", ""}), &argv, &err) == ParseStatus::Complete);
  CHECK_EQ(argv.size(), size_t(3));
  CHECK_EQ(argv[2], std::string(""));
}

static void test_null_and_zero_arrays_are_no_ops() {
  Argv argv;
  std::string err;
  CHECK(parse_one("*0\r\n", &argv, &err) == ParseStatus::Complete);
  CHECK_EQ(argv.size(), size_t(0));
  CHECK(parse_one("*-1\r\n", &argv, &err) == ParseStatus::Complete);
  CHECK_EQ(argv.size(), size_t(0));
}

static void test_parser_recovers_between_commands_after_no_op() {
  Buffer b;
  b.append(std::string("*0\r\n") + encode({"PING"}));
  RespParser p;
  Argv argv;
  std::string err;
  CHECK(p.parse(b, &argv, &err) == ParseStatus::Complete);
  CHECK_EQ(argv.size(), size_t(0));
  CHECK(p.parse(b, &argv, &err) == ParseStatus::Complete);
  CHECK_EQ(join(argv), std::string("PING"));
}

// ---------------------------------------------------------------- malformed input

static void expect_error(const std::string& input, const std::string& want_substr) {
  Argv argv;
  std::string err;
  ParseStatus st = parse_one(input, &argv, &err);
  CHECK(st == ParseStatus::Error);
  if (st == ParseStatus::Error && err.find(want_substr) == std::string::npos) {
    kvsd_test::report(__FILE__, __LINE__, "error text", "got " + kvsd_test::show(err) +
                                                            ", want substring " +
                                                            kvsd_test::show(want_substr));
  }
}

static void test_malformed_inputs() {
  expect_error("*abc\r\n", "invalid multibulk length");
  expect_error("*\r\n", "invalid multibulk length");
  expect_error("*3\r\n*2\r\n", "expected '$', got '*'");
  expect_error("*1\r\n+OK\r\n", "expected '$', got '+'");
  expect_error("*1\r\n$xyz\r\n", "invalid bulk length");
  expect_error("*1\r\n$-1\r\n", "invalid bulk length");
  // Declared length disagrees with where the terminator actually lands: 2 bytes
  // promised, so byte 2 must be '\r' and is 'c'.
  expect_error("*1\r\n$2\r\nabc\r\n", "invalid bulk length");
  // A header line that never terminates is junk, not a slow client.
  expect_error("*" + std::string(200, '9'), "too big mbulk count string");
  expect_error("*1\r\n$" + std::string(200, '9'), "too big bulk count string");
}

static void test_multibulk_count_over_limit() {
  expect_error("*1048577\r\n", "invalid multibulk length");
  // Just inside the limit is accepted as a header and then waits for arguments.
  Argv argv;
  std::string err;
  CHECK(parse_one("*1048576\r\n", &argv, &err) == ParseStatus::Incomplete);
}

static void test_error_resets_parser_state() {
  // After an error the connection is closed, but the parser must not be left holding a
  // half-command that would corrupt a reused instance.
  Buffer b;
  b.append("*1\r\n+OK\r\n");
  RespParser p;
  Argv argv;
  std::string err;
  CHECK(p.parse(b, &argv, &err) == ParseStatus::Error);

  b.consume_all();
  b.append(encode({"PING"}));
  CHECK(p.parse(b, &argv, &err) == ParseStatus::Complete);
  CHECK_EQ(join(argv), std::string("PING"));
}

// ------------------------------------------------------------------- huge bulks

static void test_declared_bulk_over_limit_is_rejected() {
  // 512 MiB + 1. The point is that this costs the server four bytes of memory to
  // refuse, not half a gigabyte to discover.
  expect_error("*1\r\n$536870913\r\n", "invalid bulk length");
  expect_error("*1\r\n$99999999999999999999999\r\n", "invalid bulk length");
}

static void test_large_bulk_arrives_in_chunks() {
  // 1 MiB payload delivered in 16 KiB reads, the shape a real large SET takes.
  const size_t kSize = 1u << 20;
  std::string payload(kSize, '\0');
  for (size_t i = 0; i < kSize; ++i) payload[i] = static_cast<char>(i & 0xff);
  const std::string wire = encode({"SET", "big", payload});

  Buffer b;
  RespParser p;
  Argv argv;
  std::string err;
  const size_t chunk = 16 * 1024;
  size_t off = 0;
  ParseStatus st = ParseStatus::Incomplete;
  while (off < wire.size()) {
    const size_t n = std::min(chunk, wire.size() - off);
    b.append(wire.data() + off, n);
    off += n;
    st = p.parse(b, &argv, &err);
    if (off < wire.size()) CHECK(st == ParseStatus::Incomplete);
  }
  CHECK(st == ParseStatus::Complete);
  CHECK_EQ(argv.size(), size_t(3));
  CHECK_EQ(argv[2].size(), kSize);
  CHECK(argv[2] == payload);
}

static void test_bulk_at_exactly_the_limit_header_is_accepted() {
  Argv argv;
  std::string err;
  // Accepting the header is correct; the payload simply never arrives in this test.
  CHECK(parse_one("*1\r\n$536870912\r\n", &argv, &err) == ParseStatus::Incomplete);
}

// ----------------------------------------------------------------- inline commands

static void test_inline_basic() {
  Argv argv;
  std::string err;
  CHECK(parse_one("PING\r\n", &argv, &err) == ParseStatus::Complete);
  CHECK_EQ(join(argv), std::string("PING"));

  CHECK(parse_one("SET foo bar\n", &argv, &err) == ParseStatus::Complete);
  CHECK_EQ(join(argv), std::string("SET|foo|bar"));

  CHECK(parse_one("  GET   spaced   \r\n", &argv, &err) == ParseStatus::Complete);
  CHECK_EQ(join(argv), std::string("GET|spaced"));
}

static void test_inline_empty_line_is_a_no_op() {
  Argv argv;
  std::string err;
  CHECK(parse_one("\r\n", &argv, &err) == ParseStatus::Complete);
  CHECK_EQ(argv.size(), size_t(0));
  CHECK(parse_one("\n", &argv, &err) == ParseStatus::Complete);
  CHECK_EQ(argv.size(), size_t(0));
}

static void test_inline_quoting() {
  Argv argv;
  std::string err;
  CHECK(parse_one("SET k \"hello world\"\r\n", &argv, &err) == ParseStatus::Complete);
  CHECK_EQ(join(argv), std::string("SET|k|hello world"));

  CHECK(parse_one("SET k 'single quoted'\r\n", &argv, &err) == ParseStatus::Complete);
  CHECK_EQ(join(argv), std::string("SET|k|single quoted"));

  CHECK(parse_one("SET k \"tab\\there\"\r\n", &argv, &err) == ParseStatus::Complete);
  CHECK_EQ(argv[2], std::string("tab\there"));

  CHECK(parse_one("SET k \"\\x41\\x42\"\r\n", &argv, &err) == ParseStatus::Complete);
  CHECK_EQ(argv[2], std::string("AB"));

  CHECK(parse_one("SET k \"embedded \\\" quote\"\r\n", &argv, &err) == ParseStatus::Complete);
  CHECK_EQ(argv[2], std::string("embedded \" quote"));
}

static void test_inline_unbalanced_quotes() {
  expect_error("SET k \"unterminated\r\n", "unbalanced quotes");
  expect_error("SET k 'unterminated\r\n", "unbalanced quotes");
  expect_error("SET k \"abc\"def\r\n", "unbalanced quotes");
}

static void test_inline_split_across_reads() {
  Buffer b;
  RespParser p;
  Argv argv;
  std::string err;
  b.append("PI");
  CHECK(p.parse(b, &argv, &err) == ParseStatus::Incomplete);
  b.append("NG\r");
  CHECK(p.parse(b, &argv, &err) == ParseStatus::Incomplete);
  b.append("\n");
  CHECK(p.parse(b, &argv, &err) == ParseStatus::Complete);
  CHECK_EQ(join(argv), std::string("PING"));
}

static void test_inline_over_length_limit() {
  expect_error(std::string(70 * 1024, 'x'), "too big inline request");
}

static void test_inline_then_multibulk_on_same_connection() {
  Buffer b;
  b.append("PING\r\n" + encode({"ECHO", "x"}));
  RespParser p;
  Argv argv;
  std::string err;
  CHECK(p.parse(b, &argv, &err) == ParseStatus::Complete);
  CHECK_EQ(join(argv), std::string("PING"));
  CHECK(p.parse(b, &argv, &err) == ParseStatus::Complete);
  CHECK_EQ(join(argv), std::string("ECHO|x"));
}

// --------------------------------------------------------------- integer parsing

static void test_string_to_int64() {
  int64_t v = 0;
  CHECK(kvsd::string_to_int64("0", &v) && v == 0);
  CHECK(kvsd::string_to_int64("-1", &v) && v == -1);
  CHECK(kvsd::string_to_int64("9223372036854775807", &v) && v == INT64_MAX);
  CHECK(kvsd::string_to_int64("-9223372036854775808", &v) && v == INT64_MIN);

  CHECK(!kvsd::string_to_int64("", &v));
  CHECK(!kvsd::string_to_int64("-", &v));
  CHECK(!kvsd::string_to_int64("+1", &v));
  CHECK(!kvsd::string_to_int64(" 1", &v));
  CHECK(!kvsd::string_to_int64("1 ", &v));
  CHECK(!kvsd::string_to_int64("1.0", &v));
  CHECK(!kvsd::string_to_int64("0x10", &v));
  CHECK(!kvsd::string_to_int64("9223372036854775808", &v));
  CHECK(!kvsd::string_to_int64("-9223372036854775809", &v));
  CHECK(!kvsd::string_to_int64(std::string("1\0" "2", 3), &v));
}

int main() {
  test_single_command();
  test_split_one_byte_at_a_time();
  test_split_at_every_boundary();
  test_pipelined_commands_in_one_read();
  test_pipelined_with_a_trailing_partial();
  test_binary_safe_payload();
  test_empty_bulk_argument();
  test_null_and_zero_arrays_are_no_ops();
  test_parser_recovers_between_commands_after_no_op();
  test_malformed_inputs();
  test_multibulk_count_over_limit();
  test_error_resets_parser_state();
  test_declared_bulk_over_limit_is_rejected();
  test_large_bulk_arrives_in_chunks();
  test_bulk_at_exactly_the_limit_header_is_accepted();
  test_inline_basic();
  test_inline_empty_line_is_a_no_op();
  test_inline_quoting();
  test_inline_unbalanced_quotes();
  test_inline_split_across_reads();
  test_inline_over_length_limit();
  test_inline_then_multibulk_on_same_connection();
  test_string_to_int64();
  return kvsd_test::summarize("resp");
}
