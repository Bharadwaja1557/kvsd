#include "util/log.h"

#include <sys/time.h>

#include <cstdarg>
#include <cstdio>
#include <ctime>

namespace kvsd {
namespace log {
namespace {

Level g_min_level = Level::Info;

const char* level_tag(Level level) {
  switch (level) {
    case Level::Debug: return "DEBUG";
    case Level::Info:  return "INFO ";
    case Level::Warn:  return "WARN ";
    case Level::Error: return "ERROR";
  }
  return "?????";
}

void vwrite(Level level, const char* fmt, va_list ap) {
  if (level < g_min_level) return;

  struct timeval tv;
  ::gettimeofday(&tv, nullptr);
  struct tm tm_buf;
  ::localtime_r(&tv.tv_sec, &tm_buf);
  char stamp[32];
  const size_t n = std::strftime(stamp, sizeof(stamp), "%Y-%m-%d %H:%M:%S", &tm_buf);
  std::snprintf(stamp + n, sizeof(stamp) - n, ".%03d", static_cast<int>(tv.tv_usec / 1000));

  // Everything goes to stderr, unbuffered by default, so a log line that was produced
  // before a crash is a log line the operator actually sees. Logs are diagnostics, not
  // program output; stdout stays free for anything a future tool wants to pipe.
  std::fprintf(stderr, "%s %s ", stamp, level_tag(level));
  std::vfprintf(stderr, fmt, ap);
  std::fputc('\n', stderr);
}

}  // namespace

void set_min_level(Level level) { g_min_level = level; }

void debug(const char* fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  vwrite(Level::Debug, fmt, ap);
  va_end(ap);
}

void info(const char* fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  vwrite(Level::Info, fmt, ap);
  va_end(ap);
}

void warn(const char* fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  vwrite(Level::Warn, fmt, ap);
  va_end(ap);
}

void error(const char* fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  vwrite(Level::Error, fmt, ap);
  va_end(ap);
}

}  // namespace log
}  // namespace kvsd
