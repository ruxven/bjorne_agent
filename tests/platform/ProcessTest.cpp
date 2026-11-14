#include "platform/Process.h"

#include <string>
#include <thread>
#include <vector>

#include <catch_amalgamated.hpp>

using bjorne_agent::platform::ChildProcess;
using bjorne_agent::platform::ReadStatus;

namespace {

// The echo helper differs per platform: POSIX has cat, Windows has the shell.
#if defined(_WIN32)
const char* kEchoCommand = "cmd.exe";
#else
const char* kEchoCommand = "cat";
#endif

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
