#include "net/buffer.h"

#include <algorithm>
#include <cstring>

namespace kvsd {
namespace {
// Below this size, growth doubles; above it, growth is linear in 16 MiB steps.
// WHY: doubling a 512 MiB bulk string would ask the allocator for 1 GiB to hold
// 512 MiB + 1 byte.
constexpr size_t kDoublingLimit = 16u * 1024 * 1024;
constexpr size_t kInitialSize = 1024;
}  // namespace

void Buffer::compact() {
  const size_t n = readable();
  if (n > 0 && read_pos_ > 0) {
    std::memmove(data_.data(), data_.data() + read_pos_, n);
  }
  read_pos_ = 0;
  write_pos_ = n;
}

void Buffer::ensure_writable(size_t n) {
  if (writable() >= n) return;

  // Reclaiming the consumed prefix is one memmove of the live bytes; growing is an
  // allocation plus a copy of the same bytes. Prefer the former when it suffices.
  if (read_pos_ + writable() >= n) {
    compact();
    return;
  }

  compact();
  size_t want = write_pos_ + n;
  size_t next = std::max(data_.size(), kInitialSize);
  while (next < want) {
    next += (next < kDoublingLimit) ? next : kDoublingLimit;
  }
  data_.resize(next);
}

void Buffer::append(const char* p, size_t n) {
  if (n == 0) return;
  ensure_writable(n);
  std::memcpy(write_ptr(), p, n);
  commit(n);
}

void Buffer::consume(size_t n) {
  if (n >= readable()) {
    consume_all();
    return;
  }
  read_pos_ += n;
}

}  // namespace kvsd
