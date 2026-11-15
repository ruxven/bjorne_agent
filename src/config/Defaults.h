#pragma once

#include <string>

namespace bjorne_agent {
namespace config {

// The system prompt a new configuration starts with. The init command writes
// it into a file next to the configuration, where the user can edit it.
constexpr const char* kDefaultSystemPrompt = "You are a coding agent. Help the user with their task.";

// The compression instruction a new configuration starts with. The binary
// carries the text, so the package holds the executable and nothing else. The
// init command writes the text into a file next to the configuration, and the
// configuration points at that file, so the user edits a copy rather than a
// file the package owns.
std::string defaultCompressionPrompt();

}  // namespace config
}  // namespace bjorne_agent
