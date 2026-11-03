#include "config/ConfigPaths.h"

#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>

namespace bjorne_agent {
namespace config {

namespace {

constexpr const char* kProjectConfigPath = ".bjorne_agent/bjorne_agent.json";

// The user level config directory follows the XDG base directory spec: the
// XDG_CONFIG_HOME variable when it is set and absolute, otherwise .config
// under the home directory.
std::string userConfigPath() {
  const char* xdgHome = std::getenv("XDG_CONFIG_HOME");
  if (xdgHome != nullptr && xdgHome[0] == '/') {
    return std::string(xdgHome) + "/bjorne_agent/bjorne_agent.json";
  }
  const char* home = std::getenv("HOME");
  if (home == nullptr || home[0] == '\0') {
    return {};
  }
  return std::string(home) + "/.config/bjorne_agent/bjorne_agent.json";
}

// Trims the spaces and tabs around a value, so a variable that holds only
// whitespace counts as unset.
std::string trimmed(const char* value) {
  if (value == nullptr) {
    return {};
  }
  const std::string text(value);
  const size_t first = text.find_first_not_of(" \t");
  if (first == std::string::npos) {
    return {};
  }
  const size_t last = text.find_last_not_of(" \t");
  return text.substr(first, last - first + 1);
}

}  // namespace

std::string environmentConfigPath() {
  return trimmed(std::getenv("BJORNE_AGENT_CONFIG"));
}

std::vector<std::string> defaultConfigPaths() {
  std::vector<std::string> paths;
  paths.emplace_back(kProjectConfigPath);
  const std::string userPath = userConfigPath();
  if (!userPath.empty()) {
    paths.push_back(userPath);
  }
  return paths;
}

std::string findDefaultConfig(std::string& error) {
  // The environment variable is an explicit choice by the caller, so it wins
  // over the search. A variable that names a file that is not there is an
  // error rather than a reason to search on, because the caller meant it.
  std::string fromEnvironment = environmentConfigPath();
  if (!fromEnvironment.empty()) {
    std::error_code code;
    if (std::filesystem::exists(fromEnvironment, code) && !code) {
      return fromEnvironment;
    }
    error = "BJORNE_AGENT_CONFIG names a file that does not exist: " + fromEnvironment;
    return {};
  }

  const std::vector<std::string> paths = defaultConfigPaths();
  for (const std::string& path : paths) {
    std::error_code code;
    if (std::filesystem::exists(path, code) && !code) {
      return path;
    }
  }

  error = "No config file found. Looked for:";
  for (const std::string& path : paths) {
    error += "\n  " + path;
  }
  error += "\nName a config file on the command line, or create one with setup.sh.";
  return {};
}

}  // namespace config
}  // namespace bjorne_agent
