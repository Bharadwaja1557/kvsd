#pragma once

// A ~40-line test harness. The project takes no third-party runtime dependencies,
// and that rule is worth more than the features a real framework would add here.

#include <cstdio>
#include <string>

namespace kvsd_test {

inline int& failures() {
  static int n = 0;
  return n;
}

inline void report(const char* file, int line, const char* expr, const std::string& detail) {
  std::fprintf(stderr, "FAIL %s:%d: %s\n", file, line, expr);
  if (!detail.empty()) std::fprintf(stderr, "     %s\n", detail.c_str());
  ++failures();
}

template <typename T>
std::string show(const T& v) {
  return std::to_string(v);
}
inline std::string show(const std::string& v) {
  std::string out = "\"";
  for (char c : v) {
    if (c == '\r') out += "\\r";
    else if (c == '\n') out += "\\n";
    else if (c >= 32 && c < 127) out += c;
    else { char buf[8]; std::snprintf(buf, sizeof(buf), "\\x%02x", (unsigned char)c); out += buf; }
  }
  return out + "\"";
}
inline std::string show(const char* v) { return show(std::string(v)); }
inline std::string show(bool v) { return v ? "true" : "false"; }

inline int summarize(const char* suite) {
  if (failures() == 0) {
    std::printf("ok  %s\n", suite);
    return 0;
  }
  std::printf("FAILED %s (%d assertion(s))\n", suite, failures());
  return 1;
}

}  // namespace kvsd_test

#define CHECK(expr)                                                      \
  do {                                                                   \
    if (!(expr)) kvsd_test::report(__FILE__, __LINE__, #expr, "");       \
  } while (0)

#define CHECK_EQ(a, b)                                                              \
  do {                                                                              \
    const auto& a_ = (a);                                                           \
    const auto& b_ = (b);                                                           \
    if (!(a_ == b_)) {                                                              \
      kvsd_test::report(__FILE__, __LINE__, #a " == " #b,                           \
                        "got " + kvsd_test::show(a_) + ", want " + kvsd_test::show(b_)); \
    }                                                                               \
  } while (0)
