#include "db/db.h"

#include "db/clock.h"

namespace kvsd {
namespace {

// How many rounds of sampling one cycle may run before it gives the event loop back
// its thread. Redis uses the same "keep going while the sample is still mostly dead"
// rule; the cap is what keeps a pathological keyspace from turning one tick into a
// long stall.
constexpr size_t kMaxRounds = 16;

// Repeat the sample while more than this fraction came back expired. Under 25% the
// remaining dead keys are sparse enough that lazy expiry will find them for free.
constexpr size_t kContinueIfExpiredNumerator = 1;
constexpr size_t kContinueIfExpiredDenominator = 4;

}  // namespace

Db::Db() : rng_(std::random_device{}()) {}

void Db::reclaim(const std::string& key) {
  map_.erase(key);
  volatile_keys_.erase(key);
  ++expired_count_;
}

Value* Db::lookup(const std::string& key) {
  auto it = map_.find(key);
  if (it == map_.end()) return nullptr;

  if (it->second.is_expired(now_ms())) {
    reclaim(key);
    return nullptr;
  }
  return &it->second;
}

Value& Db::set_string(const std::string& key, std::string data, int64_t expire_at_ms) {
  Value& v = map_[key];
  v.type = ValueType::String;
  v.data = std::move(data);
  v.expire_at_ms = expire_at_ms;

  // SET replaces the TTL as well as the value, so the volatile index has to be updated
  // in both directions -- including the "used to have a TTL, now does not" direction.
  if (expire_at_ms == Value::kNoExpiry) {
    volatile_keys_.erase(key);
  } else {
    volatile_keys_.insert(key);
  }
  return v;
}

bool Db::erase(const std::string& key) {
  // Routed through lookup so that deleting an already-expired key reports "no such
  // key" rather than a spurious success. DEL's return value is observable.
  if (lookup(key) == nullptr) return false;
  map_.erase(key);
  volatile_keys_.erase(key);
  return true;
}

bool Db::set_expire_at(const std::string& key, int64_t expire_at_ms) {
  Value* v = lookup(key);
  if (v == nullptr) return false;
  v->expire_at_ms = expire_at_ms;
  volatile_keys_.insert(key);
  return true;
}

bool Db::persist(const std::string& key) {
  Value* v = lookup(key);
  if (v == nullptr || !v->has_expiry()) return false;
  v->expire_at_ms = Value::kNoExpiry;
  volatile_keys_.erase(key);
  return true;
}

int64_t Db::pttl(const std::string& key) {
  Value* v = lookup(key);
  if (v == nullptr) return kNoKey;
  if (!v->has_expiry()) return kNoTtl;
  const int64_t remaining = v->expire_at_ms - now_ms();
  return remaining > 0 ? remaining : 0;
}

size_t Db::active_expire_cycle(size_t sample_size) {
  size_t reclaimed = 0;
  if (sample_size == 0) return 0;

  for (size_t round = 0; round < kMaxRounds; ++round) {
    if (volatile_keys_.empty()) break;

    const int64_t now = now_ms();
    const size_t bucket_count = volatile_keys_.bucket_count();
    size_t sampled = 0;
    doomed_.clear();

    // Start at a random bucket and walk forward. WHY not "pick a random element":
    // unordered_set has no indexed access, so the only O(1) entry point into the table
    // is a bucket. Walking on from a random start keeps each cycle looking at a
    // different slice of the keyspace, which is all the uniformity this needs.
    size_t bucket = static_cast<size_t>(rng_() % bucket_count);
    for (size_t scanned = 0; scanned < bucket_count && sampled < sample_size; ++scanned) {
      for (auto it = volatile_keys_.begin(bucket);
           it != volatile_keys_.end(bucket) && sampled < sample_size; ++it) {
        ++sampled;
        auto vit = map_.find(*it);
        if (vit == map_.end() || vit->second.is_expired(now)) doomed_.push_back(*it);
      }
      bucket = (bucket + 1) % bucket_count;
    }

    for (const std::string& key : doomed_) reclaim(key);
    reclaimed += doomed_.size();

    if (sampled == 0) break;
    if (doomed_.size() * kContinueIfExpiredDenominator <=
        sampled * kContinueIfExpiredNumerator) {
      break;
    }
  }
  return reclaimed;
}

}  // namespace kvsd
