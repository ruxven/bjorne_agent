#include "config/ConfigPaths.h"

#include <cctype>
#include <filesystem>
#include <string>
#include <vector>

#include "platform/Platform.h"

namespace bjorne_agent {
namespace config {

namespace {

constexpr const char* kProjectConfigPath = ".bjorne_agent/bjorne_agent.json";
constexpr const char* kUserConfigPath = "bjorne_agent/bjorne_agent.json";

// Windows accepts either separator; POSIX has only the forward slash, and a
// backslash is an ordinary character in a POSIX file name.
bool isPathSeparator(char c) {
#if defined(_WIN32)
  return c == '/' || c == '\\';
#else
  return c == '/';
#endif
}

// True for a path that starts at the root: /dir on POSIX, C:\dir or \\server
// on Windows.
bool isAbsolutePath(const std::string& path) {
  if (path.empty()) {
    return false;
  }
  if (isPathSeparator(path.front())) {
    return true;
  }
#if defined(_WIN32)
  if (path.size() >= 3 && std::isalpha(static_cast<unsigned char>(path[0])) != 0 && path[1] == ':' &&
      isPathSeparator(path[2])) {
    return true;
  }
#endif
  return false;
}

// Joins a directory and a relative path. The separator is always a forward
// slash, which both platforms accept.
std::string joinPath(const std::string& base, const std::string& rest) {
  if (base.empty()) {
    return rest;
  }
  if (isPathSeparator(base.back())) {
    return base + rest;
  }
  return base + "/" + rest;
}

// The user level config directory follows the XDG base directory spec where it
// applies: the XDG_CONFIG_HOME variable when it is set and absolute, otherwise
// .config under the home directory. Windows has no XDG variable by default, so
// the roaming application data directory is used instead.
std::string userConfigPath() {
  std::string xdgHome;
  if (platform::getEnvironmentVariable("XDG_CONFIG_HOME", xdgHome) && isAbsolutePath(xdgHome)) {
    return joinPath(xdgHome, kUserConfigPath);
  }
  std::string home;
  if (platform::getEnvironmentVariable("HOME", home) && !home.empty()) {
    return joinPath(joinPath(home, ".config"), kUserConfigPath);
  }
#if defined(_WIN32)
  // Windows has no HOME by default, so the roaming application data directory
  // is the user level location.
  std::string appData;
  if (platform::getEnvironmentVariable("APPDATA", appData) && !appData.empty()) {
    return joinPath(appData, kUserConfigPath);
  }
  std::string profile;
  if (platform::getEnvironmentVariable("USERPROFILE", profile) && !profile.empty()) {
    return joinPath(joinPath(profile, ".config"), kUserConfigPath);
  }
#endif
  return {};
}

// Trims the spaces and tabs around a value, so a variable that holds only
// whitespace counts as unset.
std::string trimmed(const std::string& value) {
  const size_t first = value.find_first_not_of(" \t");
  if (first == std::string::npos) {
    return {};
  }
  const size_t last = value.find_last_not_of(" \t");
  return value.substr(first, last - first + 1);
}

}  // namespace

std::string environmentConfigPath() {
  std::string value;
  if (!platform::getEnvironmentVariable("BJORNE_AGENT_CONFIG", value)) {
    return {};
  }
  return trimmed(value);
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
