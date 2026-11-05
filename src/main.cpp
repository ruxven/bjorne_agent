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
#include "context/ContextManager.h"
#include "log/Log.h"
#include "model/ModelClient.h"
#include "platform/Platform.h"
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

void usage() {
  std::cerr << "Usage: bjorne_agent [--verbose|--quiet] [config] [task]\n"
            << "  --verbose  report each step of the run on stderr\n"
            << "  --quiet    suppress progress output\n"
            << "  BJORNE_AGENT_LOG=quiet|normal|verbose sets the same level\n"
            << "Without a config path, the run uses BJORNE_AGENT_CONFIG when it is set,\n"
            << "then searches ./.bjorne_agent/bjorne_agent.json and\n"
            << "$XDG_CONFIG_HOME/bjorne_agent/bjorne_agent.json, in that order.\n";
}

// Parses the leading flags. The remaining arguments are the config path and
// the optional task. The requested level is returned through `level` so the
// caller can decide the precedence between flags and the environment.
int parseFlags(int argc, char** argv, int& first, bjorne_agent::log::Level& level, bool& levelSet) {
  first = 1;
  while (first < argc) {
    const std::string argument = argv[first];
    if (argument == "--verbose" || argument == "-v") {
      level = bjorne_agent::log::Level::Verbose;
      levelSet = true;
    } else if (argument == "--quiet" || argument == "-q") {
      level = bjorne_agent::log::Level::Quiet;
      levelSet = true;
    } else if (argument == "--help" || argument == "-h") {
      usage();
      return 2;
    } else {
      break;
    }
    ++first;
  }
  return 0;
}

// Reads BJORNE_AGENT_LOG. Returns false when the variable holds an unknown level.
bool applyLogEnvironment(bjorne_agent::log::Level& level, bool& levelSet) {
  std::string configured;
  if (!bjorne_agent::platform::getEnvironmentVariable("BJORNE_AGENT_LOG", configured)) {
    return true;
  }
  if (!bjorne_agent::log::parseLevel(configured, level)) {
    std::cerr << "Unknown BJORNE_AGENT_LOG level: " << configured << "\n";
    return false;
  }
  levelSet = true;
  return true;
}

// Resolves the log level from the flags and the environment, then applies it.
// A command line flag wins over the environment variable. Returns 0 to
// continue, 2 when help was printed, and 1 when the environment holds an
// unknown level.
int resolveLogLevel(int argc, char** argv, int& first, bjorne_agent::log::Level& level) {
  bool flagSet = false;
  const int flagStatus = parseFlags(argc, argv, first, level, flagSet);
  if (flagStatus == 2) {
    return 2;
  }

  bjorne_agent::log::Level environmentLevel = bjorne_agent::log::Level::Normal;
  bool environmentSet = false;
  if (!applyLogEnvironment(environmentLevel, environmentSet)) {
    return 1;
  }
  if (environmentSet && !flagSet) {
    level = environmentLevel;
  }

  // The library starts quiet so that embedding it produces no output. The
  // binary wants the user to see what it is doing, so it defaults to normal.
  bjorne_agent::log::setLevel(level);
  return 0;
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

// The positional arguments, in order, without the leading flags.
std::vector<std::string> positionalArguments(int argc, char** argv, int first) {
  std::vector<std::string> arguments;
  for (int index = first; index < argc; ++index) {
    arguments.emplace_back(argv[index]);
  }
  return arguments;
}

}  // namespace

int main(int argc, char** argv) {
  int first = 1;
  bjorne_agent::log::Level level = bjorne_agent::log::Level::Normal;
  const int logStatus = resolveLogLevel(argc, argv, first, level);
  if (logStatus == 2) {
    return 0;
  }
  if (logStatus != 0) {
    return 1;
  }

  bjorne_agent::cli::Invocation invocation;
  std::string error;
  if (!bjorne_agent::cli::resolveInvocation(positionalArguments(argc, argv, first), invocation, error)) {
    bjorne_agent::log::fail(error);
    std::cerr << error << "\n";
    return 1;
  }
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
    usage();
    return 1;
  }
  bjorne_agent::log::progress("Task: " + task);

  bjorne_agent::context::ContextManagerConfig contextConfig;
  if (!buildContextConfig(configPath, *selected, contextConfig)) {
    return 1;
  }

  bjorne_agent::model::ModelClientOptions modelOptions;
  modelOptions.endpoint = selected->endpoint;
  modelOptions.model = selected->model;
  modelOptions.api_key_env = selected->api_key_env;
  modelOptions.ignore_self_signed = selected->ignore_self_signed;
  modelOptions.hostname = selected->hostname;
  auto model = std::make_shared<bjorne_agent::model::ModelClient>(std::move(modelOptions));
  model->setTools(bjorne_agent::tools::ToolRegistry::definitionsJson());

  auto context = std::make_shared<bjorne_agent::context::ContextManager>(contextConfig, makeCompressionHandler(model));
  auto tools = std::make_shared<bjorne_agent::tools::ToolRegistry>(config.agent.workspace);

  bjorne_agent::agent::AgentLoopConfig loopConfig;
  loopConfig.system_prompt = config.agent.system_prompt;
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
