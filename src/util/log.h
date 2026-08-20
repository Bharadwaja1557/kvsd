#pragma once

namespace kvsd {
namespace log {

// Levels, in increasing severity. Debug carries the per-connection chatter (accept,
// close, expiry yield) that is useful when something is wrong and pure noise the rest
// of the time, which is why it is off unless --verbose asks for it.
enum class Level { Debug, Info, Warn, Error };

void set_min_level(Level level);

// The format attribute makes the compiler check these calls the way it checks printf.
// Without it a wrong specifier is undefined behaviour that no test would catch.
void debug(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
void info(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
void warn(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
void error(const char* fmt, ...) __attribute__((format(printf, 1, 2)));

}  // namespace log
}  // namespace kvsd
