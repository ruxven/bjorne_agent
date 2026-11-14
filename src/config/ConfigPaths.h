#pragma once

#include <string>
#include <vector>

namespace bjorne_agent {
namespace config {

// The config path named by the BJORNE_AGENT_CONFIG environment variable. Returns an
// empty string when the variable is unset or blank.
//
// This is the escape hatch for a container, where the config is mounted at a
// path the search does not know about. An explicit config path on the command
// line still wins over it.
std::string environmentConfigPath();

// The user level config directory, for example ~/.config/bjorne_agent. Empty
// when no home directory is available. The context fragment search uses it to
// find the global AGENTS.md.
std::string userConfigDirectory();

// The config locations bjorne_agent looks at when the command line names no config
// file. The project local file wins over the user level file, so a repository
// can carry its own settings without touching the user configuration.
std::vector<std::string> defaultConfigPaths();

// The first path from defaultConfigPaths that exists on disk. Returns an empty
// string when none of them exists. On failure error explains where the search
// looked and how to fix it.
std::string findDefaultConfig(std::string& error);

}  // namespace config
}  // namespace bjorne_agent
