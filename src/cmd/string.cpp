// The string commands. Phase 1 stores nothing but strings, so this is where the
// type check that protects future container types gets exercised.
#include <string>

#include "cmd/commands.h"
#include "db/clock.h"
#include "resp/parser.h"
#include "resp/writer.h"

namespace kvsd {
namespace {

void cmd_set(CommandContext& ctx) {
  const std::string& key = ctx.argv[1];

  bool nx = false;
  bool xx = false;
  bool has_ttl = false;
  int64_t unit_ms = 0;
  int64_t amount = 0;

  // Options are order-independent and each may appear once, matching redis-cli's
  // grammar. Anything unrecognised, repeated, or contradictory is a syntax error --
  // silently ignoring an option the client believed it set is worse than refusing.
  for (size_t i = 3; i < ctx.argv.size(); ++i) {
    const std::string opt = to_lower_ascii(ctx.argv[i]);
    if (opt == "nx" && !xx && !nx) {
      nx = true;
    } else if (opt == "xx" && !nx && !xx) {
      xx = true;
    } else if ((opt == "ex" || opt == "px") && !has_ttl && i + 1 < ctx.argv.size()) {
      if (!string_to_int64(ctx.argv[i + 1], &amount)) {
        reply::error(ctx.out, err::kNotInteger);
        return;
      }
      if (amount <= 0) {
        reply::error(ctx.out, "ERR invalid expire time in 'set' command");
        return;
      }
      unit_ms = (opt == "ex") ? 1000 : 1;
      has_ttl = true;
      ++i;
    } else {
      reply::error(ctx.out, err::kSyntax);
      return;
    }
  }

  const bool exists = ctx.db.lookup(key) != nullptr;
  if ((nx && exists) || (xx && !exists)) {
    // A refused conditional SET is not an error: the client asked for a condition and
    // is told it did not hold. RESP2 spells that as a null bulk string.
    reply::null_bulk(ctx.out);
    return;
  }

  int64_t deadline = Value::kNoExpiry;
  if (has_ttl && !ttl_to_deadline(amount, unit_ms, now_ms(), &deadline)) {
    reply::error(ctx.out, "ERR invalid expire time in 'set' command");
    return;
  }

  // An unconditional SET replaces the TTL too. That is Redis's documented behaviour
  // and the reason KEEPTTL had to be invented later.
  ctx.db.set_string(key, ctx.argv[2], deadline);
  reply::ok(ctx.out);
}

void cmd_get(CommandContext& ctx) {
  Value* v = ctx.db.lookup(ctx.argv[1]);
  if (v == nullptr) {
    reply::null_bulk(ctx.out);
    return;
  }
  if (v->type != ValueType::String) {
    reply::error(ctx.out, err::kWrongType);
    return;
  }
  reply::bulk(ctx.out, v->data);
}

// INCR and DECR are the same operation with opposite signs, including their error
// cases, so they share one implementation rather than two that can drift apart.
void incr_by(CommandContext& ctx, int64_t delta) {
  const std::string& key = ctx.argv[1];
  Value* v = ctx.db.lookup(key);

  if (v == nullptr) {
    ctx.db.set_string(key, std::to_string(delta));
    reply::integer(ctx.out, delta);
    return;
  }
  if (v->type != ValueType::String) {
    reply::error(ctx.out, err::kWrongType);
    return;
  }

  int64_t cur = 0;
  if (!string_to_int64(v->data, &cur)) {
    reply::error(ctx.out, err::kNotInteger);
    return;
  }
  if (delta > 0 ? cur > INT64_MAX - delta : cur < INT64_MIN - delta) {
    reply::error(ctx.out, "ERR increment or decrement would overflow");
    return;
  }

  // Written in place rather than through set_string, because incrementing a counter
  // must not clear the TTL that made it a counter with a lifetime.
  cur += delta;
  v->data = std::to_string(cur);
  reply::integer(ctx.out, cur);
}

void cmd_incr(CommandContext& ctx) { incr_by(ctx, 1); }
void cmd_decr(CommandContext& ctx) { incr_by(ctx, -1); }

}  // namespace

void register_string_commands(CommandTable& t) {
  t.add("set", cmd_set, -3);
  t.add("get", cmd_get, 2);
  t.add("incr", cmd_incr, 2);
  t.add("decr", cmd_decr, 2);
}

}  // namespace kvsd
