// Commands that operate on keys regardless of the type they hold: existence, type,
// and the TTL surface.
#include <string>

#include "cmd/commands.h"
#include "db/clock.h"
#include "resp/parser.h"
#include "resp/writer.h"

namespace kvsd {
namespace {

void cmd_del(CommandContext& ctx) {
  int64_t deleted = 0;
  for (size_t i = 1; i < ctx.argv.size(); ++i) {
    if (ctx.db.erase(ctx.argv[i])) ++deleted;
  }
  reply::integer(ctx.out, deleted);
}

void cmd_exists(CommandContext& ctx) {
  // Counts occurrences, not distinct keys: `EXISTS k k` answers 2 if k exists. That
  // looks odd in isolation but is what Redis does, and scripts rely on it.
  int64_t found = 0;
  for (size_t i = 1; i < ctx.argv.size(); ++i) {
    if (ctx.db.lookup(ctx.argv[i]) != nullptr) ++found;
  }
  reply::integer(ctx.out, found);
}

void cmd_type(CommandContext& ctx) {
  Value* v = ctx.db.lookup(ctx.argv[1]);
  // A missing key is "none" rather than an error, so a client can branch on the type
  // without a separate EXISTS round trip.
  reply::simple(ctx.out, v == nullptr ? "none" : type_name(v->type));
}

void cmd_expire(CommandContext& ctx) {
  int64_t seconds = 0;
  if (!string_to_int64(ctx.argv[2], &seconds)) {
    reply::error(ctx.out, err::kNotInteger);
    return;
  }

  const std::string& key = ctx.argv[1];
  if (ctx.db.lookup(key) == nullptr) {
    reply::integer(ctx.out, 0);
    return;
  }

  // A deadline in the past means the key is already dead, so it is deleted now rather
  // than stored and collected later. The reply is still 1: the client asked for the
  // key to stop existing at a time that has passed, and it did.
  if (seconds <= 0) {
    ctx.db.erase(key);
    reply::integer(ctx.out, 1);
    return;
  }

  int64_t deadline = 0;
  if (!ttl_to_deadline(seconds, 1000, now_ms(), &deadline)) {
    reply::error(ctx.out, "ERR invalid expire time in 'expire' command");
    return;
  }
  ctx.db.set_expire_at(key, deadline);
  reply::integer(ctx.out, 1);
}

void cmd_ttl(CommandContext& ctx) {
  const int64_t ms = ctx.db.pttl(ctx.argv[1]);
  if (ms < 0) {
    reply::integer(ctx.out, ms);  // -2 no such key, -1 no TTL
    return;
  }
  // Rounded to nearest rather than truncated, so `SET k v EX 100; TTL k` answers 100
  // and not 99 when a millisecond has already elapsed. Redis rounds the same way.
  reply::integer(ctx.out, (ms + 500) / 1000);
}

void cmd_persist(CommandContext& ctx) {
  reply::integer(ctx.out, ctx.db.persist(ctx.argv[1]) ? 1 : 0);
}

}  // namespace

void register_keyspace_commands(CommandTable& t) {
  t.add("del", cmd_del, -2);
  t.add("exists", cmd_exists, -2);
  t.add("type", cmd_type, 2);
  t.add("expire", cmd_expire, 3);
  t.add("ttl", cmd_ttl, 2);
  t.add("persist", cmd_persist, 2);
}

}  // namespace kvsd
