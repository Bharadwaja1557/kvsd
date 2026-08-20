#pragma once

#include <cstdint>

#include "cmd/table.h"

namespace kvsd {

// One registration function per command group; the table's constructor calls them in
// turn. Grouping this way keeps each source file to a single subject and means a new
// group is a new file plus one line, rather than an edit to a growing central list.
void register_connection_commands(CommandTable& t);
void register_string_commands(CommandTable& t);
void register_keyspace_commands(CommandTable& t);
void register_server_commands(CommandTable& t);

// Error strings that more than one group needs, spelled exactly as Redis spells them
// so that client libraries which match on the text keep working.
namespace err {
constexpr const char* kNotInteger = "ERR value is not an integer or out of range";
constexpr const char* kSyntax = "ERR syntax error";
constexpr const char* kWrongType =
    "WRONGTYPE Operation against a key holding the wrong kind of value";
}  // namespace err

// Converts a client-supplied TTL into the absolute deadline the keyspace stores.
// Returns false if the arithmetic would overflow, which is a case a client can reach
// with a single command (`EXPIRE k 9223372036854775807`) and which must not be allowed
// to wrap a deadline into the past.
inline bool ttl_to_deadline(int64_t amount, int64_t unit_ms, int64_t now, int64_t* out) {
  if (amount > INT64_MAX / unit_ms || amount < INT64_MIN / unit_ms) return false;
  const int64_t delta = amount * unit_ms;
  if (delta > 0 && now > INT64_MAX - delta) return false;
  if (delta < 0 && now < INT64_MIN - delta) return false;
  *out = now + delta;
  return true;
}

}  // namespace kvsd
