#include "db/db.h"

#include "db/clock.h"

namespace kvsd {

Db::Db() = default;

Value* Db::lookup(const std::string& key) {
  auto it = map_.find(key);
  if (it == map_.end()) return nullptr;

  if (it->second.is_expired(now_ms())) {
    map_.erase(it);
    ++expired_count_;
    return nullptr;
  }
  return &it->second;
}

Value& Db::set_string(const std::string& key, std::string data, int64_t expire_at_ms) {
  Value& v = map_[key];
  v.type = ValueType::String;
  v.data = std::move(data);
  v.expire_at_ms = expire_at_ms;
  return v;
}

bool Db::erase(const std::string& key) {
  // Routed through lookup so that deleting an already-expired key reports "no such
  // key" rather than a spurious success. DEL's return value is observable.
  if (lookup(key) == nullptr) return false;
  map_.erase(key);
  return true;
}

bool Db::set_expire_at(const std::string& key, int64_t expire_at_ms) {
  Value* v = lookup(key);
  if (v == nullptr) return false;
  v->expire_at_ms = expire_at_ms;
  return true;
}

bool Db::persist(const std::string& key) {
  Value* v = lookup(key);
  if (v == nullptr || !v->has_expiry()) return false;
  v->expire_at_ms = Value::kNoExpiry;
  return true;
}

int64_t Db::pttl(const std::string& key) {
  Value* v = lookup(key);
  if (v == nullptr) return kNoKey;
  if (!v->has_expiry()) return kNoTtl;
  const int64_t remaining = v->expire_at_ms - now_ms();
  return remaining > 0 ? remaining : 0;
}

}  // namespace kvsd
