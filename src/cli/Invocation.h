#pragma once

#include <string>
#include <vector>

namespace bjorne_agent {
namespace cli {

// What the command line asked for. The config path is empty when the default
// search found nothing.
struct Invocation {
  std::string configPath;
  std::string task;
  bool taskFromArgument = false;
};

// Resolves the positional arguments into a config path and a task.
//
// Two positional arguments are the config path and the task. One positional
// argument that names an existing file is the config path, so the task then
// comes from stdin. One positional argument that names no file is the task, so
// the config comes from the default search. With no positional arguments the
// config comes from the default search and the task comes from stdin.
//
// Returns false when no config file could be found, with the reason in error.
bool resolveInvocation(const std::vector<std::string>& arguments, Invocation& out, std::string& error);

}  // namespace cli
}  // namespace bjorne_agent
