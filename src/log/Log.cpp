#include "log/Log.h"

#include <algorithm>
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

void block(const std::string& label, const std::string& text, std::size_t limit) {
  if (g_level < Level::Normal || text.empty()) {
    return;
  }
  std::cerr << "[bjorne_agent " << formatSeconds(elapsedSeconds()) << "] " << label << ":\n";
  const std::size_t shown = std::min(text.size(), limit);
  std::size_t pos = 0;
  while (pos < shown) {
    std::size_t lineEnd = text.find('\n', pos);
    if (lineEnd == std::string::npos || lineEnd > shown) {
      lineEnd = shown;
    }
    std::string line = text.substr(pos, lineEnd - pos);
    // A CRLF from a tool result would otherwise leave a stray carriage return
    // that overwrites the start of the line on a terminal.
    if (!line.empty() && line.back() == '\r') {
      line.pop_back();
    }
    std::cerr << "  " << line << "\n";
    pos = lineEnd + 1;
  }
  if (shown < text.size()) {
    std::cerr << "  ... [" << (text.size() - shown) << " more characters]\n";
  }
  std::cerr.flush();
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
