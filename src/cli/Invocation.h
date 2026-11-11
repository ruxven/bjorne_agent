#pragma once

#include <string>
#include <vector>

#include "log/Log.h"

namespace bjorne_agent {
namespace cli {

// The subcommand the command line selected. `run` is the default, so the bare
// form keeps working. The other verbs are reserved by the roadmap: they parse
// as commands so that a task beginning with one has to be passed to `run`, but
// they are not implemented yet.
enum class Command { Run, Init, Sessions, Resume };

// What the command line asked for. The config path is empty for commands that
// do not run a task, and when the default search found nothing.
struct Invocation {
  Command command = Command::Run;
  std::string configPath;
  std::string task;
  bool taskFromArgument = false;
  // The session id named by `resume`. Empty when the command takes none.
  std::string session;
  // True when a verb was typed, so the caller can tell `bjorne_agent` from
  // `bjorne_agent run` and show the top level help for the first.
  bool commandSet = false;
  bool help = false;
  bool levelSet = false;
  log::Level level = log::Level::Normal;
};

// Parses the whole command line: global flags, an optional subcommand, and the
// positional arguments.
//
// The first positional argument is a subcommand when it exactly matches a
// reserved verb. Otherwise the bare form runs `run`. A task that begins with a
// reserved word is spelled `bjorne_agent run "init the project"`.
//
// Flags may appear before or after the subcommand. `--` ends flag parsing, so
// a task that begins with a dash can still be passed.
//
// Returns false on an unknown option, too many arguments, or a missing config
// file, with the reason in error.
bool parse(const std::vector<std::string>& arguments, Invocation& out, std::string& error);

// The name of a command as the user types it.
const char* commandName(Command command);

// The top level help text. Ends with a newline.
std::string usage();

// The help text for one subcommand. Ends with a newline.
std::string usage(Command command);

}  // namespace cli
}  // namespace bjorne_agent
