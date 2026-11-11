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

// The tests change HOME, XDG_CONFIG_HOME and BJORNE_AGENT_CONFIG, so each one
// restores them when it finishes. The variables are cleared on the way in, so
// an ambient value cannot change the outcome of a test.
class EnvironmentGuard {
 public:
  EnvironmentGuard() {
    save("HOME", home_);
    save("XDG_CONFIG_HOME", xdg_);
    save("BJORNE_AGENT_CONFIG", config_);
    bjorne_agent::platform::unsetEnvironmentVariable("BJORNE_AGENT_CONFIG");
  }
  ~EnvironmentGuard() {
    restore("HOME", home_);
    restore("XDG_CONFIG_HOME", xdg_);
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
  Saved xdg_;
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

// A temp directory with an empty HOME and a project local config, which is the
// setup most tests want.
struct TestSetup {
  TestSetup() : cwd(dir.str()) { pointHomeAtEmptyDirectory(dir); }

  TempDir dir;
  CurrentDirectoryGuard cwd;
};

std::string userConfig(const TempDir& dir) { return dir.str() + "/.config/bjorne_agent/bjorne_agent.json"; }

}  // namespace

TEST_CASE("two arguments are the config path and the task") {
  const EnvironmentGuard guard;
  TestSetup setup;
  const std::string configPath = setup.dir.str() + "/bjorne_agent.json";
  writeFile(configPath);

  bjorne_agent::cli::Invocation invocation;
  std::string error;
  REQUIRE(bjorne_agent::cli::parse({configPath, "do the thing"}, invocation, error));
  REQUIRE(invocation.command == bjorne_agent::cli::Command::Run);
  REQUIRE_FALSE(invocation.commandSet);
  REQUIRE(invocation.configPath == configPath);
  REQUIRE(invocation.task == "do the thing");
  REQUIRE(invocation.taskFromArgument);
}

TEST_CASE("one argument naming a file is the config path") {
  const EnvironmentGuard guard;
  TestSetup setup;
  const std::string configPath = setup.dir.str() + "/bjorne_agent.json";
  writeFile(configPath);

  bjorne_agent::cli::Invocation invocation;
  std::string error;
  REQUIRE(bjorne_agent::cli::parse({configPath}, invocation, error));
  REQUIRE(invocation.configPath == configPath);
  REQUIRE(invocation.task.empty());
  REQUIRE_FALSE(invocation.taskFromArgument);
}

TEST_CASE("one argument naming no file is the task") {
  const EnvironmentGuard guard;
  TestSetup setup;
  writeFile(userConfig(setup.dir));

  bjorne_agent::cli::Invocation invocation;
  std::string error;
  REQUIRE(bjorne_agent::cli::parse({"do the thing"}, invocation, error));
  REQUIRE(invocation.configPath == userConfig(setup.dir));
  REQUIRE(invocation.task == "do the thing");
  REQUIRE(invocation.taskFromArgument);
}

TEST_CASE("no arguments is a usage error") {
  const EnvironmentGuard guard;
  TestSetup setup;
  writeFile(userConfig(setup.dir));

  bjorne_agent::cli::Invocation invocation;
  std::string error;
  REQUIRE_FALSE(bjorne_agent::cli::parse({}, invocation, error));
  REQUIRE(invocation.showUsage);
  REQUIRE(!error.empty());
}

TEST_CASE("resolution fails when no config file exists") {
  const EnvironmentGuard guard;
  TestSetup setup;

  bjorne_agent::cli::Invocation invocation;
  std::string error;
  REQUIRE_FALSE(bjorne_agent::cli::parse({"run"}, invocation, error));
  REQUIRE(invocation.configPath.empty());
  REQUIRE(!error.empty());
}

TEST_CASE("the run verb is the default command spelled out") {
  const EnvironmentGuard guard;
  TestSetup setup;
  writeFile(userConfig(setup.dir));

  bjorne_agent::cli::Invocation invocation;
  std::string error;
  REQUIRE(bjorne_agent::cli::parse({"run", "do the thing"}, invocation, error));
  REQUIRE(invocation.command == bjorne_agent::cli::Command::Run);
  REQUIRE(invocation.commandSet);
  REQUIRE(invocation.configPath == userConfig(setup.dir));
  REQUIRE(invocation.task == "do the thing");
}

TEST_CASE("the run verb takes a config path and a task") {
  const EnvironmentGuard guard;
  TestSetup setup;
  const std::string configPath = setup.dir.str() + "/bjorne_agent.json";
  writeFile(configPath);

  bjorne_agent::cli::Invocation invocation;
  std::string error;
  REQUIRE(bjorne_agent::cli::parse({"run", configPath, "do the thing"}, invocation, error));
  REQUIRE(invocation.command == bjorne_agent::cli::Command::Run);
  REQUIRE(invocation.configPath == configPath);
  REQUIRE(invocation.task == "do the thing");
}

TEST_CASE("a task that begins with a reserved word is not a command") {
  const EnvironmentGuard guard;
  TestSetup setup;
  writeFile(userConfig(setup.dir));

  bjorne_agent::cli::Invocation invocation;
  std::string error;
  REQUIRE(bjorne_agent::cli::parse({"init the project"}, invocation, error));
  REQUIRE(invocation.command == bjorne_agent::cli::Command::Run);
  REQUIRE_FALSE(invocation.commandSet);
  REQUIRE(invocation.task == "init the project");
}

TEST_CASE("init is a command and needs no config") {
  const EnvironmentGuard guard;
  TestSetup setup;

  bjorne_agent::cli::Invocation invocation;
  std::string error;
  REQUIRE(bjorne_agent::cli::parse({"init"}, invocation, error));
  REQUIRE(invocation.command == bjorne_agent::cli::Command::Init);
  REQUIRE(invocation.commandSet);
  REQUIRE(invocation.configPath.empty());
}

TEST_CASE("sessions is a command") {
  const EnvironmentGuard guard;
  TestSetup setup;

  bjorne_agent::cli::Invocation invocation;
  std::string error;
  REQUIRE(bjorne_agent::cli::parse({"sessions"}, invocation, error));
  REQUIRE(invocation.command == bjorne_agent::cli::Command::Sessions);
  REQUIRE(invocation.commandSet);
}

TEST_CASE("resume takes a session id") {
  const EnvironmentGuard guard;
  TestSetup setup;

  bjorne_agent::cli::Invocation invocation;
  std::string error;
  REQUIRE(bjorne_agent::cli::parse({"resume", "abc123"}, invocation, error));
  REQUIRE(invocation.command == bjorne_agent::cli::Command::Resume);
  REQUIRE(invocation.commandSet);
  REQUIRE(invocation.session == "abc123");
}

TEST_CASE("resume rejects more than one session id") {
  const EnvironmentGuard guard;
  TestSetup setup;

  bjorne_agent::cli::Invocation invocation;
  std::string error;
  REQUIRE_FALSE(bjorne_agent::cli::parse({"resume", "one", "two"}, invocation, error));
  REQUIRE(error.find("resume") != std::string::npos);
}

TEST_CASE("init rejects arguments") {
  const EnvironmentGuard guard;
  TestSetup setup;

  bjorne_agent::cli::Invocation invocation;
  std::string error;
  REQUIRE_FALSE(bjorne_agent::cli::parse({"init", "extra"}, invocation, error));
  REQUIRE(error.find("init") != std::string::npos);
}

TEST_CASE("an unknown option is an error") {
  const EnvironmentGuard guard;
  TestSetup setup;
  writeFile(userConfig(setup.dir));

  bjorne_agent::cli::Invocation invocation;
  std::string error;
  REQUIRE_FALSE(bjorne_agent::cli::parse({"--bogus"}, invocation, error));
  REQUIRE(error.find("--bogus") != std::string::npos);
}

TEST_CASE("flags may follow the command") {
  const EnvironmentGuard guard;
  TestSetup setup;
  writeFile(userConfig(setup.dir));

  bjorne_agent::cli::Invocation invocation;
  std::string error;
  REQUIRE(bjorne_agent::cli::parse({"run", "--verbose", "do the thing"}, invocation, error));
  REQUIRE(invocation.command == bjorne_agent::cli::Command::Run);
  REQUIRE(invocation.levelSet);
  REQUIRE(invocation.level == bjorne_agent::log::Level::Verbose);
  REQUIRE(invocation.task == "do the thing");
}

TEST_CASE("a double dash ends flag parsing") {
  const EnvironmentGuard guard;
  TestSetup setup;
  writeFile(userConfig(setup.dir));

  bjorne_agent::cli::Invocation invocation;
  std::string error;
  REQUIRE(bjorne_agent::cli::parse({"--", "--not-a-flag"}, invocation, error));
  REQUIRE(invocation.task == "--not-a-flag");
  REQUIRE_FALSE(invocation.levelSet);
}

TEST_CASE("help needs no config") {
  const EnvironmentGuard guard;
  TestSetup setup;

  bjorne_agent::cli::Invocation invocation;
  std::string error;
  REQUIRE(bjorne_agent::cli::parse({"--help"}, invocation, error));
  REQUIRE(invocation.help);
  REQUIRE(invocation.command == bjorne_agent::cli::Command::Run);
  REQUIRE_FALSE(invocation.commandSet);
}

TEST_CASE("help after a command selects that command's help") {
  const EnvironmentGuard guard;
  TestSetup setup;

  bjorne_agent::cli::Invocation invocation;
  std::string error;
  REQUIRE(bjorne_agent::cli::parse({"resume", "--help"}, invocation, error));
  REQUIRE(invocation.help);
  REQUIRE(invocation.command == bjorne_agent::cli::Command::Resume);
  REQUIRE(invocation.commandSet);
  REQUIRE(bjorne_agent::cli::usage(invocation.command).find("resume") != std::string::npos);
}

TEST_CASE("the quiet flag sets the quiet level") {
  const EnvironmentGuard guard;
  TestSetup setup;
  writeFile(userConfig(setup.dir));

  bjorne_agent::cli::Invocation invocation;
  std::string error;
  REQUIRE(bjorne_agent::cli::parse({"-q", "do the thing"}, invocation, error));
  REQUIRE(invocation.levelSet);
  REQUIRE(invocation.level == bjorne_agent::log::Level::Quiet);
}

TEST_CASE("run rejects more than a config path and a task") {
  const EnvironmentGuard guard;
  TestSetup setup;
  writeFile(userConfig(setup.dir));

  bjorne_agent::cli::Invocation invocation;
  std::string error;
  REQUIRE_FALSE(bjorne_agent::cli::parse({"run", "one", "two", "three"}, invocation, error));
  REQUIRE(error.find("run") != std::string::npos);
}

TEST_CASE("sessions rejects arguments") {
  const EnvironmentGuard guard;
  TestSetup setup;

  bjorne_agent::cli::Invocation invocation;
  std::string error;
  REQUIRE_FALSE(bjorne_agent::cli::parse({"sessions", "extra"}, invocation, error));
  REQUIRE(error.find("sessions") != std::string::npos);
}

TEST_CASE("resume with no session id parses") {
  const EnvironmentGuard guard;
  TestSetup setup;

  bjorne_agent::cli::Invocation invocation;
  std::string error;
  REQUIRE(bjorne_agent::cli::parse({"resume"}, invocation, error));
  REQUIRE(invocation.command == bjorne_agent::cli::Command::Resume);
  REQUIRE(invocation.session.empty());
}

TEST_CASE("an unknown option after a command is an error") {
  const EnvironmentGuard guard;
  TestSetup setup;

  bjorne_agent::cli::Invocation invocation;
  std::string error;
  REQUIRE_FALSE(bjorne_agent::cli::parse({"run", "--bogus"}, invocation, error));
  REQUIRE(error.find("--bogus") != std::string::npos);
}

TEST_CASE("every command has help text that names it") {
  for (const bjorne_agent::cli::Command command :
       {bjorne_agent::cli::Command::Run, bjorne_agent::cli::Command::Init, bjorne_agent::cli::Command::Sessions,
        bjorne_agent::cli::Command::Resume}) {
    const std::string name = bjorne_agent::cli::commandName(command);
    REQUIRE_FALSE(name.empty());
    REQUIRE(bjorne_agent::cli::usage(command).find(name) != std::string::npos);
  }
}

TEST_CASE("the top level usage names every command") {
  const std::string text = bjorne_agent::cli::usage();
  REQUIRE(text.find("run") != std::string::npos);
  REQUIRE(text.find("init") != std::string::npos);
  REQUIRE(text.find("sessions") != std::string::npos);
  REQUIRE(text.find("resume") != std::string::npos);
}
