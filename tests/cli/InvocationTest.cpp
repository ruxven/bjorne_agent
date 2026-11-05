#include "cli/Invocation.h"

#include "config/ConfigPaths.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "platform/Platform.h"

#include <catch_amalgamated.hpp>

namespace {

// The tests change HOME and BJORNE_AGENT_CONFIG, so each one restores them when it
// finishes. The variables are cleared on the way in, so an ambient value
// cannot change the outcome of a test.
class EnvironmentGuard {
 public:
  EnvironmentGuard() {
    save("HOME", home_);
    save("BJORNE_AGENT_CONFIG", config_);
    bjorne_agent::platform::unsetEnvironmentVariable("BJORNE_AGENT_CONFIG");
  }
  ~EnvironmentGuard() {
    restore("HOME", home_);
    restore("BJORNE_AGENT_CONFIG", config_);
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
  Saved config_;
};

// A directory holding the config file for one test, removed afterwards.
class TempDir {
 public:
  TempDir() {
    static int counter = 0;
    path_ = std::filesystem::temp_directory_path() /
            ("bjorne_agent_invocation_test_" + std::to_string(bjorne_agent::platform::processId()) + "_" + std::to_string(++counter));
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

// Moves into a directory for the duration of a test, so that a project local
// config file in the build directory cannot change the outcome.
class CurrentDirectoryGuard {
 public:
  explicit CurrentDirectoryGuard(const std::string& path) : previous_(std::filesystem::current_path()) {
    std::error_code code;
    std::filesystem::current_path(path, code);
  }
  ~CurrentDirectoryGuard() {
    std::error_code code;
    std::filesystem::current_path(previous_, code);
  }
  CurrentDirectoryGuard(const CurrentDirectoryGuard&) = delete;
  CurrentDirectoryGuard& operator=(const CurrentDirectoryGuard&) = delete;

 private:
  std::filesystem::path previous_;
};

// Points HOME at a directory that holds no config file, so the default search
// only finds a project local file.
void pointHomeAtEmptyDirectory(const TempDir& dir) {
  bjorne_agent::platform::setEnvironmentVariable("HOME", dir.str().c_str());
  bjorne_agent::platform::unsetEnvironmentVariable("XDG_CONFIG_HOME");
}

}  // namespace

TEST_CASE("two arguments are the config path and the task") {
  const EnvironmentGuard guard;
  TempDir dir;
  pointHomeAtEmptyDirectory(dir);
  const std::string configPath = dir.str() + "/bjorne_agent.json";
  writeFile(configPath);

  bjorne_agent::cli::Invocation invocation;
  std::string error;
  REQUIRE(bjorne_agent::cli::resolveInvocation({configPath, "do the thing"}, invocation, error));
  REQUIRE(invocation.configPath == configPath);
  REQUIRE(invocation.task == "do the thing");
  REQUIRE(invocation.taskFromArgument);
}

TEST_CASE("one argument naming a file is the config path") {
  const EnvironmentGuard guard;
  TempDir dir;
  pointHomeAtEmptyDirectory(dir);
  const std::string configPath = dir.str() + "/bjorne_agent.json";
  writeFile(configPath);

  bjorne_agent::cli::Invocation invocation;
  std::string error;
  REQUIRE(bjorne_agent::cli::resolveInvocation({configPath}, invocation, error));
  REQUIRE(invocation.configPath == configPath);
  REQUIRE(invocation.task.empty());
  REQUIRE_FALSE(invocation.taskFromArgument);
}

TEST_CASE("one argument naming no file is the task") {
  const EnvironmentGuard guard;
  TempDir dir;
  pointHomeAtEmptyDirectory(dir);
  const CurrentDirectoryGuard cwd(dir.str());
  writeFile(dir.str() + "/.config/bjorne_agent/bjorne_agent.json");

  bjorne_agent::cli::Invocation invocation;
  std::string error;
  REQUIRE(bjorne_agent::cli::resolveInvocation({"do the thing"}, invocation, error));
  REQUIRE(invocation.configPath == dir.str() + "/.config/bjorne_agent/bjorne_agent.json");
  REQUIRE(invocation.task == "do the thing");
  REQUIRE(invocation.taskFromArgument);
}

TEST_CASE("no arguments take the config from the search and the task from stdin") {
  const EnvironmentGuard guard;
  TempDir dir;
  pointHomeAtEmptyDirectory(dir);
  const CurrentDirectoryGuard cwd(dir.str());
  const std::string configPath = dir.str() + "/.config/bjorne_agent/bjorne_agent.json";
  writeFile(configPath);

  bjorne_agent::cli::Invocation invocation;
  std::string error;
  REQUIRE(bjorne_agent::cli::resolveInvocation({}, invocation, error));
  REQUIRE(invocation.configPath == configPath);
  REQUIRE(invocation.task.empty());
  REQUIRE_FALSE(invocation.taskFromArgument);
}

TEST_CASE("resolution fails when no config file exists") {
  const EnvironmentGuard guard;
  TempDir dir;
  pointHomeAtEmptyDirectory(dir);
  const CurrentDirectoryGuard cwd(dir.str());

  bjorne_agent::cli::Invocation invocation;
  std::string error;
  REQUIRE_FALSE(bjorne_agent::cli::resolveInvocation({}, invocation, error));
  REQUIRE(invocation.configPath.empty());
  REQUIRE(!error.empty());
}
