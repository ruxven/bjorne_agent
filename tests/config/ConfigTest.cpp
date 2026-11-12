#include "config/Config.h"
#include "json/Json.h"

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

#include "platform/Platform.h"

#include <catch_amalgamated.hpp>

namespace {

// The path carries the process id so that two test binaries running at the
// same time do not overwrite each other's fixture.
const std::string kTempPath =
    (std::filesystem::temp_directory_path() /
     ("bjorne_agent_config_test_" + std::to_string(bjorne_agent::platform::processId()) + ".json"))
        .string();

void writeConfig(const std::string& content) {
  std::ofstream file(kTempPath);
  file << content;
}

bjorne_agent::config::Config loadConfig(const std::string& content, std::string& error) {
  writeConfig(content);
  bjorne_agent::config::Config config;
  if (!bjorne_agent::config::load(kTempPath, config, error)) {
    return config;
  }
  return config;
}

}  // namespace

TEST_CASE("Config loader accepts a valid config") {
  std::string error;
  bjorne_agent::config::Config config = loadConfig(
      R"({
        "version": 1,
        "agent": {
          "system_prompt": "You are a helper.",
          "workspace": "/tmp/workspace"
        },
        "models": [
          {
            "provider": "openai",
            "endpoint": "https://api.openai.com",
            "model": "gpt-4o",
            "context_window": 128000,
            "max_tokens": 4096,
            "temperature": 0.7,
            "api_key_env": "OPENAI_API_KEY",
            "priority": 1,
            "compression_prompt_path": "/tmp/bjorne_agent_compression.md"
          }
        ]
      })",
      error);

  REQUIRE(error.empty());
  REQUIRE(config.version == 1);
  REQUIRE(config.agent.system_prompt == "You are a helper.");
  REQUIRE(config.agent.workspace == "/tmp/workspace");
  REQUIRE(config.models.size() == 1);
  const bjorne_agent::config::ModelConfig& model = config.models.front();
  REQUIRE(model.provider == "openai");
  REQUIRE(model.endpoint == "https://api.openai.com");
  REQUIRE(model.model == "gpt-4o");
  REQUIRE(model.context_window == 128000);
  REQUIRE(model.max_tokens == 4096);
  REQUIRE(model.temperature == 0.7);
  REQUIRE(model.api_key_env == "OPENAI_API_KEY");
  REQUIRE(model.priority == 1);
  REQUIRE(model.compression_prompt_path == "/tmp/bjorne_agent_compression.md");
}

TEST_CASE("Config loader accepts a config without optional fields") {
  std::string error;
  bjorne_agent::config::Config config =
      loadConfig(R"({"version": 1, "agent": {"system_prompt": "Be brief"}})", error);

  REQUIRE(error.empty());
  REQUIRE(config.version == 1);
  REQUIRE(config.agent.system_prompt == "Be brief");
  REQUIRE(config.agent.workspace.empty());
  REQUIRE(config.models.empty());
}

TEST_CASE("Config loader rejects unknown version") {
  std::string error;
  bjorne_agent::config::Config config = loadConfig(R"({"version": 2})", error);
  REQUIRE_FALSE(config.version == 1);
  REQUIRE(!error.empty());
}

TEST_CASE("Config loader rejects missing version") {
  std::string error;
  bjorne_agent::config::Config config = loadConfig(R"({"agent": {"system_prompt": "Be brief"}})", error);
  REQUIRE(!error.empty());
}

TEST_CASE("Config loader rejects malformed JSON") {
  std::string error;
  bjorne_agent::config::Config config = loadConfig(R"({"version": 1,",)", error);
  REQUIRE(!error.empty());
}

TEST_CASE("Config loader rejects non object root") {
  std::string error;
  bjorne_agent::config::Config config = loadConfig(R"([1,2,3])", error);
  REQUIRE(!error.empty());
}

TEST_CASE("Config loader rejects missing file") {
  bjorne_agent::config::Config config;
  std::string error = "sentinel";
  const bool ok = bjorne_agent::config::load("/tmp/opencode/definitely_missing_bjorne_agent_config.json", config, error);
  REQUIRE_FALSE(ok);
  REQUIRE(!error.empty());
}

TEST_CASE("Config loader skips non object model entries and keeps numeric defaults") {
  std::string error;
  bjorne_agent::config::Config config = loadConfig(
      R"({
        "version": 1,
        "models": [
          "not-a-model",
          {"provider": "openai", "model": "gpt-4o", "priority": 2}
        ]
      })",
      error);

  REQUIRE(error.empty());
  REQUIRE(config.models.size() == 1);
  REQUIRE(config.models.front().provider == "openai");
  REQUIRE(config.models.front().context_window == 0);
  REQUIRE(config.models.front().max_tokens == 0);
  REQUIRE(config.models.front().temperature == 0.0);
}

TEST_CASE("Config loader reads the agent extension fields") {
  std::string error;
  bjorne_agent::config::Config config = loadConfig(
      R"({
        "version": 1,
        "agent": {
          "system_prompt": "Be brief",
          "workspace": "/workspace",
          "skills_path": "/opt/skills",
          "session_dir": "/sessions",
          "interactive": true
        }
      })",
      error);

  REQUIRE(error.empty());
  REQUIRE(config.agent.skills_path == "/opt/skills");
  REQUIRE(config.agent.session_dir == "/sessions");
  REQUIRE(config.agent.interactive);
}

TEST_CASE("Config loader defaults the agent extension fields") {
  std::string error;
  bjorne_agent::config::Config config = loadConfig(R"({"version": 1, "agent": {"system_prompt": "Be brief"}})", error);

  REQUIRE(error.empty());
  REQUIRE(config.agent.skills_path.empty());
  REQUIRE(config.agent.session_dir.empty());
  REQUIRE_FALSE(config.agent.interactive);
}

TEST_CASE("Config loader reads MCP servers") {
  std::string error;
  bjorne_agent::config::Config config = loadConfig(
      R"({
        "version": 1,
        "mcp_servers": {
          "filesystem": {
            "command": "mcp-filesystem",
            "args": ["--root", "/tmp", 7],
            "env": {"TOKEN": "abc", "BAD": 1}
          },
          "broken": "not-an-object"
        }
      })",
      error);

  REQUIRE(error.empty());
  REQUIRE(config.mcp_servers.size() == 1);
  const bjorne_agent::config::McpServerConfig& server = config.mcp_servers.at("filesystem");
  REQUIRE(server.command == "mcp-filesystem");
  REQUIRE(server.args == std::vector<std::string>{"--root", "/tmp"});
  REQUIRE(server.env.size() == 1);
  REQUIRE(server.env.at("TOKEN") == "abc");
}

TEST_CASE("Config loader ignores a non object mcp_servers field") {
  std::string error;
  bjorne_agent::config::Config config =
      loadConfig(R"({"version": 1, "mcp_servers": ["nope"]})", error);

  REQUIRE(error.empty());
  REQUIRE(config.mcp_servers.empty());
}
