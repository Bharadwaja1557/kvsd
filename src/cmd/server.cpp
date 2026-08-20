// Server-wide commands: keyspace statistics, bulk deletion, and the introspection
// handshake redis-cli performs before its first prompt.
#include <string>

#include "cmd/commands.h"
#include "resp/writer.h"

namespace kvsd {
namespace {

void cmd_dbsize(CommandContext& ctx) {
  reply::integer(ctx.out, static_cast<int64_t>(ctx.db.size()));
}

void cmd_flushall(CommandContext& ctx) {
  // ASYNC and SYNC are accepted and ignored: they select between freeing memory on
  // this thread and on a background one, and there is no background one. Accepting
  // them means a script written against Redis does not have to be edited to run here;
  // rejecting them would be honest about the internals and useless to the caller.
  if (ctx.argv.size() > 2) {
    reply::error(ctx.out, err::kSyntax);
    return;
  }
  if (ctx.argv.size() == 2) {
    const std::string opt = to_lower_ascii(ctx.argv[1]);
    if (opt != "async" && opt != "sync") {
      reply::error(ctx.out, err::kSyntax);
      return;
    }
  }
  ctx.db.clear();
  reply::ok(ctx.out);
}

void cmd_command(CommandContext& ctx) {
  // A deliberate stub. redis-cli issues COMMAND DOCS on connect purely to populate
  // its own tab-completion and argument hints; it treats an empty reply as "this
  // server offers no hints" and connects normally. Emitting the real reply would mean
  // maintaining a full metadata table -- flags, key positions, ACL categories -- that
  // nothing in kvsd consumes.
  if (ctx.argv.size() >= 2 && to_lower_ascii(ctx.argv[1]) == "count") {
    reply::integer(ctx.out, static_cast<int64_t>(CommandTable::instance().size()));
    return;
  }
  reply::array_header(ctx.out, 0);
}

}  // namespace

void register_server_commands(CommandTable& t) {
  t.add("dbsize", cmd_dbsize, 1);
  t.add("flushall", cmd_flushall, -1);
  t.add("command", cmd_command, -1);
}

}  // namespace kvsd
