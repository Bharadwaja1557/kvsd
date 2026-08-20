#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <unordered_map>

#include "db/value.h"

namespace kvsd {

// The keyspace. A single unordered_map, reachable only from the event loop thread,
// which is why there is not a mutex anywhere in this file.
class Db {
 public:
  Db();

  // Returns the live value for key, or nullptr if it is absent or has expired.
  //
  // This is the lazy half of expiration: nothing scans for dead keys, but no dead key
  // is ever observable, because every read path goes through here and a key found past
  // its deadline is deleted before the caller sees it.
  Value* lookup(const std::string& key);

  // Overwrites any existing value, including its TTL. Returns the stored value so the
  // caller can keep working with it.
  Value& set_string(const std::string& key, std::string data,
                    int64_t expire_at_ms = Value::kNoExpiry);

  bool erase(const std::string& key);

  // Attaches or replaces a deadline. Returns false if the key does not exist.
  bool set_expire_at(const std::string& key, int64_t expire_at_ms);

  // Clears the deadline. Returns true only if there was one to clear.
  bool persist(const std::string& key);

  // Milliseconds until expiry, or kNoKey / kNoExpiry as Redis defines them for TTL.
  static constexpr int64_t kNoKey = -2;
  static constexpr int64_t kNoTtl = -1;
  int64_t pttl(const std::string& key);

  // The raw table size.
  //
  // WHY this can transiently exceed the number of live keys: it counts entries, and an
  // expired key that nobody has looked up yet is still an entry. Redis's DBSIZE has
  // exactly this property for exactly this reason -- making it exact would mean walking
  // the whole keyspace on an O(1) command. The active expiry cycle converges it.
  size_t size() const { return map_.size(); }

  void clear() { map_.clear(); }

  // Number of keys reclaimed by either half of the expiry machinery, for logging.
  uint64_t expired_count() const { return expired_count_; }

 private:
  std::unordered_map<std::string, Value> map_;
  uint64_t expired_count_ = 0;
};

}  // namespace kvsd
