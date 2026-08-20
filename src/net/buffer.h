#pragma once

#include <cstddef>
#include <string>
#include <vector>

namespace kvsd {

// A growable byte buffer with a read cursor.
//
// WHY a cursor instead of erasing consumed bytes from the front: a pipelined client
// sends many commands per read(), and erase-from-front would memmove the entire
// remaining buffer once per command -- quadratic in the number of pipelined commands.
// Consuming is instead a pointer bump, and the wasted prefix is reclaimed only when
// it is worth the single memmove (see ensure_writable).
class Buffer {
 public:
  Buffer() = default;

  const char* peek() const { return data_.data() + read_pos_; }
  size_t readable() const { return write_pos_ - read_pos_; }
  bool empty() const { return readable() == 0; }

  // Bytes available to write into without growing.
  size_t writable() const { return data_.size() - write_pos_; }
  char* write_ptr() { return data_.data() + write_pos_; }

  // Guarantees writable() >= n, first by reclaiming the consumed prefix and only
  // then by growing. read()/write() call this before handing the region to the kernel.
  void ensure_writable(size_t n);

  // Records that n bytes were written directly into write_ptr() (by read(2), say).
  void commit(size_t n) { write_pos_ += n; }

  void append(const char* p, size_t n);
  void append(const std::string& s) { append(s.data(), s.size()); }
  void append(char c) { append(&c, 1); }

  void consume(size_t n);
  void consume_all() { read_pos_ = write_pos_ = 0; }

  // Capacity of the underlying storage. Used by the output-buffer limit check and
  // by tests that assert the compaction path actually runs.
  size_t capacity() const { return data_.size(); }

  std::string to_string() const { return std::string(peek(), readable()); }

 private:
  void compact();

  std::vector<char> data_;
  size_t read_pos_ = 0;
  size_t write_pos_ = 0;
};

}  // namespace kvsd
