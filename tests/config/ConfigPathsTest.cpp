#include "config/ConfigPaths.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "platform/Platform.h"

#include <catch_amalgamated.hpp>

namespace {

// The tests change HOME and XDG_CONFIG_HOME, so each one restores them when
// it finishes.
class EnvironmentGuard {
 public:
  EnvironmentGuard() {
    save("HOME", home_);
    save("XDG_CONFIG_HOME", xdg_);
    save("BJORNE_AGENT_CONFIG", config_);
    save("APPDATA", appData_);
    save("USERPROFILE", userProfile_);
    // An ambient value must not change the outcome of a test.
    bjorne_agent::platform::unsetEnvironmentVariable("BJORNE_AGENT_CONFIG");
  }
  ~EnvironmentGuard() {
    restore("HOME", home_);
    restore("XDG_CONFIG_HOME", xdg_);
    restore("BJORNE_AGENT_CONFIG", config_);
    restore("APPDATA", appData_);
    restore("USERPROFILE", userProfile_);
  }

 private:
  struct Saved {
    bool had = false;
    std::string value;
  };

  static void save(const std::string& name, Saved& out) {
    out.had = bjorne_agent::platform::getEnvironmentVariable(name, out.value);
  }

  static void restore(const std::string& name, const Saved& saved) {
    if (saved.had) {
      bjorne_agent::platform::setEnvironmentVariable(name, saved.value);
    } else {
      bjorne_agent::platform::unsetEnvironmentVariable(name);
    }
  }

