#include "platform/Process.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

#include <catch_amalgamated.hpp>

#if !defined(_WIN32)
#include <unistd.h>
#endif

using bjorne_agent::platform::ChildProcess;
using bjorne_agent::platform::ReadStatus;

namespace {

// The tests drive the child process transport against a stub that echoes each
// line back. Neither shell would do: cmd.exe prints a banner and treats every
// input line as a command, and a POSIX shell adds a prompt of its own.
const char* kEchoCommand = BJORNE_AGENT_ECHO_STUB;

}  // namespace

TEST_CASE("a child process echoes a line back") {
  ChildProcess process;
  std::string error;
  REQUIRE(process.start(kEchoCommand, {}, {}, error));
  REQUIRE(process.started());
  REQUIRE(process.running());

  REQUIRE(process.sendLine("hello", error));
  std::string line;
  REQUIRE(process.readLine(2000, line, error) == ReadStatus::Line);
  REQUIRE(line == "hello");

  process.stop();
}

TEST_CASE("a child process reads several lines") {
  ChildProcess process;
  std::string error;
  REQUIRE(process.start(kEchoCommand, {}, {}, error));

  REQUIRE(process.sendLine("first", error));
  REQUIRE(process.sendLine("second", error));

  std::string line;
  REQUIRE(process.readLine(2000, line, error) == ReadStatus::Line);
  REQUIRE(line == "first");
  REQUIRE(process.readLine(2000, line, error) == ReadStatus::Line);
  REQUIRE(line == "second");

  process.stop();
}

TEST_CASE("a read without data times out") {
  ChildProcess process;
  std::string error;
  REQUIRE(process.start(kEchoCommand, {}, {}, error));

  std::string line;
  REQUIRE(process.readLine(100, line, error) == ReadStatus::Timeout);

  process.stop();
}

TEST_CASE("a closed child process reports the closed pipe") {
  ChildProcess process;
  std::string error;
  REQUIRE(process.start(kEchoCommand, {}, {}, error));
  process.stop();

  std::string line;
  REQUIRE(process.readLine(100, line, error) == ReadStatus::Closed);
}

TEST_CASE("a missing command is reported") {
  ChildProcess process;
  std::string error;
  REQUIRE_FALSE(process.start("bjorne_agent_no_such_command_12345", {}, {}, error));
  REQUIRE(error.find("bjorne_agent_no_such_command_12345") != std::string::npos);
  REQUIRE_FALSE(process.started());
}

#if !defined(_WIN32)
TEST_CASE("a command that is not a program is reported") {
  const std::filesystem::path directory =
      std::filesystem::temp_directory_path() /
      ("bjorne_process_" +
       std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(
                          std::chrono::system_clock::now().time_since_epoch())
                          .count()) +
       "-" + std::to_string(::getpid()));
  std::filesystem::create_directories(directory);

  // A directory and a file without the execute bit both exist, and neither can
  // be run as a program.
  ChildProcess asDirectory;
  std::string error;
  REQUIRE_FALSE(asDirectory.start(directory.string(), {}, {}, error));
  REQUIRE(error.find(directory.string()) != std::string::npos);
  REQUIRE_FALSE(asDirectory.started());

  const std::filesystem::path file = directory / "not-a-program";
  {
    std::ofstream stream(file);
    stream << "hello\n";
  }
  ChildProcess asFile;
  REQUIRE_FALSE(asFile.start(file.string(), {}, {}, error));
  REQUIRE(error.find(file.string()) != std::string::npos);
  REQUIRE(error.find("Cannot start MCP server") != std::string::npos);
  REQUIRE_FALSE(asFile.started());

  std::error_code code;
  std::filesystem::remove_all(directory, code);
}
#endif

TEST_CASE("a process cannot be started twice") {
  ChildProcess process;
  std::string error;
  REQUIRE(process.start(kEchoCommand, {}, {}, error));
  REQUIRE_FALSE(process.start(kEchoCommand, {}, {}, error));
  REQUIRE(error.find("already") != std::string::npos);
  process.stop();
}

TEST_CASE("the environment of the child carries the extra entries") {
#if defined(_WIN32)
  const char* kPrintCommand = "cmd.exe";
  const std::vector<std::string> arguments = {"/c", "echo", "%BJORNE_AGENT_TEST_VALUE%"};
#else
  const char* kPrintCommand = "sh";
  const std::vector<std::string> arguments = {"-c", "echo $BJORNE_AGENT_TEST_VALUE"};
#endif

  ChildProcess process;
  std::string error;
  REQUIRE(process.start(kPrintCommand, arguments, {{"BJORNE_AGENT_TEST_VALUE", "from-config"}}, error));

  std::string line;
  REQUIRE(process.readLine(2000, line, error) == ReadStatus::Line);
  REQUIRE(line.find("from-config") != std::string::npos);

  process.stop();
}

TEST_CASE("a write to a stopped process is refused") {
  ChildProcess process;
  std::string error;
  REQUIRE(process.start(kEchoCommand, {}, {}, error));
  process.stop();

  REQUIRE_FALSE(process.sendLine("hello", error));
  REQUIRE(error.find("not running") != std::string::npos);
}

TEST_CASE("the exit code of a finished process is reported") {
#if defined(_WIN32)
  const char* kExitCommand = "cmd.exe";
  const std::vector<std::string> arguments = {"/c", "exit 7"};
#else
  const char* kExitCommand = "sh";
  const std::vector<std::string> arguments = {"-c", "exit 7"};
#endif

  ChildProcess process;
  std::string error;
  REQUIRE(process.start(kExitCommand, arguments, {}, error));

  // The exit code is only known once the process has ended.
  for (int attempt = 0; attempt < 100 && process.running(); ++attempt) {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  REQUIRE(process.exitCode() == 7);
}

TEST_CASE("the exit code of a process that runs is not known") {
  ChildProcess process;
  REQUIRE(process.exitCode() == -1);
}
