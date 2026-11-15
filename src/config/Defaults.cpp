#include "config/Defaults.h"

namespace bjorne_agent {
namespace config {

std::string defaultCompressionPrompt() {
  return "Summarize the conversation so far. Keep decisions, facts, file paths, and open tasks. "
         "Drop greetings, repetition, and commentary about the summarization itself.\n";
}

}  // namespace config
}  // namespace bjorne_agent
