#pragma once

#include <cstddef>
#include <string>
#include <unordered_map>
#include <vector>

#include "db/db.h"
#include "net/buffer.h"

namespace kvsd {

// Everything a command handler is allowed to touch. Passing one struct rather than
// four arguments means adding server state later (a config, statistics) does not edit
// the signature of every handler.
struct CommandContext {
  // argv[0] is the command name as the client spelled it, case and all.
  const std::vector<std::string>& argv;
  // Handlers append their reply here; nothing in src/cmd touches a socket. That is
  // what lets the command layer be tested without a network, and what lets a pipeline
  // of N commands leave the loop with one write() to make.
  Buffer& out;
  Db& db;

  // QUIT sets this. The connection flushes the reply it has already queued, then
  // closes -- a close before the flush would lose the +OK the client is waiting for.
  bool close_after_reply = false;
};

using CommandHandler = void (*)(CommandContext&);

struct Command {
  const char* name;  // lowercase; the lookup key
  CommandHandler handler;

  // Redis's convention, kept deliberately: a positive arity is the exact argument
  // count *including* the command name, a negative one is a minimum of |arity|. One
  // integer covers both fixed commands (GET, arity 2) and variadic ones (DEL, arity
  // -2), which is why the check can live in the dispatcher instead of in every handler.
  int arity;
};

// The name -> handler map. Built once, then read-only, so lookups need no locking and
// a handler pointer stays valid for the life of the process.
class CommandTable {
 public:
  static const CommandTable& instance();

  // name is matched case-insensitively, because RESP command names are.
  const Command* find(const std::string& name) const;

  size_t size() const { return by_name_.size(); }

  // Looks the command up, checks its arity, and runs it. Unknown commands and arity
  // violations are answered here with the error reply Redis sends, so that no handler
  // has to start by re-checking argv.size().
  void dispatch(CommandContext& ctx) const;

  // Only the group registration functions call this, and only during construction.
  void add(const char* name, CommandHandler handler, int arity);

 private:
  CommandTable();

  std::unordered_map<std::string, Command> by_name_;
};

// Lowercases ASCII only: command names are ASCII by definition, and a locale-aware
// tolower would make dispatch depend on the server's environment.
std::string to_lower_ascii(const std::string& s);

}  // namespace kvsd
