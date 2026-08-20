#pragma once

#include <chrono>
#include <cstdint>

namespace kvsd {

// Milliseconds since the Unix epoch, from the system (wall) clock.
//
// WHY wall time rather than a monotonic clock, given that a monotonic clock is the
// textbook answer for measuring durations: an expiry deadline is not a duration. It is
// a point in time that has to mean the same thing to two parties that never compare
// clocks -- the client that said "EXPIRE k 60" and, in Phase 2, the snapshot file that
// outlives the process. A monotonic clock's zero point is the boot (or the process
// start), so a deadline recorded against it is meaningless after a restart and cannot
// be written to disk at all. Redis makes the same trade for the same reason.
//
// What this costs, stated plainly: if the system clock jumps backward, keys expire
// later than they should by the size of the jump; if it jumps forward, they expire
// early. Neither corrupts the keyspace -- an expired key is simply a key that is gone.
// docs/DESIGN.md, "Why expiry uses wall time", has the full argument.
inline int64_t now_ms() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

}  // namespace kvsd
