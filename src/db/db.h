#pragma once

#include <cstddef>
#include <cstdint>
#include <random>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

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

  // The active half of expiration: sample up to sample_size keys that have a TTL and
  // delete the ones whose deadline has passed. Returns how many were reclaimed.
  //
  // WHY sampling instead of scanning: a scan is O(keyspace) and this runs on the same
  // thread that serves commands, so a full pass would be a latency spike proportional
  // to how much data the user stored. Sampling makes the cost per tick a constant the
  // operator chooses, and repeats while the sample keeps coming back mostly-expired,
  // so a mass expiry event is cleared in a few ticks rather than one long stall.
  size_t active_expire_cycle(size_t sample_size);

  // The raw table size.
  //
  // WHY this can transiently exceed the number of live keys: it counts entries, and an
  // expired key that nobody has looked up yet is still an entry. Redis's DBSIZE has
  // exactly this property for exactly this reason -- making it exact would mean walking
  // the whole keyspace on an O(1) command. The active expiry cycle converges it.
  size_t size() const { return map_.size(); }

  // How many keys currently carry a TTL. Used by tests and by the expiry cycle.
  size_t volatile_size() const { return volatile_keys_.size(); }

  void clear() {
    map_.clear();
    volatile_keys_.clear();
  }

  // Number of keys reclaimed by either half of the expiry machinery, for logging.
  uint64_t expired_count() const { return expired_count_; }

 private:
  // Drops key from both tables and counts it as expired.
  void reclaim(const std::string& key);

  std::unordered_map<std::string, Value> map_;

  // The keys in map_ that have a deadline.
  //
  // WHY a second table rather than sampling map_ directly: the active cycle picks keys
  // at random and asks "is this one dead?", so its yield is the fraction of sampled
  // keys that are volatile. A workload with a million persistent keys and a thousand
  // TTL'd ones -- a cache in front of a permanent working set, which is typical --
  // would sample ~0.1% usefully and effectively never collect. Redis solves this with a
  // dedicated expires dict; this is the same idea. The cost is one extra hash lookup on
  // the paths that add or drop a TTL, and a duplicated key string per volatile key.
  std::unordered_set<std::string> volatile_keys_;

  // Sampling needs randomness, not cryptographic randomness: an attacker who could
  // predict which keys get collected early gains nothing, because collection is
  // invisible (lazy expiry already hides every dead key).
  std::minstd_rand rng_;

  // Scratch space for active_expire_cycle, kept as a member so the cycle does not
  // allocate on every tick. Erasing during the bucket walk would invalidate the
  // iterators the walk is holding, so doomed keys are collected first, deleted after.
  std::vector<std::string> doomed_;

  uint64_t expired_count_ = 0;
};

}  // namespace kvsd
