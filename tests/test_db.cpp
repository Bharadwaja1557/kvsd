// Keyspace tests: lazy expiry, the TTL accessors, and the active expiry cycle.
#include "db/db.h"

#include <string>

#include "db/clock.h"
#include "test_util.h"

using namespace kvsd;

namespace {

void test_set_and_lookup() {
  Db db;
  CHECK(db.lookup("missing") == nullptr);

  db.set_string("k", "v1");
  Value* v = db.lookup("k");
  CHECK(v != nullptr);
  if (v) {
    CHECK_EQ(v->data, std::string("v1"));
    CHECK(v->type == ValueType::String);
    CHECK(!v->has_expiry());
  }

  db.set_string("k", "v2");
  CHECK_EQ(db.size(), size_t(1));
  CHECK_EQ(db.lookup("k")->data, std::string("v2"));
}

void test_lazy_expiry() {
  Db db;
  db.set_string("gone", "x", now_ms() - 1);
  db.set_string("live", "x", now_ms() + 60000);

  // The dead key is still an entry until something looks at it -- that is what makes
  // this lazy -- but it is never observable.
  CHECK_EQ(db.size(), size_t(2));
  CHECK(db.lookup("gone") == nullptr);
  CHECK_EQ(db.size(), size_t(1));
  CHECK_EQ(db.expired_count(), uint64_t(1));
  CHECK(db.lookup("live") != nullptr);

  // A key that expired is reported as absent by every accessor, not just lookup.
  db.set_string("gone", "x", now_ms() - 1);
  CHECK_EQ(db.pttl("gone"), Db::kNoKey);
  db.set_string("gone", "x", now_ms() - 1);
  CHECK(!db.erase("gone"));
  db.set_string("gone", "x", now_ms() - 1);
  CHECK(!db.persist("gone"));
  db.set_string("gone", "x", now_ms() - 1);
  CHECK(!db.set_expire_at("gone", now_ms() + 1000));
}

void test_ttl_accessors() {
  Db db;
  CHECK_EQ(db.pttl("nope"), Db::kNoKey);

  db.set_string("k", "v");
  CHECK_EQ(db.pttl("k"), Db::kNoTtl);

  CHECK(db.set_expire_at("k", now_ms() + 5000));
  const int64_t ttl = db.pttl("k");
  CHECK(ttl > 4000 && ttl <= 5000);

  CHECK(db.persist("k"));
  CHECK_EQ(db.pttl("k"), Db::kNoTtl);
  // Nothing left to clear the second time.
  CHECK(!db.persist("k"));

  CHECK(!db.set_expire_at("absent", now_ms() + 1000));
}

// The volatile index is a second table that has to stay in step with the main one; a
// key that leaks into it makes the expiry cycle sample garbage, and one that fails to
// land in it never gets actively collected.
void test_volatile_index_tracks_ttls() {
  Db db;
  db.set_string("a", "v", now_ms() + 10000);
  CHECK_EQ(db.volatile_size(), size_t(1));

  // SET without an expiry clears the old TTL, so the key stops being volatile.
  db.set_string("a", "v");
  CHECK_EQ(db.volatile_size(), size_t(0));

  db.set_expire_at("a", now_ms() + 10000);
  CHECK_EQ(db.volatile_size(), size_t(1));
  db.persist("a");
  CHECK_EQ(db.volatile_size(), size_t(0));

  db.set_expire_at("a", now_ms() + 10000);
  db.erase("a");
  CHECK_EQ(db.volatile_size(), size_t(0));

  db.set_string("b", "v", now_ms() - 1);
  CHECK(db.lookup("b") == nullptr);  // lazy expiry must clean up both tables
  CHECK_EQ(db.volatile_size(), size_t(0));

  db.set_string("c", "v", now_ms() + 10000);
  db.clear();
  CHECK_EQ(db.volatile_size(), size_t(0));
  CHECK_EQ(db.size(), size_t(0));
}

void test_active_expire_cycle() {
  Db db;
  for (int i = 0; i < 200; ++i) db.set_string("dead" + std::to_string(i), "v", now_ms() - 1);
  for (int i = 0; i < 200; ++i) db.set_string("live" + std::to_string(i), "v");
  db.set_string("later", "v", now_ms() + 60000);

  CHECK_EQ(db.size(), size_t(401));

  // Each call is bounded work, so it takes several ticks to drain 200 dead keys.
  size_t total = 0;
  for (int tick = 0; tick < 200 && db.volatile_size() > 1; ++tick) {
    total += db.active_expire_cycle(20);
  }
  CHECK_EQ(total, size_t(200));
  CHECK_EQ(db.size(), size_t(201));
  CHECK_EQ(db.volatile_size(), size_t(1));  // "later" is volatile but not yet due

  // Persistent keys are untouched, and so is the unexpired volatile one.
  CHECK(db.lookup("live7") != nullptr);
  CHECK(db.lookup("later") != nullptr);

  // A cycle over a keyspace with nothing due reclaims nothing.
  CHECK_EQ(db.active_expire_cycle(20), size_t(0));
  CHECK_EQ(db.active_expire_cycle(0), size_t(0));

  Db empty;
  CHECK_EQ(empty.active_expire_cycle(20), size_t(0));
}

}  // namespace

int main() {
  test_set_and_lookup();
  test_lazy_expiry();
  test_ttl_accessors();
  test_volatile_index_tracks_ttls();
  test_active_expire_cycle();
  return kvsd_test::summarize("test_db");
}
