#include "resp/writer.h"

#include <cstdint>
#include <string>

#include "net/buffer.h"
#include "test_util.h"

using kvsd::Buffer;
namespace reply = kvsd::reply;

static void test_simple_and_error() {
  Buffer b;
  reply::ok(b);
  CHECK_EQ(b.to_string(), std::string("+OK\r\n"));

  b.consume_all();
  reply::simple(b, "PONG");
  CHECK_EQ(b.to_string(), std::string("+PONG\r\n"));

  b.consume_all();
  reply::error(b, "ERR unknown command 'nope'");
  CHECK_EQ(b.to_string(), std::string("-ERR unknown command 'nope'\r\n"));
}

static void test_integers_including_extremes() {
  struct Case { int64_t n; const char* want; };
  const Case cases[] = {
      {0, ":0\r\n"},
      {1, ":1\r\n"},
      {-1, ":-1\r\n"},
      {42, ":42\r\n"},
      {-9999, ":-9999\r\n"},
      {INT64_MAX, ":9223372036854775807\r\n"},
      // The case a naive negate would get wrong: -INT64_MIN overflows a signed type.
      {INT64_MIN, ":-9223372036854775808\r\n"},
  };
  for (const Case& c : cases) {
    Buffer b;
    reply::integer(b, c.n);
    CHECK_EQ(b.to_string(), std::string(c.want));
  }
}

static void test_bulk_is_binary_safe() {
  Buffer b;
  const std::string payload = std::string("a\0b\r\n", 5);
  reply::bulk(b, payload);
  CHECK_EQ(b.to_string(), std::string("$5\r\na\0b\r\n\r\n", 11));

  b.consume_all();
  reply::bulk(b, "");
  CHECK_EQ(b.to_string(), std::string("$0\r\n\r\n"));

  b.consume_all();
  reply::null_bulk(b);
  CHECK_EQ(b.to_string(), std::string("$-1\r\n"));
}

static void test_large_bulk_round_trip() {
  Buffer b;
  const std::string payload(100000, 'z');
  reply::bulk(b, payload);
  const std::string want = "$100000\r\n" + payload + "\r\n";
  CHECK_EQ(b.readable(), want.size());
  CHECK(b.to_string() == want);
}

static void test_array_header_and_appending() {
  Buffer b;
  reply::array_header(b, 2);
  reply::bulk(b, "a");
  reply::integer(b, 7);
  CHECK_EQ(b.to_string(), std::string("*2\r\n$1\r\na\r\n:7\r\n"));

  b.consume_all();
  reply::array_header(b, 0);
  CHECK_EQ(b.to_string(), std::string("*0\r\n"));
}

static void test_replies_accumulate_for_pipelining() {
  // A pipelined batch must leave one contiguous byte stream for a single write().
  Buffer b;
  reply::ok(b);
  reply::bulk(b, "value");
  reply::integer(b, 3);
  CHECK_EQ(b.to_string(), std::string("+OK\r\n$5\r\nvalue\r\n:3\r\n"));
}

int main() {
  test_simple_and_error();
  test_integers_including_extremes();
  test_bulk_is_binary_safe();
  test_large_bulk_round_trip();
  test_array_header_and_appending();
  test_replies_accumulate_for_pipelining();
  return kvsd_test::summarize("writer");
}
