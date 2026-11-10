#include "log/Log.h"

#include <string>

#include <catch_amalgamated.hpp>

#include "log/LogCapture.h"

using bjorne_agent_test::CaptureGuard;
using bjorne_agent_test::LevelGuard;

TEST_CASE("log level starts quiet") {
  // The library must not write to the console unless the binary asks it to.
  REQUIRE(bjorne_agent::log::level() == bjorne_agent::log::Level::Quiet);
}

TEST_CASE("log level parses the known names") {
  bjorne_agent::log::Level level = bjorne_agent::log::Level::Quiet;
  REQUIRE(bjorne_agent::log::parseLevel("quiet", level));
  REQUIRE(level == bjorne_agent::log::Level::Quiet);
  REQUIRE(bjorne_agent::log::parseLevel("normal", level));
  REQUIRE(level == bjorne_agent::log::Level::Normal);
  REQUIRE(bjorne_agent::log::parseLevel("verbose", level));
  REQUIRE(level == bjorne_agent::log::Level::Verbose);
}

TEST_CASE("log level rejects an unknown name") {
  bjorne_agent::log::Level level = bjorne_agent::log::Level::Normal;
  REQUIRE_FALSE(bjorne_agent::log::parseLevel("loud", level));
  REQUIRE(level == bjorne_agent::log::Level::Normal);
}

TEST_CASE("log level can be set and read back") {
  const LevelGuard guard(bjorne_agent::log::Level::Quiet);
  bjorne_agent::log::setLevel(bjorne_agent::log::Level::Verbose);
  REQUIRE(bjorne_agent::log::level() == bjorne_agent::log::Level::Verbose);
  bjorne_agent::log::setLevel(bjorne_agent::log::Level::Normal);
  REQUIRE(bjorne_agent::log::level() == bjorne_agent::log::Level::Normal);
}

TEST_CASE("progress output accepts a message at every level") {
  const LevelGuard guard(bjorne_agent::log::Level::Quiet);
  // The calls must not crash or block regardless of the level.
  bjorne_agent::log::progress("progress line");
  bjorne_agent::log::detail("detail line");
  bjorne_agent::log::fail("error line");
}

TEST_CASE("quiet level writes nothing to stderr") {
  const LevelGuard guard(bjorne_agent::log::Level::Quiet);
  const CaptureGuard capture;

  bjorne_agent::log::progress("progress line");
  bjorne_agent::log::detail("detail line");
  bjorne_agent::log::fail("error line");

  REQUIRE(capture.text().empty());
}

TEST_CASE("normal level writes progress and error lines to stderr") {
  const LevelGuard guard(bjorne_agent::log::Level::Quiet);
  const CaptureGuard capture;

  bjorne_agent::log::setLevel(bjorne_agent::log::Level::Normal);
  bjorne_agent::log::progress("progress line");
  bjorne_agent::log::fail("error line");

  const std::string text = capture.text();
  // The elapsed time prefix comes first, then the kind, then the message.
  REQUIRE(text.find("[bjorne_agent ") != std::string::npos);
  REQUIRE(text.find("info: progress line") != std::string::npos);
  REQUIRE(text.find("error: error line") != std::string::npos);
  // Detail lines need the verbose level, so they stay out.
  REQUIRE(text.find("detail line") == std::string::npos);
}

TEST_CASE("detail lines need the verbose level") {
  const LevelGuard guard(bjorne_agent::log::Level::Quiet);

  {
    const CaptureGuard capture;
    bjorne_agent::log::setLevel(bjorne_agent::log::Level::Normal);
    bjorne_agent::log::detail("detail line");
    REQUIRE(capture.text().empty());
  }

  const CaptureGuard capture;
  bjorne_agent::log::setLevel(bjorne_agent::log::Level::Verbose);
  bjorne_agent::log::detail("detail line");
  REQUIRE(capture.text().find("detail: detail line") != std::string::npos);
}

TEST_CASE("block output indents every line under its label") {
  const LevelGuard guard(bjorne_agent::log::Level::Normal);
  const CaptureGuard capture;

  bjorne_agent::log::block("thinking", "first line\nsecond line");

  const std::string text = capture.text();
  REQUIRE(text.find("thinking:\n  first line\n  second line\n") != std::string::npos);
}

TEST_CASE("block output ignores empty text") {
  const LevelGuard guard(bjorne_agent::log::Level::Normal);
  const CaptureGuard capture;

  bjorne_agent::log::block("thinking", "");

  REQUIRE(capture.text().empty());
}

TEST_CASE("block output is hidden below the normal level") {
  const LevelGuard guard(bjorne_agent::log::Level::Quiet);
  const CaptureGuard capture;

  bjorne_agent::log::block("thinking", "hidden");

  REQUIRE(capture.text().empty());
}

TEST_CASE("block output cuts long text and says how much was left out") {
  const LevelGuard guard(bjorne_agent::log::Level::Normal);
  const CaptureGuard capture;

  bjorne_agent::log::block("result", "abcdef", 3);

  const std::string text = capture.text();
  REQUIRE(text.find("  abc\n") != std::string::npos);
  REQUIRE(text.find("[3 more characters]") != std::string::npos);
}

TEST_CASE("block output drops the carriage return of a CRLF line") {
  const LevelGuard guard(bjorne_agent::log::Level::Normal);
  const CaptureGuard capture;

  bjorne_agent::log::block("result", "one\r\ntwo");

  const std::string text = capture.text();
  REQUIRE(text.find("  one\n  two\n") != std::string::npos);
  REQUIRE(text.find('\r') == std::string::npos);
}

TEST_CASE("seconds format as a short string") {
  REQUIRE(bjorne_agent::log::formatSeconds(0.0) == "0.0s");
  REQUIRE(bjorne_agent::log::formatSeconds(1.25) == "1.2s");
  REQUIRE(bjorne_agent::log::formatSeconds(12.0) == "12.0s");
}

TEST_CASE("elapsed time only moves forward") {
  const double first = bjorne_agent::log::elapsedSeconds();
  const double second = bjorne_agent::log::elapsedSeconds();
  REQUIRE(second >= first);
}
