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

}  // namespace

bool resolveInvocation(const std::vector<std::string>& arguments, Invocation& out, std::string& error) {
  if (arguments.size() >= 2) {
    // Two arguments are the config path and the task.
    out.configPath = arguments[0];
    out.task = arguments[1];
    out.taskFromArgument = true;
    return true;
  }

  if (arguments.size() == 1) {
    const std::string& argument = arguments[0];
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

}  // namespace cli
}  // namespace bjorne_agent
