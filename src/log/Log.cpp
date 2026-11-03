#include "log/Log.h"

#include <chrono>
#include <cstdio>
#include <iostream>

namespace bjorne_agent {
namespace log {

namespace {

Level g_level = Level::Quiet;

std::chrono::steady_clock::time_point g_started = std::chrono::steady_clock::now();

// One line, prefixed with the elapsed time so a stalled run shows how long it
// has been waiting at the point where it stopped.
void write(const char* kind, const std::string& message) {
  std::cerr << "[bjorne_agent " << formatSeconds(elapsedSeconds()) << "] " << kind << ": " << message << "\n";
  std::cerr.flush();
}

}  // namespace

void setLevel(Level level) {
  g_level = level;
}

Level level() {
  return g_level;
}

bool parseLevel(const std::string& name, Level& out) {
  if (name == "quiet") {
    out = Level::Quiet;
    return true;
  }
  if (name == "normal") {
    out = Level::Normal;
    return true;
  }
  if (name == "verbose") {
    out = Level::Verbose;
    return true;
  }
  return false;
}

void progress(const std::string& message) {
  if (g_level < Level::Normal) {
    return;
  }
  write("info", message);
}

void detail(const std::string& message) {
  if (g_level < Level::Verbose) {
    return;
  }
  write("detail", message);
}

void fail(const std::string& message) {
  if (g_level < Level::Normal) {
    return;
  }
  write("error", message);
}

double elapsedSeconds() {
  const auto now = std::chrono::steady_clock::now();
  return std::chrono::duration<double>(now - g_started).count();
}

std::string formatSeconds(double seconds) {
  char buffer[32];
  const int written = std::snprintf(buffer, sizeof(buffer), "%.1fs", seconds);
  if (written < 0 || static_cast<size_t>(written) >= sizeof(buffer)) {
    return "unknown";
  }
  return buffer;
}

}  // namespace log
}  // namespace bjorne_agent
