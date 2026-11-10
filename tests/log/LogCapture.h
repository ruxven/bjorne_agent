#pragma once

#include <iostream>
#include <sstream>
#include <string>

#include "log/Log.h"

namespace bjorne_agent_test {

// Sets a log level for the lifetime of the guard and restores the previous
// one, so one test cannot change what the next test sees.
class LevelGuard {
 public:
  explicit LevelGuard(bjorne_agent::log::Level level) : previous_(bjorne_agent::log::level()) {
    bjorne_agent::log::setLevel(level);
  }
  ~LevelGuard() { bjorne_agent::log::setLevel(previous_); }

  LevelGuard(const LevelGuard&) = delete;
  LevelGuard& operator=(const LevelGuard&) = delete;

 private:
  bjorne_agent::log::Level previous_;
};

// Redirects std::cerr into a buffer for as long as the guard lives, so a test
// can read back what the log wrote. The stream is restored on the way out,
// even when the test fails part way through.
class CaptureGuard {
 public:
  CaptureGuard() : original_(std::cerr.rdbuf(buffer_.rdbuf())) {}
  ~CaptureGuard() { std::cerr.rdbuf(original_); }

  CaptureGuard(const CaptureGuard&) = delete;
  CaptureGuard& operator=(const CaptureGuard&) = delete;

  std::string text() const { return buffer_.str(); }

 private:
  std::ostringstream buffer_;
  std::streambuf* original_;
};

}  // namespace bjorne_agent_test
