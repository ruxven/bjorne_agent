#pragma once

#include <cstddef>
#include <string>

namespace bjorne_agent {
namespace log {

// Progress output levels. The library starts at Quiet so that embedding bjorne_agent
// as a library produces no output at all. The binary raises the level at
// startup, and the user selects it with --verbose, --quiet, or BJORNE_AGENT_LOG.
enum class Level { Quiet = 0, Normal = 1, Verbose = 2 };

// Sets the process wide level. Threads other than the caller see the change
// on their next log call.
void setLevel(Level level);

// Current process wide level.
Level level();

// Parses a level name: quiet, normal, or verbose. Returns false for any other
// name, so the caller can report the valid choices.
bool parseLevel(const std::string& name, Level& out);

// Progress line. Shown at Normal and Verbose. Written to stderr so stdout
// stays clean for the final answer.
void progress(const std::string& message);

// Detail line. Shown only at Verbose.
void detail(const std::string& message);

// Error line. Shown at Normal and Verbose. Errors that end the run are also
// printed by the caller, so this reports the ones that do not stop the run.
void fail(const std::string& message);

// A labelled block of text, such as the model's own words or a tool payload.
// The label goes on its own line and every text line is indented under it, so
// a multi-line answer stays readable next to the progress lines. Shown at
// Normal and Verbose; empty text is ignored. Text longer than `limit` is cut,
// and a final line says how many characters were left out.
void block(const std::string& label, const std::string& text, std::size_t limit = 4000);

// Seconds since the first log call. Used to stamp lines so a stalled run
// shows where it stopped.
double elapsedSeconds();

// Formats a duration as a short string, for example 1.2s.
std::string formatSeconds(double seconds);

}  // namespace log
}  // namespace bjorne_agent