  Saved home_;
  Saved xdg_;
  Saved config_;
  Saved appData_;
  Saved userProfile_;
};

// A directory that holds the config file for one test, removed afterwards.
class TempDir {
 public:
  TempDir() {
    static int counter = 0;
    path_ = std::filesystem::temp_directory_path() /
            ("bjorne_agent_paths_test_" + std::to_string(bjorne_agent::platform::processId()) + "_" + std::to_string(++counter));
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

TEST_CASE("the user config directory follows the XDG rules") {
  const EnvironmentGuard guard;
  bjorne_agent::platform::setEnvironmentVariable("HOME", "/home/tester");
  bjorne_agent::platform::setEnvironmentVariable("XDG_CONFIG_HOME", "/xdg");

  REQUIRE(bjorne_agent::config::userConfigDirectory() == "/xdg/bjorne_agent");

  bjorne_agent::platform::unsetEnvironmentVariable("XDG_CONFIG_HOME");
  REQUIRE(bjorne_agent::config::userConfigDirectory() == "/home/tester/.config/bjorne_agent");
}

TEST_CASE("default config paths start with the project local file") {
  const EnvironmentGuard guard;
  bjorne_agent::platform::unsetEnvironmentVariable("XDG_CONFIG_HOME");
  bjorne_agent::platform::setEnvironmentVariable("HOME", "/home/tester");

  const std::vector<std::string> paths = bjorne_agent::config::defaultConfigPaths();
  REQUIRE(paths.size() == 2);
  REQUIRE(paths[0] == ".bjorne_agent/bjorne_agent.json");
  REQUIRE(paths[1] == "/home/tester/.config/bjorne_agent/bjorne_agent.json");
}

TEST_CASE("XDG_CONFIG_HOME replaces the home directory default") {
  const EnvironmentGuard guard;
  bjorne_agent::platform::setEnvironmentVariable("HOME", "/home/tester");
  bjorne_agent::platform::setEnvironmentVariable("XDG_CONFIG_HOME", "/xdg");

  const std::vector<std::string> paths = bjorne_agent::config::defaultConfigPaths();
  REQUIRE(paths.size() == 2);
  REQUIRE(paths[1] == "/xdg/bjorne_agent/bjorne_agent.json");
}

TEST_CASE("a trailing separator in XDG_CONFIG_HOME is not doubled") {
  const EnvironmentGuard guard;
  bjorne_agent::platform::setEnvironmentVariable("HOME", "/home/tester");
  bjorne_agent::platform::setEnvironmentVariable("XDG_CONFIG_HOME", "/xdg/");

  const std::vector<std::string> paths = bjorne_agent::config::defaultConfigPaths();
  REQUIRE(paths.size() == 2);
  REQUIRE(paths[1] == "/xdg/bjorne_agent/bjorne_agent.json");
}

TEST_CASE("a relative XDG_CONFIG_HOME is ignored") {
  const EnvironmentGuard guard;
  bjorne_agent::platform::setEnvironmentVariable("HOME", "/home/tester");
  // The XDG spec says a relative value must be ignored.
  bjorne_agent::platform::setEnvironmentVariable("XDG_CONFIG_HOME", "relative/path");

  const std::vector<std::string> paths = bjorne_agent::config::defaultConfigPaths();
  REQUIRE(paths.size() == 2);
  REQUIRE(paths[1] == "/home/tester/.config/bjorne_agent/bjorne_agent.json");
}

TEST_CASE("no home directory leaves only the project local path") {
  const EnvironmentGuard guard;
  bjorne_agent::platform::unsetEnvironmentVariable("XDG_CONFIG_HOME");
  bjorne_agent::platform::unsetEnvironmentVariable("HOME");
  // Windows falls back to the application data directories, so they are
  // cleared too to make the case meaningful there.
  bjorne_agent::platform::unsetEnvironmentVariable("APPDATA");
  bjorne_agent::platform::unsetEnvironmentVariable("USERPROFILE");

  const std::vector<std::string> paths = bjorne_agent::config::defaultConfigPaths();
  REQUIRE(paths.size() == 1);
  REQUIRE(paths[0] == ".bjorne_agent/bjorne_agent.json");
}

TEST_CASE("findDefaultConfig reports where it looked when nothing exists") {
  const EnvironmentGuard guard;
  bjorne_agent::platform::setEnvironmentVariable("HOME", "/nonexistent-home-for-bjorne_agent-tests");
  bjorne_agent::platform::unsetEnvironmentVariable("XDG_CONFIG_HOME");

  std::string error;
  const std::string path = bjorne_agent::config::findDefaultConfig(error);
  REQUIRE(path.empty());
  REQUIRE(error.find(".bjorne_agent/bjorne_agent.json") != std::string::npos);
  REQUIRE(error.find("setup.sh") != std::string::npos);
}

TEST_CASE("findDefaultConfig finds the user level file") {
  const EnvironmentGuard guard;
  TempDir dir;
  bjorne_agent::platform::unsetEnvironmentVariable("XDG_CONFIG_HOME");
  bjorne_agent::platform::setEnvironmentVariable("HOME", dir.str().c_str());

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
  bjorne_agent::platform::setEnvironmentVariable("BJORNE_AGENT_CONFIG", expected.c_str());

  REQUIRE(bjorne_agent::config::environmentConfigPath() == expected);

  std::string error;
  const std::string path = bjorne_agent::config::findDefaultConfig(error);
  REQUIRE(path == expected);
}

TEST_CASE("the environment variable wins over the search") {
  const EnvironmentGuard guard;
  TempDir dir;
  bjorne_agent::platform::unsetEnvironmentVariable("XDG_CONFIG_HOME");
  bjorne_agent::platform::setEnvironmentVariable("HOME", dir.str().c_str());
  const std::string fromSearch = dir.str() + "/.config/bjorne_agent/bjorne_agent.json";
  writeFile(fromSearch);

  const std::string fromEnvironment = dir.str() + "/mounted.json";
  writeFile(fromEnvironment);
  bjorne_agent::platform::setEnvironmentVariable("BJORNE_AGENT_CONFIG", fromEnvironment.c_str());

  std::string error;
  const std::string path = bjorne_agent::config::findDefaultConfig(error);
  REQUIRE(path == fromEnvironment);
}

TEST_CASE("an environment variable naming a missing file is an error") {
  const EnvironmentGuard guard;
  TempDir dir;
  bjorne_agent::platform::unsetEnvironmentVariable("XDG_CONFIG_HOME");
  bjorne_agent::platform::setEnvironmentVariable("HOME", dir.str().c_str());
  const std::string missing = dir.str() + "/not-there.json";
  bjorne_agent::platform::setEnvironmentVariable("BJORNE_AGENT_CONFIG", missing.c_str());

  std::string error;
  const std::string path = bjorne_agent::config::findDefaultConfig(error);
  REQUIRE(path.empty());
  REQUIRE(error.find("BJORNE_AGENT_CONFIG") != std::string::npos);
  REQUIRE(error.find(missing) != std::string::npos);
}

TEST_CASE("a blank environment variable counts as unset") {
  const EnvironmentGuard guard;
  bjorne_agent::platform::setEnvironmentVariable("BJORNE_AGENT_CONFIG", "   ");

  REQUIRE(bjorne_agent::config::environmentConfigPath().empty());
}
