// Commands that concern the connection itself rather than the data.
#include "cmd/commands.h"
#include "resp/writer.h"

namespace kvsd {
namespace {

void cmd_ping(CommandContext& ctx) {
  if (ctx.argv.size() == 1) {
    reply::simple(ctx.out, "PONG");
    return;
  }
  if (ctx.argv.size() == 2) {
    // PING with an argument echoes it as a bulk string, which is how a client
    // distinguishes its own probe from another reply when pipelining.
    reply::bulk(ctx.out, ctx.argv[1]);
    return;
  }
  reply::error(ctx.out, "ERR wrong number of arguments for 'ping' command");
}

void cmd_echo(CommandContext& ctx) { reply::bulk(ctx.out, ctx.argv[1]); }

void cmd_quit(CommandContext& ctx) {
  reply::ok(ctx.out);
  // The close is deferred to the connection layer so the +OK above actually reaches
  // the wire; see CommandContext::close_after_reply.
  ctx.close_after_reply = true;
}

}  // namespace

void register_connection_commands(CommandTable& t) {
  // PING takes an optional message, so its minimum is 1 and the exact check lives in
  // the handler -- the arity field cannot express "1 or 2".
  t.add("ping", cmd_ping, -1);
  t.add("echo", cmd_echo, 2);
  t.add("quit", cmd_quit, 1);
}

}  // namespace kvsd
