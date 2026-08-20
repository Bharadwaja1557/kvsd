#include "net/buffer.h"

#include <cstring>
#include <string>

#include "test_util.h"

using kvsd::Buffer;

static void test_append_and_consume() {
  Buffer b;
  CHECK(b.empty());
  b.append("hello", 5);
  CHECK_EQ(b.readable(), size_t(5));
  CHECK_EQ(b.to_string(), std::string("hello"));

  b.consume(2);
  CHECK_EQ(b.to_string(), std::string("llo"));
  b.consume(3);
  CHECK(b.empty());
}

static void test_consume_more_than_readable_resets() {
  Buffer b;
  b.append("abc", 3);
  b.consume(99);
  CHECK(b.empty());
  b.append("z", 1);
  CHECK_EQ(b.to_string(), std::string("z"));
}

static void test_cursor_survives_interleaved_append() {
  // The pipelining pattern: consume one command, more data arrives, repeat.
  Buffer b;
  b.append("one|", 4);
  b.consume(4);
  b.append("two|", 4);
  CHECK_EQ(b.to_string(), std::string("two|"));
  b.consume(2);
  b.append("three", 5);
  CHECK_EQ(b.to_string(), std::string("o|three"));
}

static void test_compaction_reclaims_prefix_without_growing() {
  Buffer b;
  b.ensure_writable(64);
  const size_t cap = b.capacity();

  // Fill it, consume most of it, then ask for room that only fits if the consumed
  // prefix is reclaimed. Capacity must not change.
  std::string filler(cap, 'x');
  b.append(filler);
  b.consume(cap - 4);
  b.ensure_writable(cap - 8);
  CHECK_EQ(b.capacity(), cap);
  CHECK_EQ(b.readable(), size_t(4));
  CHECK_EQ(b.to_string(), std::string("xxxx"));
}

static void test_growth_preserves_contents() {
  Buffer b;
  std::string s;
  for (int i = 0; i < 5000; ++i) s += static_cast<char>('a' + (i % 26));
  b.append(s);
  CHECK_EQ(b.readable(), s.size());
  CHECK_EQ(b.to_string(), s);
}

static void test_write_ptr_roundtrip() {
  // This is the path read(2) uses: reserve, write into the raw region, commit.
  Buffer b;
  b.ensure_writable(16);
  CHECK(b.writable() >= 16);
  std::memcpy(b.write_ptr(), "0123456789", 10);
  b.commit(10);
  CHECK_EQ(b.to_string(), std::string("0123456789"));
}

static void test_binary_safe() {
  Buffer b;
  const char raw[] = {'a', '\0', '\r', '\n', 'b'};
  b.append(raw, sizeof(raw));
  CHECK_EQ(b.readable(), size_t(5));
  CHECK_EQ(b.to_string(), std::string(raw, sizeof(raw)));
}

int main() {
  test_append_and_consume();
  test_consume_more_than_readable_resets();
  test_cursor_survives_interleaved_append();
  test_compaction_reclaims_prefix_without_growing();
  test_growth_preserves_contents();
  test_write_ptr_roundtrip();
  test_binary_safe();
  return kvsd_test::summarize("buffer");
}
