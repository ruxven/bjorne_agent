#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <functional>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include "agent/AgentLoop.h"
#include "cli/Invocation.h"
#include "config/Config.h"
#include "config/ConfigPaths.h"
#include "context/AgentsFiles.h"
#include "context/ContextManager.h"
#include "context/Fragments.h"
#include "log/Log.h"
#include "model/ModelClient.h"
#include "platform/Platform.h"
#include "skills/Skills.h"
#include "tools/Tools.h"

namespace {

std::string readStdin() {
  std::stringstream buffer;
  buffer << std::cin.rdbuf();
  return buffer.str();
}

bool readTextFile(const std::string& path, std::string& out) {
  std::ifstream file(path);
  if (!file.is_open()) {
    return false;
  }
  std::stringstream buffer;
  buffer << file.rdbuf();
  out = buffer.str();
  return true;
}

// Windows accepts either separator; POSIX has only the forward slash, and a
// backslash is an ordinary character in a POSIX file name.
bool isPathSeparator(char c) {
#if defined(_WIN32)
  return c == '/' || c == '\\';
#else
  return c == '/';
#endif
}

// True for a path that starts at the root: /dir on POSIX, C:\dir or \\server
// on Windows.
bool isAbsolutePath(const std::string& path) {
  if (path.empty()) {
    return false;
  }
  if (isPathSeparator(path.front())) {
    return true;
  }
#if defined(_WIN32)
  if (path.size() >= 3 && std::isalpha(static_cast<unsigned char>(path[0])) != 0 && path[1] == ':' &&
      isPathSeparator(path[2])) {
    return true;
  }
#endif
  return false;
}

size_t lastPathSeparator(const std::string& path) {
#if defined(_WIN32)
  return path.find_last_of("/\\");
#else
  return path.find_last_of('/');
#endif
}

std::string resolveAgainstConfig(const std::string& configPath, const std::string& path) {
  if (path.empty() || isAbsolutePath(path)) {
    return path;
  }
  const size_t slash = lastPathSeparator(configPath);
  if (slash == std::string::npos) {
    return path;
  }
  return configPath.substr(0, slash + 1) + path;
}

const bjorne_agent::config::ModelConfig* selectModel(const std::vector<bjorne_agent::config::ModelConfig>& models) {
  if (models.empty()) {
    return nullptr;
  }
  const auto best = std::min_element(models.begin(), models.end(),
                                     [](const bjorne_agent::config::ModelConfig& left, const bjorne_agent::config::ModelConfig& right) {
                                       return left.priority < right.priority;
                                     });
  return &*best;
}

// Reads BJORNE_AGENT_LOG into the level. Returns false when the variable holds
// an unknown level.
bool applyLogEnvironment(bjorne_agent::log::Level& level) {
  std::string configured;
  if (!bjorne_agent::platform::getEnvironmentVariable("BJORNE_AGENT_LOG", configured)) {
    return true;
  }
  if (!bjorne_agent::log::parseLevel(configured, level)) {
    std::cerr << "Unknown BJORNE_AGENT_LOG level: " << configured << "\n";
    return false;
  }
  return true;
}

// Fills in the context configuration and loads the compression prompt. The
// prompt path is relative to the config file, so it is resolved against the
// config path rather than the working directory.
bool buildContextConfig(const std::string& configPath, const bjorne_agent::config::ModelConfig& model,
                        bjorne_agent::context::ContextManagerConfig& contextConfig) {
  contextConfig.context_window = model.context_window;
  contextConfig.max_tokens = model.max_tokens;
  if (model.compression_prompt_path.empty()) {
    return true;
  }

  std::string prompt;
  const std::string promptPath = resolveAgainstConfig(configPath, model.compression_prompt_path);
  bjorne_agent::log::detail("Compression prompt: " + promptPath);
  if (!readTextFile(promptPath, prompt)) {
    const std::string message = "Cannot open compression prompt: " + promptPath;
    bjorne_agent::log::fail(message);
    std::cerr << message << "\n";
    return false;
  }
  contextConfig.compression_prompt = prompt;
  return true;
}

// Builds the callback that asks the model to summarize a conversation when
// the context no longer fits.
bjorne_agent::context::CompressionHandler makeCompressionHandler(const std::shared_ptr<bjorne_agent::model::ModelClient>& model) {
  return [model](const std::vector<bjorne_agent::model::ModelMessage>& messages, const std::string& instruction,
                 std::string& summary, std::string& handlerError) {
    std::vector<bjorne_agent::model::ModelMessage> request;
    bjorne_agent::model::ModelMessage system;
    system.role = "system";
    system.content = instruction;
    request.push_back(system);
    request.insert(request.end(), messages.begin(), messages.end());
    bjorne_agent::model::ModelResponse response;
    bjorne_agent::log::progress("Compressing context");
    if (!model->respond(request, response, handlerError)) {
      return false;
    }
    summary = response.content;
    return true;
  };
}

// Builds the system prompt from the configured text and the assembled
// fragments. Skills come first and the nearest AGENTS.md last, so the most
// specific instruction is the last thing the model reads.
std::string buildSystemPrompt(const std::string& base, const std::vector<bjorne_agent::context::Fragment>& fragments) {
  std::string prompt = base;
  const std::string assembled = bjorne_agent::context::renderFragments(fragments);
  if (assembled.empty()) {
    return prompt;
  }
  if (!prompt.empty()) {
    prompt += "\n\n";
  }
  prompt += assembled;
  return prompt;
}

// Runs one task: loads the config, builds the model, context and tools, and
// prints the final answer. Returns 0 on success and 1 after reporting an error.
int runTask(const bjorne_agent::cli::Invocation& invocation) {
  std::string error;
  const std::string& configPath = invocation.configPath;
  bjorne_agent::log::progress("Loading config: " + configPath);

  bjorne_agent::config::Config config;
  if (!bjorne_agent::config::load(configPath, config, error)) {
    bjorne_agent::log::fail(error);
    std::cerr << error << "\n";
    return 1;
  }

  const bjorne_agent::config::ModelConfig* selected = selectModel(config.models);
  if (selected == nullptr) {
    bjorne_agent::log::fail("Config has no models");
    std::cerr << "Config has no models\n";
    return 1;
  }
  bjorne_agent::log::progress("Selected model: " + selected->model + " at " + selected->endpoint);
  bjorne_agent::log::detail("API key comes from environment variable: " + selected->api_key_env);
  bjorne_agent::log::detail("Workspace: " + config.agent.workspace);
  if (selected->ignore_self_signed) {
    bjorne_agent::log::progress("TLS peer verification is off (ignore_self_signed is set)");
  }
  if (!selected->hostname.empty()) {
    bjorne_agent::log::detail("TLS server name: " + selected->hostname);
  }

  // The task comes from the command line when one was given, otherwise from
  // standard input.
  std::string task = invocation.taskFromArgument ? invocation.task : readStdin();
  if (task.empty()) {
    std::cerr << bjorne_agent::cli::usage();
    return 1;
  }
  bjorne_agent::log::progress("Task: " + task);

  bjorne_agent::context::ContextManagerConfig contextConfig;
  if (!buildContextConfig(configPath, *selected, contextConfig)) {
    return 1;
  }

  // Skills and AGENTS.md are assembled before the model client, because the
  // system prompt they produce is part of the request and load_skill has to be
  // in the tool set from the first turn.
  const std::string skillsDirectory = resolveAgainstConfig(configPath, config.agent.skills_path);
  const bjorne_agent::skills::SkillLoad skillLoad = bjorne_agent::skills::loadSkills(skillsDirectory);
  if (!skillsDirectory.empty() && !skillLoad.directory_found) {
    bjorne_agent::log::fail("Skills directory not found: " + skillsDirectory);
  }
  for (const std::string& skipped : skillLoad.skipped) {
    bjorne_agent::log::fail("Skipping skill " + skipped);
  }
  for (const bjorne_agent::skills::Skill& skill : skillLoad.skills) {
    bjorne_agent::log::detail("Skill: " + skill.name);
  }

  const bjorne_agent::context::AgentsFiles agents =
      bjorne_agent::context::loadAgentsFiles(config.agent.workspace, bjorne_agent::config::userConfigDirectory());
  for (const std::string& path : agents.unreadable) {
    bjorne_agent::log::fail("Cannot read " + path);
  }
  for (const bjorne_agent::context::Fragment& fragment : agents.fragments) {
    bjorne_agent::log::detail("Context: " + fragment.title);
  }

  std::vector<bjorne_agent::context::Fragment> fragments;
  if (!skillLoad.skills.empty()) {
    fragments.push_back(bjorne_agent::skills::skillIndexFragment(skillLoad.skills));
  }
  fragments.insert(fragments.end(), agents.fragments.begin(), agents.fragments.end());

  bjorne_agent::model::ModelClientOptions modelOptions;
  modelOptions.endpoint = selected->endpoint;
  modelOptions.model = selected->model;
  modelOptions.api_key_env = selected->api_key_env;
  modelOptions.ignore_self_signed = selected->ignore_self_signed;
  modelOptions.hostname = selected->hostname;
  auto model = std::make_shared<bjorne_agent::model::ModelClient>(std::move(modelOptions));

  auto context = std::make_shared<bjorne_agent::context::ContextManager>(contextConfig, makeCompressionHandler(model));
  auto tools = std::make_shared<bjorne_agent::tools::ToolRegistry>(config.agent.workspace);
  if (!skillLoad.skills.empty()) {
    auto skillCatalog = std::make_shared<std::vector<bjorne_agent::skills::Skill>>(skillLoad.skills);
    bjorne_agent::skills::registerLoadSkillTool(*tools, skillCatalog);
  }
  model->setTools(tools->definitionsJson());

  bjorne_agent::agent::AgentLoopConfig loopConfig;
  loopConfig.system_prompt = buildSystemPrompt(config.agent.system_prompt, fragments);
  loopConfig.workspace = config.agent.workspace;
  bjorne_agent::agent::AgentLoop loop(loopConfig, model, context, tools);

  std::string result;
  if (!loop.run(task, result, error)) {
    bjorne_agent::log::fail(error);
    std::cerr << error << "\n";
    return 1;
  }
  bjorne_agent::log::progress("Done");
  std::cout << result << "\n";
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  std::vector<std::string> arguments;
  for (int index = 1; index < argc; ++index) {
    arguments.emplace_back(argv[index]);
  }

  bjorne_agent::cli::Invocation invocation;
  std::string error;
  if (!bjorne_agent::cli::parse(arguments, invocation, error)) {
    bjorne_agent::log::fail(error);
    std::cerr << error << "\n";
    if (invocation.showUsage) {
      std::cerr << "\n" << bjorne_agent::cli::usage();
    }
    return 1;
  }

  if (invocation.help) {
    if (invocation.commandSet) {
      std::cout << bjorne_agent::cli::usage(invocation.command);
    } else {
      std::cout << bjorne_agent::cli::usage();
    }
    return 0;
  }

  // A command line flag wins over the environment variable. The library starts
  // quiet so that embedding it produces no output; the binary defaults to
  // normal so the user sees what it is doing.
  bjorne_agent::log::Level level = invocation.levelSet ? invocation.level : bjorne_agent::log::Level::Normal;
  if (!invocation.levelSet && !applyLogEnvironment(level)) {
    return 1;
  }
  bjorne_agent::log::setLevel(level);

  if (invocation.command != bjorne_agent::cli::Command::Run) {
    std::cerr << "The '" << bjorne_agent::cli::commandName(invocation.command) << "' command is not implemented yet.\n";
    return 1;
  }

  return runTask(invocation);
}
