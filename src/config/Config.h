#pragma once

#include <map>
#include <string>
#include <vector>

namespace bjorne_agent {
namespace config {

  struct ModelConfig {
  std::string provider;
  std::string endpoint;
  std::string model;
  long long context_window = 0;
  long long max_tokens = 0;
  double temperature = 0.0;
  std::string api_key_env;
  long long priority = 1;
  std::string compression_prompt_path;
  bool ignore_self_signed = false;
  std::string hostname;  // for SNI; empty = use host from endpoint
};

struct AgentConfig {
  std::string system_prompt;
  std::string workspace;
  std::string skills_path;
  std::string session_dir;
  bool interactive = false;
};

// One stdio MCP server. The command and its arguments are spawned as a child
// process; the environment entries are added to the inherited environment.
struct McpServerConfig {
  std::string command;
  std::vector<std::string> args;
  std::map<std::string, std::string> env;
};

struct Config {
  long long version = 0;
  AgentConfig agent;
  std::vector<ModelConfig> models;
  std::map<std::string, McpServerConfig> mcp_servers;
};

bool load(const std::string& path, Config& out, std::string& error);

}  // namespace config
}  // namespace bjorne_agent
