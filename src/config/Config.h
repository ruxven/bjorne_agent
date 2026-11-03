#pragma once

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
};

struct Config {
  long long version = 0;
  AgentConfig agent;
  std::vector<ModelConfig> models;
};

bool load(const std::string& path, Config& out, std::string& error);

}  // namespace config
}  // namespace bjorne_agent
