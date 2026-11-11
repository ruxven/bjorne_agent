#include "cli/Invocation.h"

#include "config/ConfigPaths.h"

#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

namespace bjorne_agent {
namespace cli {

namespace {

bool namesAnExistingFile(const std::string& path) {
  std::error_code code;
  return std::filesystem::exists(path, code) && !code;
}

// The reserved verbs. A first positional that matches one exactly is a
// command, so a task that begins with a reserved word is spelled
// `bjorne_agent run "init the project"`.
bool reservedVerb(const std::string& value, Command& out) {
  if (value == "run") {
    out = Command::Run;
    return true;
  }
  if (value == "init") {
    out = Command::Init;
    return true;
  }
  if (value == "sessions") {
    out = Command::Sessions;
    return true;
  }
  if (value == "resume") {
    out = Command::Resume;
    return true;
  }
  return false;
}

// Resolves the positional arguments of `run` into a config path and a task.
//
// Two positional arguments are the config path and the task. One positional
// argument that names an existing file is the config path, so the task then
// comes from stdin. One positional argument that names no file is the task, so
// the config comes from the default search. With no positional arguments the
// config comes from the default search and the task comes from stdin.
bool resolveRun(const std::vector<std::string>& positional, Invocation& out, std::string& error) {
  if (positional.size() > 2) {
    error = "The 'run' command takes at most a config path and a task";
    return false;
  }

  if (positional.size() == 2) {
    out.configPath = positional[0];
    out.task = positional[1];
    out.taskFromArgument = true;
    return true;
  }

  if (positional.size() == 1) {
    const std::string& argument = positional[0];
    if (namesAnExistingFile(argument)) {
      // The argument is a config file, so the task comes from stdin.
      out.configPath = argument;
      return true;
    }
    // The argument names no file, so it is the task and the config comes from
    // the default search.
    out.configPath = config::findDefaultConfig(error);
    if (out.configPath.empty()) {
      return false;
    }
    out.task = argument;
    out.taskFromArgument = true;
    return true;
  }

  out.configPath = config::findDefaultConfig(error);
  return !out.configPath.empty();
}

// Scans the flags and collects the positional arguments. `--` ends flag
// parsing, so a task that begins with a dash can still be passed.
bool parseFlags(const std::vector<std::string>& arguments, Invocation& out, std::vector<std::string>& positional,
                std::string& error) {
  bool endOfFlags = false;
  for (const std::string& argument : arguments) {
    if (endOfFlags) {
      positional.push_back(argument);
      continue;
    }
    if (argument == "--") {
      endOfFlags = true;
      continue;
    }
    if (argument == "--verbose" || argument == "-v") {
      out.level = log::Level::Verbose;
      out.levelSet = true;
      continue;
    }
    if (argument == "--quiet" || argument == "-q") {
      out.level = log::Level::Quiet;
      out.levelSet = true;
      continue;
    }
    if (argument == "--help" || argument == "-h") {
      out.help = true;
      continue;
    }
    if (argument.size() > 1 && argument.front() == '-') {
      error = "Unknown option: " + argument;
      return false;
    }
    positional.push_back(argument);
  }
  return true;
}

// Takes the first positional argument as a command when it matches a reserved
// verb, and leaves the rest for the command to interpret.
void selectCommand(std::vector<std::string>& positional, Invocation& out) {
  out.command = Command::Run;
  out.commandSet = false;
  if (positional.empty()) {
    return;
  }
  Command command = Command::Run;
  if (!reservedVerb(positional.front(), command)) {
    return;
  }
  out.command = command;
  out.commandSet = true;
  positional.erase(positional.begin());
}

bool parseCommandArguments(const std::vector<std::string>& positional, Invocation& out, std::string& error) {
  switch (out.command) {
    case Command::Run:
      return resolveRun(positional, out, error);
    case Command::Init:
    case Command::Sessions:
      if (!positional.empty()) {
        error = std::string("The '") + commandName(out.command) + "' command takes no arguments";
        return false;
      }
      return true;
    case Command::Resume:
      if (positional.size() > 1) {
        error = "The 'resume' command takes at most one session id";
        return false;
      }
      if (!positional.empty()) {
        out.session = positional[0];
      }
      return true;
  }
  error = "Unknown command";
  return false;
}

}  // namespace

bool parse(const std::vector<std::string>& arguments, Invocation& out, std::string& error) {
  std::vector<std::string> positional;
  if (!parseFlags(arguments, out, positional, error)) {
    return false;
  }
  selectCommand(positional, out);

  // Help is answered before any config lookup, so `bjorne_agent --help` works
  // in a directory with no config file.
  if (out.help) {
    return true;
  }
  return parseCommandArguments(positional, out, error);
}

const char* commandName(Command command) {
  switch (command) {
    case Command::Run:
      return "run";
    case Command::Init:
      return "init";
    case Command::Sessions:
      return "sessions";
    case Command::Resume:
      return "resume";
  }
  return "run";
}

std::string usage() {
  return "Usage: bjorne_agent [options] [config] [task]\n"
         "       bjorne_agent [options] run [config] [task]\n"
         "       bjorne_agent [options] init\n"
         "       bjorne_agent [options] sessions\n"
         "       bjorne_agent [options] resume <session>\n"
         "\n"
         "Run a task. With no task argument the task comes from standard input.\n"
         "\n"
         "Commands:\n"
         "  run       Run a task (the default when no command is given)\n"
         "  init      Create a config file\n"
         "  sessions  List stored sessions\n"
         "  resume    Continue a stored session\n"
         "\n"
         "Options:\n"
         "  -h, --help     Show this help and exit\n"
         "  -v, --verbose  Report each step of the run on stderr\n"
         "  -q, --quiet    Suppress progress output\n"
         "  --             Treat the remaining arguments as positionals\n"
         "\n"
         "Environment:\n"
         "  BJORNE_AGENT_LOG=quiet|normal|verbose sets the same level as the flags.\n"
         "  BJORNE_AGENT_CONFIG names the config file when no path is given.\n"
         "\n"
         "Without a config path, the run uses BJORNE_AGENT_CONFIG when it is set,\n"
         "then searches ./.bjorne_agent/bjorne_agent.json and\n"
         "$XDG_CONFIG_HOME/bjorne_agent/bjorne_agent.json, in that order.\n";
}

std::string usage(Command command) {
  switch (command) {
    case Command::Run:
      return "Usage: bjorne_agent [options] run [config] [task]\n"
             "\n"
             "Run a task. The command is the default, so the bare form\n"
             "`bjorne_agent [config] [task]` does the same thing. With no task\n"
             "argument the task comes from standard input.\n"
             "\n"
             "Options:\n"
             "  -h, --help     Show this help and exit\n"
             "  -v, --verbose  Report each step of the run on stderr\n"
             "  -q, --quiet    Suppress progress output\n"
             "  --             Treat the remaining arguments as positionals\n"
             "\n"
             "A task that begins with a reserved word is passed after `run`:\n"
             "  bjorne_agent run \"init the project\"\n";
    case Command::Init:
      return "Usage: bjorne_agent [options] init\n"
             "\n"
             "Create a config file. Not implemented yet.\n";
    case Command::Sessions:
      return "Usage: bjorne_agent [options] sessions\n"
             "\n"
             "List stored sessions. Not implemented yet.\n";
    case Command::Resume:
      return "Usage: bjorne_agent [options] resume <session>\n"
             "\n"
             "Continue a stored session. Not implemented yet.\n";
  }
  return usage();
}

}  // namespace cli
}  // namespace bjorne_agent
