#include "config/ConfigPaths.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <unistd.h>
#include <vector>

#include <catch_amalgamated.hpp>

namespace {

// The tests change HOME and XDG_CONFIG_HOME, so each one restores them when
// it finishes.
class EnvironmentGuard {
 public:
  EnvironmentGuard() {
    const char* home = std::getenv("HOME");
    const char* xdg = std::getenv("XDG_CONFIG_HOME");
    hadHome_ = home != nullptr;
    hadXdg_ = xdg != nullptr;
    if (hadHome_) {
      oldHome_ = home;
    }
    if (hadXdg_) {
      oldXdg_ = xdg;
    }
    const char* config = std::getenv("BJORNE_AGENT_CONFIG");
    hadConfig_ = config != nullptr;
    if (hadConfig_) {
      oldConfig_ = config;
    }
    // An ambient value must not change the outcome of a test.
    ::unsetenv("BJORNE_AGENT_CONFIG");
  }
  ~EnvironmentGuard() {
    if (hadHome_) {
      ::setenv("HOME", oldHome_.c_str(), 1);
    } else {
      ::unsetenv("HOME");
    }
    if (hadXdg_) {
      ::setenv("XDG_CONFIG_HOME", oldXdg_.c_str(), 1);
    } else {
      ::unsetenv("XDG_CONFIG_HOME");
    }
    if (hadConfig_) {
      ::setenv("BJORNE_AGENT_CONFIG", oldConfig_.c_str(), 1);
    } else {
      ::unsetenv("BJORNE_AGENT_CONFIG");
    }
  }

 private:
  bool hadHome_ = false;
  bool hadXdg_ = false;
  bool hadConfig_ = false;
  std::string oldHome_;
  std::string oldXdg_;
  std::string oldConfig_;
};

// A directory that holds the config file for one test, removed afterwards.
class TempDir {
 public:
  TempDir() {
    static int counter = 0;
    path_ = std::filesystem::temp_directory_path() /
            ("bjorne_agent_paths_test_" + std::to_string(::getpid()) + "_" + std::to_string(++counter));
    std::filesystem::create_directories(path_);
  }
  ~TempDir() {
    std::error_code code;
    std::filesystem::remove_all(path_, code);
  }
  TempDir(const TempDir&) = delete;
  TempDir& operator=(const TempDir&) = delete;

  std::string str() const { return path_.string(); }

 private:
  std::filesystem::path path_;
};

void writeFile(const std::string& path) {
  std::filesystem::create_directories(std::filesystem::path(path).parent_path());
  std::ofstream file(path);
  file << "{}";
}

}  // namespace

TEST_CASE("default config paths start with the project local file") {
  const EnvironmentGuard guard;
  ::unsetenv("XDG_CONFIG_HOME");
  ::setenv("HOME", "/home/tester", 1);

  const std::vector<std::string> paths = bjorne_agent::config::defaultConfigPaths();
  REQUIRE(paths.size() == 2);
  REQUIRE(paths[0] == ".bjorne_agent/bjorne_agent.json");
  REQUIRE(paths[1] == "/home/tester/.config/bjorne_agent/bjorne_agent.json");
}

TEST_CASE("XDG_CONFIG_HOME replaces the home directory default") {
  const EnvironmentGuard guard;
  ::setenv("HOME", "/home/tester", 1);
  ::setenv("XDG_CONFIG_HOME", "/xdg", 1);

  const std::vector<std::string> paths = bjorne_agent::config::defaultConfigPaths();
  REQUIRE(paths.size() == 2);
  REQUIRE(paths[1] == "/xdg/bjorne_agent/bjorne_agent.json");
}

TEST_CASE("a relative XDG_CONFIG_HOME is ignored") {
  const EnvironmentGuard guard;
  ::setenv("HOME", "/home/tester", 1);
  // The XDG spec says a relative value must be ignored.
  ::setenv("XDG_CONFIG_HOME", "relative/path", 1);

  const std::vector<std::string> paths = bjorne_agent::config::defaultConfigPaths();
  REQUIRE(paths.size() == 2);
  REQUIRE(paths[1] == "/home/tester/.config/bjorne_agent/bjorne_agent.json");
}

