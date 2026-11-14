#pragma once

#include <string>
#include <vector>

#include "context/Fragments.h"

namespace bjorne_agent {
namespace context {

// The AGENTS.md files that apply to a run, already read into fragments.
struct AgentsFiles {
  std::vector<Fragment> fragments;
  std::vector<std::string> unreadable;
};

// Searches the global AGENTS.md in `global_config_directory`, then the
// workspace and every parent directory. The order puts the global guidance
// first and the most specific guidance last. A file that appears in the search
// but cannot be read is reported in `unreadable` rather than failing the run.
AgentsFiles loadAgentsFiles(const std::string& workspace, const std::string& global_config_directory);

}  // namespace context
}  // namespace bjorne_agent
