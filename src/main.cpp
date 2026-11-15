#include <iostream>
#include <string>
#include <vector>

#include "app/App.h"
#include "cli/Invocation.h"
#include "log/Log.h"
#include "platform/Platform.h"
#include "platform/Shutdown.h"

namespace {

// Reads BJORNE_AGENT_LOG into the level. Returns false when the variable holds
// an unknown level.
bool applyLogEnvironment(bjorne_agent::log::Level& level) {
  std::string configured;
  if (!bjorne_agent::platform::getEnvironmentVariable("BJORNE_AGENT_LOG", configured)) {
    return true;
  }
  if (!bjorne_agent::log::parseLevel(configured, level)) {
    std::cerr << "Unknown BJORNE_AGENT_LOG level: " << configured << "\n";
    return false;
  }
  return true;
}

// Parses the command line and reports whether the run may continue. The usage
// text is printed when the parse failed in a way the user can fix by naming a
// command.
bool parseCommandLine(int argc, char** argv, bjorne_agent::cli::Invocation& invocation) {
  std::vector<std::string> arguments;
  for (int index = 1; index < argc; ++index) {
    arguments.emplace_back(argv[index]);
  }

  std::string error;
  if (bjorne_agent::cli::parse(arguments, invocation, error)) {
    return true;
  }
  bjorne_agent::log::fail(error);
  std::cerr << error << "\n";
  if (invocation.showUsage) {
    std::cerr << "\n" << bjorne_agent::cli::usage();
  }
  return false;
}

// A command line flag wins over the environment variable. The library starts
// quiet so that embedding it produces no output; the binary defaults to
// normal so the user sees what it is doing.
bool applyLogLevel(const bjorne_agent::cli::Invocation& invocation) {
  bjorne_agent::log::Level level = invocation.levelSet ? invocation.level : bjorne_agent::log::Level::Normal;
  if (!invocation.levelSet && !applyLogEnvironment(level)) {
    return false;
  }
  bjorne_agent::log::setLevel(level);
  return true;
}

// Prints the help text for the command that was named, or the top level help
// when no command was named.
void printHelp(const bjorne_agent::cli::Invocation& invocation) {
  if (invocation.commandSet) {
    std::cout << bjorne_agent::cli::usage(invocation.command);
  } else {
    std::cout << bjorne_agent::cli::usage();
  }
}

}  // namespace

int main(int argc, char** argv) {
  if (!bjorne_agent::platform::installShutdownHandlers()) {
    std::cerr << "Cannot install the shutdown handlers; an interrupted session may not be saved.\n";
  }

  bjorne_agent::cli::Invocation invocation;
  if (!parseCommandLine(argc, argv, invocation)) {
    return 1;
  }

  if (invocation.help) {
    printHelp(invocation);
    return 0;
  }

  if (!applyLogLevel(invocation)) {
    return 1;
  }

  bjorne_agent::app::App app;
  return app.run(invocation);
}