TEST_CASE("no home directory leaves only the project local path") {
  const EnvironmentGuard guard;
  ::unsetenv("XDG_CONFIG_HOME");
  ::unsetenv("HOME");

  const std::vector<std::string> paths = bjorne_agent::config::defaultConfigPaths();
  REQUIRE(paths.size() == 1);
  REQUIRE(paths[0] == ".bjorne_agent/bjorne_agent.json");
}

TEST_CASE("findDefaultConfig reports where it looked when nothing exists") {
  const EnvironmentGuard guard;
  ::setenv("HOME", "/nonexistent-home-for-bjorne_agent-tests", 1);
  ::unsetenv("XDG_CONFIG_HOME");

  std::string error;
  const std::string path = bjorne_agent::config::findDefaultConfig(error);
  REQUIRE(path.empty());
  REQUIRE(error.find(".bjorne_agent/bjorne_agent.json") != std::string::npos);
  REQUIRE(error.find("setup.sh") != std::string::npos);
}

TEST_CASE("findDefaultConfig finds the user level file") {
  const EnvironmentGuard guard;
  TempDir dir;
  ::unsetenv("XDG_CONFIG_HOME");
  ::setenv("HOME", dir.str().c_str(), 1);

  const std::string expected = dir.str() + "/.config/bjorne_agent/bjorne_agent.json";
  writeFile(expected);

  std::string error;
  const std::string path = bjorne_agent::config::findDefaultConfig(error);
  REQUIRE(path == expected);
}

TEST_CASE("the environment variable names the config file") {
  const EnvironmentGuard guard;
  TempDir dir;
  const std::string expected = dir.str() + "/mounted.json";
  writeFile(expected);
  ::setenv("BJORNE_AGENT_CONFIG", expected.c_str(), 1);

  REQUIRE(bjorne_agent::config::environmentConfigPath() == expected);

  std::string error;
  const std::string path = bjorne_agent::config::findDefaultConfig(error);
  REQUIRE(path == expected);
}

TEST_CASE("the environment variable wins over the search") {
  const EnvironmentGuard guard;
  TempDir dir;
  ::unsetenv("XDG_CONFIG_HOME");
  ::setenv("HOME", dir.str().c_str(), 1);
  const std::string fromSearch = dir.str() + "/.config/bjorne_agent/bjorne_agent.json";
  writeFile(fromSearch);

  const std::string fromEnvironment = dir.str() + "/mounted.json";
  writeFile(fromEnvironment);
  ::setenv("BJORNE_AGENT_CONFIG", fromEnvironment.c_str(), 1);

  std::string error;
  const std::string path = bjorne_agent::config::findDefaultConfig(error);
  REQUIRE(path == fromEnvironment);
}

TEST_CASE("an environment variable naming a missing file is an error") {
  const EnvironmentGuard guard;
  TempDir dir;
  ::unsetenv("XDG_CONFIG_HOME");
  ::setenv("HOME", dir.str().c_str(), 1);
  const std::string missing = dir.str() + "/not-there.json";
  ::setenv("BJORNE_AGENT_CONFIG", missing.c_str(), 1);

  std::string error;
  const std::string path = bjorne_agent::config::findDefaultConfig(error);
  REQUIRE(path.empty());
  REQUIRE(error.find("BJORNE_AGENT_CONFIG") != std::string::npos);
  REQUIRE(error.find(missing) != std::string::npos);
}

TEST_CASE("a blank environment variable counts as unset") {
  const EnvironmentGuard guard;
  ::setenv("BJORNE_AGENT_CONFIG", "   ", 1);

  REQUIRE(bjorne_agent::config::environmentConfigPath().empty());
}
