// Command-layer tests. They drive CommandTable::dispatch directly and assert on the
// exact RESP bytes, because the wire format is the contract redis-cli holds us to.
#include <string>
#include <vector>

#include "cmd/commands.h"
#include "db/clock.h"
#include "resp/writer.h"
#include "test_util.h"

using namespace kvsd;

namespace {

std::string run(Db& db, const std::vector<std::string>& argv, bool* close_after = nullptr) {
  Buffer out;
  CommandContext ctx{argv, out, db};
  CommandTable::instance().dispatch(ctx);
  if (close_after != nullptr) *close_after = ctx.close_after_reply;
  return out.to_string();
}

void test_dispatch_errors() {
  Db db;
  const std::string unknown = run(db, {"nosuchcmd", "a"});
  CHECK(unknown.compare(0, 21, "-ERR unknown command ") == 0);

  CHECK_EQ(run(db, {"get"}), std::string("-ERR wrong number of arguments for 'get' command\r\n"));
  CHECK_EQ(run(db, {"get", "a", "b"}),
           std::string("-ERR wrong number of arguments for 'get' command\r\n"));
  CHECK_EQ(run(db, {"del"}), std::string("-ERR wrong number of arguments for 'del' command\r\n"));

  // Command names are case-insensitive on the wire.
  CHECK_EQ(run(db, {"PiNg"}), std::string("+PONG\r\n"));
}

void test_connection_commands() {
  Db db;
  CHECK_EQ(run(db, {"ping"}), std::string("+PONG\r\n"));
  CHECK_EQ(run(db, {"ping", "hi"}), std::string("$2\r\nhi\r\n"));
  CHECK_EQ(run(db, {"ping", "a", "b"}),
           std::string("-ERR wrong number of arguments for 'ping' command\r\n"));
  CHECK_EQ(run(db, {"echo", "hello"}), std::string("$5\r\nhello\r\n"));

  bool close_after = false;
  CHECK_EQ(run(db, {"quit"}, &close_after), std::string("+OK\r\n"));
  CHECK(close_after);

  close_after = true;
  run(db, {"ping"}, &close_after);
  CHECK(!close_after);
}

void test_set_and_get() {
  Db db;
  CHECK_EQ(run(db, {"get", "k"}), std::string("$-1\r\n"));
  CHECK_EQ(run(db, {"set", "k", "v"}), std::string("+OK\r\n"));
  CHECK_EQ(run(db, {"get", "k"}), std::string("$1\r\nv\r\n"));

  // Binary-safe values, including embedded NUL and CRLF.
  const std::string binary("a\0b\r\nc", 6);
  run(db, {"set", "bin", binary});
  CHECK_EQ(run(db, {"get", "bin"}), "$6\r\n" + binary + "\r\n");

  // NX refuses when present, XX refuses when absent; both answer with a null bulk.
  CHECK_EQ(run(db, {"set", "k", "other", "NX"}), std::string("$-1\r\n"));
  CHECK_EQ(run(db, {"get", "k"}), std::string("$1\r\nv\r\n"));
  CHECK_EQ(run(db, {"set", "fresh", "v", "XX"}), std::string("$-1\r\n"));
  CHECK_EQ(run(db, {"exists", "fresh"}), std::string(":0\r\n"));
  CHECK_EQ(run(db, {"set", "fresh", "v", "nx"}), std::string("+OK\r\n"));
  CHECK_EQ(run(db, {"set", "fresh", "v2", "xx"}), std::string("+OK\r\n"));

  CHECK_EQ(run(db, {"set", "k", "v", "NX", "XX"}), std::string("-ERR syntax error\r\n"));
  CHECK_EQ(run(db, {"set", "k", "v", "BOGUS"}), std::string("-ERR syntax error\r\n"));
  CHECK_EQ(run(db, {"set", "k", "v", "EX"}), std::string("-ERR syntax error\r\n"));
  CHECK_EQ(run(db, {"set", "k", "v", "EX", "abc"}), std::string("-") + err::kNotInteger + "\r\n");
  CHECK_EQ(run(db, {"set", "k", "v", "EX", "0"}),
           std::string("-ERR invalid expire time in 'set' command\r\n"));
  CHECK_EQ(run(db, {"set", "k", "v", "EX", "9223372036854775807"}),
           std::string("-ERR invalid expire time in 'set' command\r\n"));
}

void test_set_expiry_options() {
  Db db;
  CHECK_EQ(run(db, {"set", "k", "v", "EX", "100"}), std::string("+OK\r\n"));
  CHECK_EQ(run(db, {"ttl", "k"}), std::string(":100\r\n"));

  CHECK_EQ(run(db, {"set", "k", "v", "PX", "50000"}), std::string("+OK\r\n"));
  CHECK_EQ(run(db, {"ttl", "k"}), std::string(":50\r\n"));

  // A plain SET drops the TTL.
  run(db, {"set", "k", "v"});
  CHECK_EQ(run(db, {"ttl", "k"}), std::string(":-1\r\n"));

  // Options in either order, and NX combined with a TTL.
  CHECK_EQ(run(db, {"set", "n", "v", "NX", "EX", "30"}), std::string("+OK\r\n"));
  CHECK_EQ(run(db, {"ttl", "n"}), std::string(":30\r\n"));

  // A key whose deadline has passed is gone even though nothing swept it.
  run(db, {"set", "quick", "v", "PX", "1"});
  db.set_expire_at("quick", now_ms() - 1);
  CHECK_EQ(run(db, {"get", "quick"}), std::string("$-1\r\n"));
}

void test_incr_decr() {
  Db db;
  CHECK_EQ(run(db, {"incr", "c"}), std::string(":1\r\n"));
  CHECK_EQ(run(db, {"incr", "c"}), std::string(":2\r\n"));
  CHECK_EQ(run(db, {"decr", "c"}), std::string(":1\r\n"));
  CHECK_EQ(run(db, {"decr", "fresh"}), std::string(":-1\r\n"));
  CHECK_EQ(run(db, {"get", "c"}), std::string("$1\r\n1\r\n"));

  run(db, {"set", "word", "abc"});
  CHECK_EQ(run(db, {"incr", "word"}), std::string("-") + err::kNotInteger + "\r\n");
  // Leading whitespace and a trailing sign are not integers either.
  run(db, {"set", "word", " 1"});
  CHECK_EQ(run(db, {"incr", "word"}), std::string("-") + err::kNotInteger + "\r\n");

  run(db, {"set", "big", "9223372036854775807"});
  CHECK_EQ(run(db, {"incr", "big"}),
           std::string("-ERR increment or decrement would overflow\r\n"));
  run(db, {"set", "small", "-9223372036854775808"});
  CHECK_EQ(run(db, {"decr", "small"}),
           std::string("-ERR increment or decrement would overflow\r\n"));

  // Incrementing must not disturb the key's TTL.
  run(db, {"set", "ctr", "1", "EX", "100"});
  run(db, {"incr", "ctr"});
  CHECK_EQ(run(db, {"ttl", "ctr"}), std::string(":100\r\n"));
}

void test_keyspace_commands() {
  Db db;
  run(db, {"set", "a", "1"});
  run(db, {"set", "b", "2"});

  CHECK_EQ(run(db, {"exists", "a"}), std::string(":1\r\n"));
  CHECK_EQ(run(db, {"exists", "a", "b", "missing"}), std::string(":2\r\n"));
  CHECK_EQ(run(db, {"exists", "a", "a"}), std::string(":2\r\n"));

  CHECK_EQ(run(db, {"type", "a"}), std::string("+string\r\n"));
  CHECK_EQ(run(db, {"type", "missing"}), std::string("+none\r\n"));

  CHECK_EQ(run(db, {"del", "a", "missing", "b"}), std::string(":2\r\n"));
  CHECK_EQ(run(db, {"del", "a"}), std::string(":0\r\n"));
}

void test_expire_ttl_persist() {
  Db db;
  CHECK_EQ(run(db, {"expire", "missing", "10"}), std::string(":0\r\n"));
  CHECK_EQ(run(db, {"ttl", "missing"}), std::string(":-2\r\n"));

  run(db, {"set", "k", "v"});
  CHECK_EQ(run(db, {"ttl", "k"}), std::string(":-1\r\n"));
  CHECK_EQ(run(db, {"expire", "k", "60"}), std::string(":1\r\n"));
  CHECK_EQ(run(db, {"ttl", "k"}), std::string(":60\r\n"));
  CHECK_EQ(run(db, {"persist", "k"}), std::string(":1\r\n"));
  CHECK_EQ(run(db, {"persist", "k"}), std::string(":0\r\n"));
  CHECK_EQ(run(db, {"persist", "missing"}), std::string(":0\r\n"));

  CHECK_EQ(run(db, {"expire", "k", "notanumber"}), std::string("-") + err::kNotInteger + "\r\n");
  CHECK_EQ(run(db, {"expire", "k", "9223372036854775807"}),
           std::string("-ERR invalid expire time in 'expire' command\r\n"));

  // A non-positive TTL deletes the key immediately and still reports success.
  CHECK_EQ(run(db, {"expire", "k", "-1"}), std::string(":1\r\n"));
  CHECK_EQ(run(db, {"exists", "k"}), std::string(":0\r\n"));
}

void test_server_commands() {
  Db db;
  CHECK_EQ(run(db, {"dbsize"}), std::string(":0\r\n"));
  run(db, {"set", "a", "1"});
  run(db, {"set", "b", "2"});
  CHECK_EQ(run(db, {"dbsize"}), std::string(":2\r\n"));

  CHECK_EQ(run(db, {"flushall"}), std::string("+OK\r\n"));
  CHECK_EQ(run(db, {"dbsize"}), std::string(":0\r\n"));
  CHECK_EQ(run(db, {"flushall", "ASYNC"}), std::string("+OK\r\n"));
  CHECK_EQ(run(db, {"flushall", "LATER"}), std::string("-ERR syntax error\r\n"));

  CHECK_EQ(run(db, {"command"}), std::string("*0\r\n"));
  CHECK_EQ(run(db, {"command", "docs"}), std::string("*0\r\n"));
  CHECK_EQ(run(db, {"command", "count"}),
           ":" + std::to_string(CommandTable::instance().size()) + "\r\n");
}

}  // namespace

int main() {
  test_dispatch_errors();
  test_connection_commands();
  test_set_and_get();
  test_set_expiry_options();
  test_incr_decr();
  test_keyspace_commands();
  test_expire_ttl_persist();
  test_server_commands();
  return kvsd_test::summarize("test_cmd");
}
