#pragma once

#include <cstdint>
#include <string>

namespace kvsd {

// WHY a type tag when Phase 1 only ever stores strings: TYPE has to answer from stored
// state rather than from an assumption, and the commands that will reject a wrong-typed
// key later (LPUSH against a string, say) need the check to exist now, in the one place
// it belongs. Adding lists or hashes then extends this enum instead of changing the
// shape of every map entry.
enum class ValueType : uint8_t {
  String,
};

inline const char* type_name(ValueType t) {
  switch (t) {
    case ValueType::String:
      return "string";
  }
  return "none";
}

struct Value {
  ValueType type = ValueType::String;
  std::string data;

  // Absolute deadline in milliseconds since the Unix epoch. kNoExpiry means the key
  // is persistent.
  //
  // WHY absolute rather than a remaining-TTL countdown: a countdown has to be
  // decremented, which means every key with a TTL would need touching on every tick --
  // exactly the per-key work this design exists to avoid. An absolute deadline is
  // written once and every later check is a single integer comparison. It is also the
  // form that survives being written to a file, which Phase 2 needs.
  int64_t expire_at_ms = 0;

  static constexpr int64_t kNoExpiry = 0;

  bool has_expiry() const { return expire_at_ms != kNoExpiry; }
  bool is_expired(int64_t now_ms_value) const {
    return has_expiry() && expire_at_ms <= now_ms_value;
  }
};

}  // namespace kvsd
