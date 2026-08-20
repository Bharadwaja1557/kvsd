#include "cmd/table.h"

#include "cmd/commands.h"
#include "resp/writer.h"

namespace kvsd {

std::string to_lower_ascii(const std::string& s) {
  std::string out(s);
  for (char& c : out) {
    if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
  }
  return out;
}

CommandTable::CommandTable() {
  register_connection_commands(*this);
  register_string_commands(*this);
  register_keyspace_commands(*this);
  register_server_commands(*this);
}

const CommandTable& CommandTable::instance() {
  // Function-local static: built on first use, after which it is immutable. There is
  // one thread, so the initialization guard is never contended.
  static const CommandTable table;
  return table;
}

void CommandTable::add(const char* name, CommandHandler handler, int arity) {
  by_name_[name] = Command{name, handler, arity};
}

const Command* CommandTable::find(const std::string& name) const {
  auto it = by_name_.find(to_lower_ascii(name));
  return it == by_name_.end() ? nullptr : &it->second;
}

void CommandTable::dispatch(CommandContext& ctx) const {
  const Command* cmd = find(ctx.argv[0]);
  if (cmd == nullptr) {
    // Redis echoes the arguments back so an operator reading a client's logs can see
    // which call misfired. The arguments are truncated because they are attacker
    // controlled and this string ends up in logs.
    std::string msg = "ERR unknown command '" + ctx.argv[0] + "', with args beginning with: ";
    for (size_t i = 1; i < ctx.argv.size() && i <= 3; ++i) {
      const std::string& a = ctx.argv[i];
      msg += "'" + a.substr(0, 32) + "', ";
    }
    reply::error(ctx.out, msg);
    return;
  }

  const int argc = static_cast<int>(ctx.argv.size());
  const bool ok = cmd->arity >= 0 ? argc == cmd->arity : argc >= -cmd->arity;
  if (!ok) {
    reply::error(ctx.out,
                 "ERR wrong number of arguments for '" + std::string(cmd->name) + "' command");
    return;
  }

  cmd->handler(ctx);
}

}  // namespace kvsd
